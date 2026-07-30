/*
 * mmc.c - SDIO card enumeration / CMD52 / CMD53 on top of the FCIE5 host,
 *         ported from the raspix wlan driver (sdhci_* -> fcie5_*).
 */
#include <types.h>
#include <utils/log.h>

#include "mmc.h"
#include "sdio.h"
#include "fcie5.h"

#define MMC_DEBUG   0

static struct mmc _mmc;

static int mmc_set_ios(void) {
    fcie5_set_bus_width(_mmc.bus_width);
    fcie5_set_clock(_mmc.clock);
    return 0;
}

static int mmc_sdio_try_enable_high_speed(bool *enabled) {
    uint8_t speed = 0;
    int err;

    *enabled = false;

    err = mmc_io_rw_direct(0, 0, SDIO_CCCR_SPEED, 0, &speed);
    if (err)
        return err;

    if ((speed & SDIO_SPEED_SHS) == 0)
        return 0;

    speed &= ~SDIO_SPEED_BSS_MASK;
    speed |= SDIO_SPEED_EHS;
    err = mmc_io_rw_direct(1, 0, SDIO_CCCR_SPEED, speed, NULL);
    if (err)
        return err;

    *enabled = true;
    return 0;
}

int mmc_io_rw_direct_host(int write, unsigned fn,
    unsigned addr, uint8_t in, uint8_t *out) {
    struct mmc_cmd cmd = {};
    int err;

    /* sanity check */
    if (addr & ~0x1FFFF)
        return -EINVAL;

    cmd.cmdidx = SD_IO_RW_DIRECT;
    cmd.cmdarg = write ? 0x80000000 : 0x00000000;
    cmd.cmdarg |= fn << 28;
    cmd.cmdarg |= (write && out) ? 0x08000000 : 0x00000000;
    cmd.cmdarg |= addr << 9;
    cmd.cmdarg |= in;
    cmd.resp_type = MMC_RSP_R5 | MMC_CMD_AC;

    err = fcie5_send_command(&cmd, NULL);

#if MMC_DEBUG
    if (out)
        wifi_log("%s w:%d f:%d a:%x in:%x out:%x\n", __func__, write, fn, addr, in, cmd.response[0] & 0xFF);
    else
        wifi_log("%s w:%d f:%d a:%x in:%x\n", __func__, write, fn, addr, in);
#endif

    if (err) {
        wifi_log("cmd52 transport fail w=%d fn=%u addr=0x%x in=0x%x err=%d stat_resp=0x%x\n",
            write, fn, addr, in, err, cmd.response[0]);
        return err;
    }

    if (cmd.response[0] & R5_ERROR) {
        wifi_log("cmd52 r5 error w=%d fn=%u addr=0x%x resp=0x%x\n",
            write, fn, addr, cmd.response[0]);
        return -EIO;
    }
    if (cmd.response[0] & R5_FUNCTION_NUMBER) {
        wifi_log("cmd52 fn error w=%d fn=%u addr=0x%x resp=0x%x\n",
            write, fn, addr, cmd.response[0]);
        return -EINVAL;
    }
    if (cmd.response[0] & R5_OUT_OF_RANGE) {
        wifi_log("cmd52 range error w=%d fn=%u addr=0x%x resp=0x%x\n",
            write, fn, addr, cmd.response[0]);
        return -ERANGE;
    }

    if (out)
        *out = cmd.response[0] & 0xFF;

    return 0;
}

int mmc_io_rw_direct(int write, unsigned fn,
    unsigned addr, uint8_t in, uint8_t *out) {
    return mmc_io_rw_direct_host(write, fn, addr, in, out);
}

int mmc_io_rw_extended(int write, int fn,
    unsigned addr, int incr_addr, uint8_t *buf, unsigned blocks, unsigned blksz) {
    struct mmc_cmd cmd = {};
    struct mmc_data data = {};
    int err;

    /* sanity check */
    if (addr & ~0x1FFFF)
        return -EINVAL;
    if (blksz == 0)
        return -EINVAL;

    cmd.cmdidx = SD_IO_RW_EXTENDED;
    cmd.cmdarg = write ? 0x80000000 : 0x00000000;
    cmd.cmdarg |= fn << 28;
    cmd.cmdarg |= incr_addr ? 0x04000000 : 0x00000000;
    cmd.cmdarg |= addr << 9;
    if (blocks == 0)
        cmd.cmdarg |= (blksz == 512) ? 0 : blksz;   /* byte mode */
    else
        cmd.cmdarg |= 0x08000000 | blocks;          /* block mode */
    cmd.resp_type = MMC_RSP_R5 | MMC_CMD_ADTC;

    data.blocksize = blksz;
    /* Code in host drivers/fwk assumes that "blocks" always is >=1 */
    data.blocks = blocks ? blocks : 1;
    data.flags = write ? MMC_DATA_WRITE : MMC_DATA_READ;
    data.src = buf;

    err = fcie5_send_command(&cmd, &data);
    if (err)
        return err;

    if (cmd.response[0] & R5_ERROR)
        err = -EIO;
    else if (cmd.response[0] & R5_FUNCTION_NUMBER)
        err = -EINVAL;
    else if (cmd.response[0] & R5_OUT_OF_RANGE)
        err = -ERANGE;
    else
        err = 0;

    return err;
}

static int mmc_go_idle(void) {
    struct mmc_cmd cmd;
    int err;

    usleep(1000);

    cmd.cmdidx = MMC_CMD_GO_IDLE_STATE;
    cmd.cmdarg = 0;
    cmd.resp_type = MMC_RSP_NONE;

    err = fcie5_send_command(&cmd, NULL);
    if (err)
        return err;

    return 0;
}

static int mmc_sdio_set_bus_width(uint8_t width) {
    uint8_t if_ctrl;
    int err;

    err = mmc_io_rw_direct(0, 0, SDIO_CCCR_IF, 0, &if_ctrl);
    if (err)
        return err;

    if_ctrl &= ~SDIO_BUS_WIDTH_MASK;
    if_ctrl |= (width == 4) ? SDIO_BUS_WIDTH_4BIT : SDIO_BUS_WIDTH_1BIT;

    err = mmc_io_rw_direct(1, 0, SDIO_CCCR_IF, if_ctrl, NULL);
    if (err)
        return err;

    _mmc.bus_width = width;
    return mmc_set_ios();
}

int mmc_configure_sdio_bus(uint8_t width, uint32_t clock) {
    int err;

    err = mmc_sdio_set_bus_width(width);
    if (err)
        return err;

    _mmc.clock = clock;
    return mmc_set_ios();
}

/*
 * SDIO card enumeration for the RTL8723CS:
 * CMD5(0) -> CMD5(OCR) -> CMD3 -> CMD7, then 4bit + high speed.
 */
static int sdio_card_init(void) {
    struct mmc_cmd cmd = {};
    int err;

    usleep(1000);

    cmd.cmdidx = SD_IO_SEND_OP_COND;
    cmd.cmdarg = 0x0;
    cmd.resp_type = MMC_RSP_R4 | MMC_CMD_BCR;

    err = fcie5_send_command(&cmd, NULL);
    if (err)
        return err;
    _mmc.ocr = cmd.response[0];
    wifi_log("sdio: R4 ocr=0x%x funcs=%d\n", _mmc.ocr, (_mmc.ocr >> 28) & 0x7);

    cmd.cmdidx = SD_IO_SEND_OP_COND;
    cmd.cmdarg = 0x300000; /* 3.2~3.4V */
    cmd.resp_type = MMC_RSP_R4 | MMC_CMD_BCR;

    err = fcie5_send_command(&cmd, NULL);
    if (err)
        return err;

    cmd.cmdidx = MMC_CMD_SET_RELATIVE_ADDR;
    cmd.cmdarg = 0x0;
    cmd.resp_type = MMC_RSP_R6 | MMC_CMD_BCR;

    err = fcie5_send_command(&cmd, NULL);
    if (err)
        return err;

    _mmc.rca = (cmd.response[0] >> 16) & 0xffff;
    if (_mmc.rca == 0)
        _mmc.rca = 1;

    cmd.cmdidx = MMC_CMD_SELECT_CARD;
    cmd.cmdarg = ((uint32_t)_mmc.rca) << 16;
    cmd.resp_type = MMC_RSP_R1 | MMC_CMD_AC;

    err = fcie5_send_command(&cmd, NULL);
    if (err)
        return err;

    bool high_speed = false;

    err = mmc_sdio_set_bus_width(4);
    if (err)
        return err;

    err = mmc_sdio_try_enable_high_speed(&high_speed);
    if (err)
        wifi_log("sdio high-speed enable failed %d, keep current timing\n", err);

    /*
     * Verify the bus actually works at the chosen rate before handing
     * it to the probe path; step the FCIE clock down until CCCR reads
     * back reliably. Sources: 48/40/20/12MHz.
     */
    {
        static const uint32_t try_clks[] =
            { 48000000, 40000000, 20000000, 12000000 };
        unsigned int i;
        uint8_t cccr_rev;

        err = -EIO;
        for (i = 0; i < sizeof(try_clks) / sizeof(try_clks[0]); i++) {
            if (high_speed == false && try_clks[i] > 25000000)
                continue;
            _mmc.clock = try_clks[i];
            err = mmc_set_ios();
            if (err)
                continue;

            err = mmc_io_rw_direct(0, 0, SDIO_CCCR_CCCR, 0, &cccr_rev);
            if (!err)
                break;

            wifi_log("sdio bus dead at %uHz, stepping down\n", try_clks[i]);
            high_speed = false;
        }
        if (err) {
            wifi_log("sdio bus unusable at any clock, err=%d\n", err);
            return err;
        }
        wifi_log("sdio: bus up at %uHz %s\n", _mmc.clock,
            high_speed ? "(high-speed)" : "");
    }

    return 0;
}

int mmc_hw_reset(void) {
    int err;

    fcie5_init();
    _mmc.bus_width = 1;
    _mmc.clock = 400000;
    if (mmc_set_ios())
        return -EIO;

    err = mmc_go_idle();
    if (err)
        return err;
    usleep(200000);

    return sdio_card_init();
}
