#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>
#include <arch/bcm2712/mmc.h>

typedef uint32_t lbaint_t;
struct mmc _mmc;
struct mmc_config _config;

extern struct bus_ops* bcm2712_sdhci_init(void);

int mmc_send_cmd(struct mmc *mmc, struct mmc_cmd *cmd, struct mmc_data *data){
    (void)mmc;
    int ret = mmc->ops->send_command(cmd, data);
    return ret;
}

static int mmc_send_cmd_retry(struct mmc *mmc, struct mmc_cmd *cmd,
                  struct mmc_data *data, uint32_t retries)
{
    int ret;

    do {
        ret = mmc_send_cmd(mmc, cmd, data);
    } while (ret && retries--);

    return ret;
}

int mmc_send_status(struct mmc *mmc, unsigned int *status)
{
    struct mmc_cmd cmd;
    int ret;

    cmd.cmdidx = MMC_CMD_SEND_STATUS;
    cmd.resp_type = MMC_RSP_R1;
    if (!mmc_host_is_spi(mmc))
        cmd.cmdarg = mmc->rca << 16;

    ret = mmc_send_cmd_retry(mmc, &cmd, NULL, 4);
    if (!ret)
        *status = cmd.response[0];

    return ret;
}

int mmc_poll_for_busy(struct mmc *mmc, int timeout_ms)
{
    unsigned int status;
    int err;

    while (1) {
        err = mmc_send_status(mmc, &status);
        if (err)
            return err;

        /* Return success right here: the old shared break + trailing
         * "timeout_ms <= 0" check misreported a card that became
         * ready on the very last poll iteration as a timeout. */
        if ((status & MMC_STATUS_RDY_FOR_DATA) &&
            (status & MMC_STATUS_CURR_STATE) !=
             MMC_STATE_PRG)
            return 0;

        if (status & MMC_STATUS_MASK) {
            return -ECOMM;
        }

        if (timeout_ms-- <= 0)
            return -ETIMEDOUT;

        usleep(1000);
    }
}

int mmc_send_stop_transmission(struct mmc *mmc, int write)
{
    struct mmc_cmd cmd;

    cmd.cmdidx = MMC_CMD_STOP_TRANSMISSION;
    cmd.cmdarg = 0;
    /* Keep BCM2712's userspace SD path aligned with the kernel-side
     * bcm2712 implementation: SD uses the R1b form for CMD12 and the
     * SDHCI_QUIRK_BROKEN_R1B workaround in sdhci.c handles the missing
     * INT_RESPONSE case. This makes the normal multi-block stop and the
     * recovery stop go through the same command semantics. */
    cmd.resp_type = (IS_SD(mmc) || write) ? MMC_RSP_R1b : MMC_RSP_R1;

    return mmc_send_cmd(mmc, &cmd, NULL);
}

static int mmc_go_idle(struct mmc *mmc)
{
    struct mmc_cmd cmd;
    int err;
    usleep(1000);

    cmd.cmdidx = MMC_CMD_GO_IDLE_STATE;
    cmd.cmdarg = 0;
    cmd.resp_type = MMC_RSP_NONE;

    err = mmc_send_cmd(mmc, &cmd, NULL);

    if (err)
        return err;

    usleep(2000);

    return 0;
}

static int mmc_send_if_cond(struct mmc *mmc)
{
    struct mmc_cmd cmd;
    int err;

    cmd.cmdidx = SD_CMD_SEND_IF_COND;
    cmd.cmdarg = ((mmc->cfg->voltages & 0xff8000) != 0) << 8 | 0xaa;
    cmd.resp_type = MMC_RSP_R7;

    err = mmc_send_cmd(mmc, &cmd, NULL);

    if (err)
        return err;

    if ((cmd.response[0] & 0xff) != 0xaa)
        return -EOPNOTSUPP;
    else
        mmc->version = SD_VERSION_2;

    return 0;
}

static int mmc_send_op_cond_iter(struct mmc *mmc, int use_arg)
{
    struct mmc_cmd cmd;
    int err;
    cmd.cmdidx = MMC_CMD_SEND_OP_COND;
    cmd.resp_type = MMC_RSP_R3;
    cmd.cmdarg = 0;
    if (use_arg)
        cmd.cmdarg = OCR_HCS |
            (mmc->cfg->voltages &
            (mmc->ocr & OCR_VOLTAGE_MASK)) |
            (mmc->ocr & OCR_ACCESS_MODE);

    err = mmc_send_cmd(mmc, &cmd, NULL);
    if (err)
        return err;
    mmc->ocr = cmd.response[0];
    return 0;
}

static int sd_send_op_cond(struct mmc *mmc, bool uhs_en)
{
    int timeout = 1000;
    int err;
    struct mmc_cmd cmd;

    while (1) {
        cmd.cmdidx = MMC_CMD_APP_CMD;
        cmd.resp_type = MMC_RSP_R1;
        cmd.cmdarg = 0;

        err = mmc_send_cmd(mmc, &cmd, NULL);

        if (err)
            return err;

        cmd.cmdidx = SD_CMD_APP_SEND_OP_COND;
        cmd.resp_type = MMC_RSP_R3;

        cmd.cmdarg = (mmc->cfg->voltages & 0xff8000);

        if (mmc->version == SD_VERSION_2)
            cmd.cmdarg |= OCR_HCS;

        if (uhs_en)
            cmd.cmdarg |= OCR_S18R;

        err = mmc_send_cmd(mmc, &cmd, NULL);

        if (err)
            return err;

        if (cmd.response[0] & OCR_BUSY)
            break;

        if (timeout-- <= 0)
            return -EOPNOTSUPP;

        usleep(1000);
    }

    if (mmc->version != SD_VERSION_2)
        mmc->version = SD_VERSION_1_0;

    mmc->ocr = cmd.response[0];
    mmc->high_capacity = ((mmc->ocr & OCR_HCS) == OCR_HCS);
    mmc->rca = 0;
    return 0;
}

void mmc_set_initial_state(struct mmc *mmc)
{
    mmc->ops->set_ios(mmc);
}

static int mmc_send_op_cond(struct mmc *mmc)
{
    int err, i;
    uint32_t timeout = 1000000;
    uint32_t retry_count = 0;

    mmc_go_idle(mmc);

    for (i = 0; ; i++) {
        err = mmc_send_op_cond_iter(mmc, i != 0);
        if (err)
            return err;

        if (mmc->ocr & OCR_BUSY)
            break;

        if (retry_count > timeout)
            return -ETIMEDOUT;
        usleep(1000);
    }
    mmc->op_cond_pending = 1;
    return 0;
}

int mmc_get_op_cond(struct mmc *mmc)
{
        /*
         * Raspi5 currently does not implement the full SD UHS 1.8V switch
         * sequence (CMD11 + host/card voltage transition). Advertising S18R
         * and then forcing SDR25 leaves some cards in an unstable state.
         * Stay on the proven 3.3V path until the full switch flow exists.
         */
        bool uhs_en = false;
    int err;

    if (mmc->has_init)
        return 0;

retry:
    mmc_set_initial_state(mmc);

    err = mmc_go_idle(mmc);
    if (err)
        return err;

    err = mmc_send_if_cond(mmc);

    err = sd_send_op_cond(mmc, uhs_en);
    if (err && uhs_en) {
        uhs_en = false;
        goto retry;
    }

    if (err == -ETIMEDOUT) {
        err = mmc_send_op_cond(mmc);
        if (err) {
            printf("Card did not respond to voltage select! : %d\n", err);
            return -EOPNOTSUPP;
        }
    }

    return err;
}

static const int fbase[] = {
    10000,
    100000,
    1000000,
    10000000,
};

static const uint8_t multipliers[] = {
    0,	/* reserved */
    10, 12, 13, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 70, 80,
};

int mmc_set_blocklen(struct mmc *mmc, int len)
{
    struct mmc_cmd cmd;

    cmd.cmdidx = MMC_CMD_SET_BLOCKLEN;
    cmd.resp_type = MMC_RSP_R1;
    cmd.cmdarg = len;

    return mmc_send_cmd(mmc, &cmd, NULL);
}

static int sd_select_bus_width(struct mmc *mmc, int w)
{
    int err;
    struct mmc_cmd cmd;

    if ((w != 4) && (w != 1))
        return -EINVAL;

    cmd.cmdidx = MMC_CMD_APP_CMD;
    cmd.resp_type = MMC_RSP_R1;
    cmd.cmdarg = mmc->rca << 16;

    err = mmc_send_cmd(mmc, &cmd, NULL);
    if (err)
        return err;

    cmd.cmdidx = SD_CMD_APP_SET_BUS_WIDTH;
    cmd.resp_type = MMC_RSP_R1;
    if (w == 4)
        cmd.cmdarg = 2;
    else if (w == 1)
        cmd.cmdarg = 0;
    err = mmc_send_cmd(mmc, &cmd, NULL);
    if (err)
        return err;

    return 0;
}

static int mmc_switch(struct mmc *mmc, int mode)
{
    struct mmc_cmd cmd;
    struct mmc_data data;
    uint8_t buf[64];
    int target_mode = mode;
    uint8_t mode_bit = 0;
    int clock = 0;
    int retry = 3;

    switch(mode){
        case UHS_SDR104:
            mode = 0x03;
            mode_bit = 0x8;
            clock = 208000000;
            break;
        case UHS_SDR50:
            mode = 0x02;
            mode_bit = 0x4;
            clock = 100000000;
            break;
        case UHS_SDR25:
            mode = 0x01;
            mode_bit = 0x2;
            clock = 50000000;
            break;
        default:
            return -EINVAL;
    }

    data.dest = (char*)buf;
    data.blocks = 1;
    data.blocksize = 64;
    data.flags = MMC_DATA_READ;

    cmd.cmdidx = MMC_CMD_SWITCH;
    cmd.resp_type = MMC_RSP_R1b;
    cmd.cmdarg = 0x0;

    int ret = mmc_send_cmd(mmc, &cmd, &data);
    if (ret)
        return ret;

    if (!(buf[13] & mode_bit)) {
        printf("mmc_switch: mode %d failed\n", mode);
        return -EINVAL;
    }

    if ((buf[16] & 0xf) == (unsigned)mode) {
        mmc->selected_mode = target_mode;
        return 0;
    }

do_retry:
    data.dest = (char*)buf;
    data.blocks = 1;
    data.blocksize = 64;
    data.flags = MMC_DATA_READ;

    cmd.cmdidx = MMC_CMD_SWITCH;
    cmd.resp_type = MMC_RSP_R1b;
    cmd.cmdarg = mode | (0x1 << 31) | 0xfffff0;
    ret = mmc_send_cmd(mmc, &cmd, &data);
    if (ret) {
        printf("mmc_switch: failed\n");
        return -EINVAL;
    }

    if ((buf[16] & 0xf) == (unsigned)mode) {
        mmc->clock = clock;
        mmc->selected_mode = target_mode;
        mmc->ops->set_ios(mmc);
        return 0;
    }

    if ((buf[16] & 0xf) == 0xf) {
        printf("mmc_switch: mode %d error\n", mode);
        return -EINVAL;
    }
    if (retry-- > 0)
        goto do_retry;
    return -EINVAL;
}

static int mmc_startup(struct mmc *mmc)
{
    int err, i;
    uint32_t mult, freq;
    uint64_t cmult, csize;
    struct mmc_cmd cmd;

    cmd.cmdidx = MMC_CMD_ALL_SEND_CID;
    cmd.resp_type = MMC_RSP_R2;
    cmd.cmdarg = 0;
    err = mmc_send_cmd(mmc, &cmd, NULL);
    if (err)
        return err;
    memcpy(mmc->cid, cmd.response, 16);

    cmd.cmdidx = SD_CMD_SEND_RELATIVE_ADDR;
    cmd.cmdarg = mmc->rca << 16;
    cmd.resp_type = MMC_RSP_R6;

    err = mmc_send_cmd(mmc, &cmd, NULL);
    if (err)
        return err;

    if (IS_SD(mmc))
        mmc->rca = (cmd.response[0] >> 16) & 0xffff;

    cmd.cmdidx = MMC_CMD_SEND_CSD;
    cmd.resp_type = MMC_RSP_R2;
    cmd.cmdarg = mmc->rca << 16;

    err = mmc_send_cmd(mmc, &cmd, NULL);
    if (err)
        return err;

    mmc->csd[0] = cmd.response[0];
    mmc->csd[1] = cmd.response[1];
    mmc->csd[2] = cmd.response[2];
    mmc->csd[3] = cmd.response[3];

    freq = fbase[(cmd.response[0] & 0x7)];
    mult = multipliers[((cmd.response[0] >> 3) & 0xf)];
    mmc->legacy_speed = freq * mult;

    mmc->dsr_imp = ((cmd.response[1] >> 12) & 0x1);
    mmc->read_bl_len = 1 << ((cmd.response[1] >> 16) & 0xf);
    if (IS_SD(mmc))
        mmc->write_bl_len = mmc->read_bl_len;
    else
        mmc->write_bl_len = 1 << ((cmd.response[3] >> 22) & 0xf);

    if (mmc->high_capacity) {
        csize = (mmc->csd[1] & 0x3f) << 16
            | (mmc->csd[2] & 0xffff0000) >> 16;
        cmult = 8;
    } else {
        csize = (mmc->csd[1] & 0x3ff) << 2
            | (mmc->csd[2] & 0xc0000000) >> 30;
        cmult = (mmc->csd[2] & 0x00038000) >> 15;
    }

    mmc->capacity_user = (csize + 1) << (cmult + 2);
    mmc->capacity_user *= mmc->read_bl_len;
    mmc->capacity_boot = 0;
    mmc->capacity_rpmb = 0;
    for (i = 0; i < 4; i++)
        mmc->capacity_gp[i] = 0;

    if (mmc->read_bl_len > MMC_MAX_BLOCK_LEN)
        mmc->read_bl_len = MMC_MAX_BLOCK_LEN;

    if (mmc->write_bl_len > MMC_MAX_BLOCK_LEN)
        mmc->write_bl_len = MMC_MAX_BLOCK_LEN;

    if ((mmc->dsr_imp) && (0xffffffff != mmc->dsr)) {
        cmd.cmdidx = MMC_CMD_SET_DSR;
        cmd.cmdarg = (mmc->dsr & 0xffff) << 16;
        cmd.resp_type = MMC_RSP_NONE;
        if (mmc_send_cmd(mmc, &cmd, NULL))
            printf("MMC: SET_DSR failed\n");
    }

    cmd.cmdidx = MMC_CMD_SELECT_CARD;
    cmd.resp_type = MMC_RSP_R1;
    cmd.cmdarg = mmc->rca << 16;
    err = mmc_send_cmd(mmc, &cmd, NULL);

    if (err)
        return err;

    mmc_set_blocklen(mmc, mmc->read_bl_len);
    sd_select_bus_width(mmc, mmc->bus_width);
    return 0;
}

int mmc_init(int host_index) {
    (void)host_index;
    int ret;

    if (_mmc.has_init)
        return 0;

    memset(&_mmc, 0, sizeof(struct mmc));
    memset(&_config, 0, sizeof(struct mmc_config));

    _mmc.cfg = &_config;
    _mmc.bus_width = 1;
    _mmc.clock = 400000;
    _mmc.has_init = false;
    _mmc.cfg->host_caps = MMC_MODE_4BIT | MMC_MODE_HS | MMC_MODE_HS_52MHz;
    _mmc.cfg->voltages = MMC_VDD_32_33 | MMC_VDD_33_34;

    _mmc.ops = bcm2712_sdhci_init();
    if (_mmc.ops == NULL)
        return -1;

    ret = mmc_get_op_cond(&_mmc);
    if (ret != 0)
        return ret;

    _mmc.bus_width = 4;
    _mmc.clock = 25000000;
    ret = _mmc.ops->set_ios(&_mmc);
    if (ret != 0)
        return ret;

    ret = mmc_startup(&_mmc);
    if (ret != 0)
        return ret;
    if (_mmc.read_bl_len == 0)
        return -EIO;

    _mmc.capacity = _mmc.capacity_user / _mmc.read_bl_len;
    _mmc.has_init = true;
    return 0;
}

int mmc_read_blocks(void *dst, lbaint_t start, lbaint_t blkcnt)
{
    struct mmc_cmd cmd;
    struct mmc_data data;
    int retries = 3;
    int reinited = 0;

retry_read:
    if (blkcnt > 1)
        cmd.cmdidx = MMC_CMD_READ_MULTIPLE_BLOCK;
    else
        cmd.cmdidx = MMC_CMD_READ_SINGLE_BLOCK;

    if (_mmc.high_capacity)
        cmd.cmdarg = start;
    else
        cmd.cmdarg = start * _mmc.read_bl_len;
    cmd.resp_type = MMC_RSP_R1;

    data.dest = dst;
    data.blocks = blkcnt;
    data.blocksize = _mmc.read_bl_len;
    data.flags = MMC_DATA_READ;

    if (mmc_send_cmd(&_mmc, &cmd, &data))
        goto recover_read;

    if (blkcnt > 1 && !_mmc.ops->auto_stop) {
        if (mmc_send_stop_transmission(&_mmc, false))
            goto recover_read;
        /* Same as raspix: after the stop command, wait until the card
         * has fully left the data state before the next command is
         * allowed through, or the following transfer can start while
         * the DAT lines are still settling. */
        if (mmc_poll_for_busy(&_mmc, 1000))
            goto recover_read;
    }

    return blkcnt;

recover_read:
    /* Failed data phase, stop or ready wait: the card may be stalled
     * (internal housekeeping after write bursts) or stuck in the data
     * state. Per the SD spec recovery sequence force it back to tran
     * with CMD12, wait for ready, then redo the whole read. Returning
     * without this recovery leaves the card in the data state and
     * wedges every following transfer. */
    {
        mmc_send_stop_transmission(&_mmc, false);
        mmc_poll_for_busy(&_mmc, 500);
    }
    if (--retries > 0)
        goto retry_read;
    /* CMD12-based recovery could not unwedge the card: fall back to a
     * full controller reset + card re-enumeration (what a reboot
     * does), then give the read one more chance. */
    if (!reinited) {
        reinited = 1;
        printf("mmc_read: recover failed, re-initializing card\n");
        _mmc.has_init = false;
        if (mmc_init(0) == 0) {
            retries = 1;
            goto retry_read;
        }
    }
    printf("mmc_read: FAILED (sec %u cnt %u)\n", start, blkcnt);
    return 0;
}

uint32_t mmc_write_blocks(uint32_t start, uint32_t blkcnt, const void *src)
{
    struct mmc_cmd cmd;
    struct mmc_data data;
    int timeout_ms = 1000;
    int retried = 0;

    if (blkcnt == 0)
        return 0;

retry_write:
    if (blkcnt == 1)
        cmd.cmdidx = MMC_CMD_WRITE_SINGLE_BLOCK;
    else
        cmd.cmdidx = MMC_CMD_WRITE_MULTIPLE_BLOCK;

    if (_mmc.high_capacity)
        cmd.cmdarg = start;
    else
        cmd.cmdarg = start * _mmc.write_bl_len;

    cmd.resp_type = MMC_RSP_R1;

    data.src = src;
    data.blocks = blkcnt;
    data.blocksize = _mmc.write_bl_len;
    data.flags = MMC_DATA_WRITE;

    if (mmc_send_cmd(&_mmc, &cmd, &data)) {
        printf("mmc_write: data phase FAILED (sec %u, resp0 0x%x)\n",
                start, cmd.response[0]);
        /* Per the SD spec error recovery sequence: a failed write data
         * phase can leave the card in the rcv/data state, where it will
         * reject any further data command. Send CMD12 to force it back
         * to tran and wait for ready before any retry. */
        struct mmc_cmd stop;
        stop.cmdidx = MMC_CMD_STOP_TRANSMISSION;
        stop.cmdarg = 0;
        stop.resp_type = MMC_RSP_R1b;
        mmc_send_cmd(&_mmc, &stop, NULL);
        mmc_poll_for_busy(&_mmc, timeout_ms);
        if (!retried) {
            retried = 1;
            goto retry_write;
        }
        printf("mmc_write: FAILED for good (sec %u)\n", start);
        return 0;
    }

    /* Only needed when the host does not issue AUTO CMD12 itself. */
    if (blkcnt > 1 && !_mmc.ops->auto_stop) {
        cmd.cmdidx = MMC_CMD_STOP_TRANSMISSION;
        cmd.cmdarg = 0;
        cmd.resp_type = MMC_RSP_R1b;
        if (mmc_send_cmd(&_mmc, &cmd, NULL)) {
            printf("mmc_write: stop FAILED (sec %u)\n", start);
            /* Do not bail out with the card possibly still in the
             * rcv state: force it back to tran, wait for ready and
             * redo the whole write, or every following transfer
             * would be rejected. */
            struct mmc_cmd stop;
            stop.cmdidx = MMC_CMD_STOP_TRANSMISSION;
            stop.cmdarg = 0;
            stop.resp_type = MMC_RSP_R1b;
            mmc_send_cmd(&_mmc, &stop, NULL);
            mmc_poll_for_busy(&_mmc, timeout_ms);
            if (!retried) {
                retried = 1;
                goto retry_write;
            }
            printf("mmc_write: FAILED for good (sec %u)\n", start);
            return 0;
        }
    }

    /* Waiting for the ready status */
    if (mmc_poll_for_busy(&_mmc, timeout_ms)) {
        printf("mmc_write: poll busy after write FAILED (sec %u)\n",
                start);
        return 0;
    }

    /* No read-back verify: the data-phase CRC, the card's internal ECC
     * and the poll-for-busy above are the integrity guarantees; the old
     * full read-back only papered over a historical driver-timing issue
     * and doubled the cost of every write. */
    return blkcnt;
}
