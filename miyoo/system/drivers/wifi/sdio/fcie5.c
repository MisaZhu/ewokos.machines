/*
 * fcie5.c - user-space SD/SDIO host driver for the SigmaStar SSD202D
 *           FCIE5 IP, ported from the kernel BSP HAL
 *           (machines/miyoo/kernel/bsp/sdmmc.c).
 *
 * The wifi module (RTL8723CS) hangs off the FCIE IP at RIU bank 0x1413,
 * see fcie5_reg.h. All data phases use R2N (CIFD PIO) transfers: the IP
 * exposes a 64-byte double buffer which we drain/fill 32 words at a time,
 * so no physically-contiguous DMA memory is required in user space.
 */
#include <string.h>

#include <types.h>
#include <utils/log.h>

#include "fcie5_reg.h"
#include "fcie5.h"
#include "mmc.h"

/* wait times, same as the kernel HAL (ms) */
#define WT_EVENT_RSP        10
#define WT_EVENT_READ       2000
#define WT_EVENT_WRITE      3000
#define WT_EVENT_CIFD       500
#define WT_DAT0HI_END       1000
#define WT_RESET            100

#define RT_CLEAN_SDSTS      3
#define RT_CLEAN_MIEEVENT   3

/* SD_MODE / CMD_RSP_SIZE static parts */
#define V_CMD_SIZE_INIT     (5 << 8)

static uint16_t _sd_mode_datline = 0;       /* R_BUS_WIDTH_x */
static uint16_t _ddr_mode = 0;              /* DDR_MOD for DMA jobs */
static uint16_t _ddr_mode_r2n = 0;          /* DDR_MOD for R2N jobs */
static uint32_t _nrc_us = 100;              /* cmd-to-cmd gap (us) */

/*
 * clock sources of reg_ckg_fcie (bits [4:2]), SSD20xD:
 *   0:48MHz 1:43.2MHz 2:40MHz 3:36MHz 4:32MHz 5:20MHz 6:12MHz 7:300kHz
 */
static const uint32_t _clk_srcs[8] = {
    48000000, 43200000, 40000000, 36000000,
    32000000, 20000000, 12000000, 300000
};

static void set_nrc_delay(uint32_t real_clk) {
    if (real_clk >= 8000000)        _nrc_us = 1;
    else if (real_clk >= 4000000)   _nrc_us = 2;
    else if (real_clk >= 2000000)   _nrc_us = 4;
    else if (real_clk >= 1000000)   _nrc_us = 8;
    else if (real_clk >= 400000)    _nrc_us = 20;
    else if (real_clk >= 300000)    _nrc_us = 27;
    else if (real_clk >= 100000)    _nrc_us = 81;
    else                            _nrc_us = 100;
}

/* ------------------------------------------------------------------ */
/* low level helpers, mirroring the kernel HAL                        */
/* ------------------------------------------------------------------ */

static int clear_sdsts(int retry) {
    do {
        FCIE_REG_SETBIT(REG_SD_STS, M_SD_ERRSTS);
        if (!(FCIE_REG(REG_SD_STS) & M_SD_ERRSTS))
            return 0;
    } while (retry--);
    return -1;
}

static int clear_mie_event(int retry) {
    do {
        FCIE_REG(REG_MIE_EVENT) = M_SD_MIEEVENT;
        if (!(FCIE_REG(REG_MIE_EVENT) & M_SD_MIEEVENT))
            return 0;
    } while (retry--);
    return -1;
}

/* wait for all bits of "events" in MIE_EVENT (poll, 1ms granularity) */
static int wait_mie_event(uint16_t events, uint32_t wait_ms) {
    uint32_t t = 0;
    do {
        if ((FCIE_REG(REG_MIE_EVENT) & events) == events)
            return 0;
        usleep(1000);
        t++;
    } while (t <= wait_ms);
    return -1;
}

/* wait for all bits of "events" in CIFD_EVENT */
static int wait_cifd_event(uint16_t events, uint32_t wait_ms) {
    uint32_t t = 0;
    do {
        if ((FCIE_REG(REG_CIFD_EVENT) & events) == events)
            return 0;
        usleep(1000);
        t++;
    } while (t <= wait_ms);
    return -1;
}

/* wait for the card to release DAT0 after an R1B command */
static int wait_dat0_high(uint32_t wait_ms) {
    uint32_t t = 0;
    do {
        if (FCIE_REG(REG_SD_STS) & R_DAT0)
            return 0;
        usleep(100);
        t++;
    } while (t <= wait_ms * 10);
    return -1;
}

void fcie5_reset(void) {
    uint32_t t = 0;

    FCIE_REG_CLRBIT(REG_SD_CTL, R_JOB_START);
    FCIE_REG_CLRBIT(REG_FCIE_RST, R_FCIE_SOFT_RST);
    while ((FCIE_REG(REG_FCIE_RST) & M_RST_STS) != M_RST_STS) {
        if (t++ > 1000 * WT_RESET) {
            wifi_log("fcie: reset switch low fail\n");
            break;
        }
        usleep(1);
    }

    t = 0;
    FCIE_REG_SETBIT(REG_FCIE_RST, R_FCIE_SOFT_RST);
    while ((FCIE_REG(REG_FCIE_RST) & M_RST_STS) != 0) {
        if (t++ > 1000 * WT_RESET) {
            wifi_log("fcie: reset switch high fail\n");
            break;
        }
        usleep(1);
    }
}

/* ------------------------------------------------------------------ */
/* CIFD (R2N) PIO transfer                                            */
/* ------------------------------------------------------------------ */

static void cifd_data_io(int is_read, uint16_t *buf, int word_cnt) {
    int i;
    for (i = 0; i < word_cnt; i++) {
        if (is_read)
            buf[i] = CIFD_REG(CIFD_RD_FIFO + i);
        else
            CIFD_REG(CIFD_WR_FIFO + i) = buf[i];
    }
}

/*
 * Move "len" bytes between the caller buffer and the 64-byte CIFD
 * double buffer, region by region (kernel _BUF_CIFD_WaitEvent()).
 */
static int cifd_transfer(int is_read, uint8_t *buf, uint32_t len) {
    uint32_t region, region_max;
    uint32_t remain = len & (64 - 1);

    region_max = (len >> 6) + (remain ? 1 : 0);

    for (region = 0; region < region_max; region++) {
        uint16_t *p = (uint16_t *)(buf + (region << 6));
        int words = 32;

        if ((region == region_max - 1) && remain > 0)
            words = remain / 2;

        if (is_read) {
            if (wait_cifd_event(R_WBUF_FULL, WT_EVENT_CIFD) != 0)
                return -1;
            cifd_data_io(1, p, words);
            FCIE_REG(REG_CIFD_EVENT) = R_WBUF_FULL;
            FCIE_REG(REG_CIFD_EVENT) = R_WBUF_EMPTY_TRIG;
        }
        else {
            cifd_data_io(0, p, words);
            FCIE_REG(REG_CIFD_EVENT) = R_RBUF_FULL_TRIG;
            if (wait_cifd_event(R_RBUF_EMPTY, WT_EVENT_CIFD) != 0)
                return -1;
            FCIE_REG(REG_CIFD_EVENT) = R_RBUF_EMPTY;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* command / response                                                 */
/* ------------------------------------------------------------------ */

static void set_cmd_token(uint8_t cmd, uint32_t arg) {
    FCIE_REG(REG_CMD_FIFO + 0) = (((arg >> 24) & 0xff) << 8) | (0x40 | cmd);
    FCIE_REG(REG_CMD_FIFO + 1) = (((arg >> 8) & 0xff) << 8) | ((arg >> 16) & 0xff);
    FCIE_REG(REG_CMD_FIFO + 2) = arg & 0xff;
}

/* response bytes live in the CMD FIFO after completion */
static uint8_t get_rsp_byte(int pos) {
    uint16_t v = FCIE_REG(REG_CMD_FIFO + (pos >> 1));
    return (pos & 1) ? (v >> 8) : (v & 0xff);
}

static void read_response(struct mmc_cmd *cmd, int rsp_size) {
    int i;

    memset(cmd->response, 0, sizeof(cmd->response));
    if (rsp_size <= 0)
        return;

    if (cmd->resp_type & MMC_RSP_136) {
        /* byte 0 is the start/echo byte, payload follows big-endian */
        for (i = 0; i < 4; i++) {
            cmd->response[i] = ((uint32_t)get_rsp_byte(1 + i * 4) << 24) |
                               ((uint32_t)get_rsp_byte(2 + i * 4) << 16) |
                               ((uint32_t)get_rsp_byte(3 + i * 4) << 8)  |
                               ((uint32_t)get_rsp_byte(4 + i * 4));
        }
    }
    else {
        cmd->response[0] = ((uint32_t)get_rsp_byte(1) << 24) |
                           ((uint32_t)get_rsp_byte(2) << 16) |
                           ((uint32_t)get_rsp_byte(3) << 8)  |
                           ((uint32_t)get_rsp_byte(4));
    }
}

/*
 * translate the generic MMC_RSP_* flags into the FCIE response setting:
 * rsp_size (bytes in the CMD FIFO) and SD_CTL response-enable bits.
 */
static void map_resp_type(uint32_t resp_type, uint16_t *rsp_size, uint16_t *ctl_bits) {
    if (!(resp_type & MMC_RSP_PRESENT)) {
        *rsp_size = 0;
        *ctl_bits = 0;
    }
    else if (resp_type & MMC_RSP_136) {
        *rsp_size = 0x10;
        *ctl_bits = R_RSPR2_EN | R_RSP_EN;
    }
    else {
        *rsp_size = 0x05;
        *ctl_bits = R_RSP_EN;
    }
}

int fcie5_send_command(struct mmc_cmd *cmd, struct mmc_data *data) {
    uint16_t rsp_size, rsp_ctl;
    uint16_t sd_mode, sd_ctl;
    uint16_t wait_event = R_CMD_END;
    uint32_t wait_ms = WT_EVENT_RSP;
    int is_read = (data != NULL) && (data->flags & MMC_DATA_READ);
    int is_write = (data != NULL) && (data->flags & MMC_DATA_WRITE);
    uint16_t sts;

    map_resp_type(cmd->resp_type, &rsp_size, &rsp_ctl);

    /* R2N transfer setting (kernel Hal_SDMMC_TransCmdSetting) */
    if (data != NULL) {
        uint32_t tran_len = data->blocks * data->blocksize;
        FCIE_REG(REG_BLK_SIZE) = data->blocksize;
        FCIE_REG(REG_JOB_BLK_CNT) = data->blocks;
        FCIE_REG(REG_DMA_LEN_L) = (uint16_t)(tran_len & 0xffff);
        FCIE_REG(REG_DMA_LEN_H) = (uint16_t)(tran_len >> 16);
    }

    sd_mode = R_CLK_EN | _sd_mode_datline;
    if (data != NULL)
        sd_mode |= R_DEST_R2N;

    sd_ctl = rsp_ctl;
    if (is_write)
        sd_ctl |= R_JOB_DIR;

    set_cmd_token(cmd->cmdidx, cmd->cmdarg);

    FCIE_REG(REG_CMD_RSP_SIZE) = V_CMD_SIZE_INIT | rsp_size;
    FCIE_REG(REG_MIE_FUNC_CTL) = R_SDIO_MODE_EN;
    FCIE_REG(REG_SD_MODE) = sd_mode;
    FCIE_REG(REG_SD_CTL) = sd_ctl;
    FCIE_REG(REG_MMA_PRI) = R_MIU_R_PRIORITY | R_MIU_W_PRIORITY;
    FCIE_REG(REG_DDR_MOD) = (data != NULL) ? _ddr_mode_r2n : _ddr_mode;

    FCIE_REG_CLRBIT(REG_BOOT_MOD, R_BOOT_MODE);
    FCIE_REG_CLRBIT(REG_BOOT, R_NAND_BOOT_EN | R_BOOTSRAM_ACCESS_SEL | R_IMI_SEL);

    usleep(_nrc_us);

    if (clear_sdsts(RT_CLEAN_SDSTS) != 0 || clear_mie_event(RT_CLEAN_MIEEVENT) != 0) {
        fcie5_reset();
        return -EIO;
    }

    if (is_read) {
        /* R2N read: data phase completion is checked after draining CIFD */
        FCIE_REG_SETBIT(REG_SD_CTL, R_CMD_EN | R_DTRX_EN);
        FCIE_REG_SETBIT(REG_SD_CTL, R_CMD_EN | R_DTRX_EN | R_JOB_START);
    }
    else {
        FCIE_REG_SETBIT(REG_SD_CTL, R_CMD_EN);
        FCIE_REG_SETBIT(REG_SD_CTL, R_CMD_EN | R_JOB_START);
    }

    if (wait_mie_event(wait_event, wait_ms) != 0) {
        fcie5_reset();
        return -ETIMEDOUT;
    }

    /* R2N read data phase */
    if (is_read) {
        if (cifd_transfer(1, data->dest, data->blocks * data->blocksize) != 0) {
            fcie5_reset();
            return -ETIMEDOUT;
        }
        if (wait_mie_event(R_DATA_END, WT_EVENT_READ) != 0) {
            fcie5_reset();
            return -ETIMEDOUT;
        }
    }

    if (cmd->resp_type & MMC_RSP_BUSY) {
        if (wait_dat0_high(WT_DAT0HI_END) != 0) {
            fcie5_reset();
            return -ETIMEDOUT;
        }
    }
    else if ((cmd->resp_type & MMC_RSP_PRESENT) && !(cmd->resp_type & MMC_RSP_CRC)) {
        /* R3/R4 have no CRC7: clear the bogus CRC error (IP quirk) */
        FCIE_REG(REG_SD_STS) = R_CMDRSP_CERR;
    }

    /* R2N write data phase: separate DTRX job after the command phase */
    if (is_write && !(FCIE_REG(REG_SD_STS) & M_SD_ERRSTS)) {
        if (clear_sdsts(RT_CLEAN_SDSTS) != 0 || clear_mie_event(RT_CLEAN_MIEEVENT) != 0) {
            fcie5_reset();
            return -EIO;
        }
        FCIE_REG(REG_SD_CTL) = R_JOB_DIR;
        FCIE_REG_SETBIT(REG_SD_CTL, R_DTRX_EN);
        FCIE_REG_SETBIT(REG_SD_CTL, R_DTRX_EN | R_JOB_START);

        if (cifd_transfer(0, data->src, data->blocks * data->blocksize) != 0) {
            fcie5_reset();
            return -ETIMEDOUT;
        }
        if (wait_mie_event(R_DATA_END, WT_EVENT_WRITE) != 0) {
            fcie5_reset();
            return -ETIMEDOUT;
        }
    }

    sts = FCIE_REG(REG_SD_STS) & M_SD_ERRSTS;
    if (sts != 0) {
        if (sts & R_CMD_NORSP)
            return -ETIMEDOUT;
        return -EIO;
    }

    read_response(cmd, rsp_size);
    return 0;
}

/* ------------------------------------------------------------------ */
/* clock / bus width / init                                           */
/* ------------------------------------------------------------------ */

uint32_t fcie5_set_clock(uint32_t hz) {
    int sel = 7;    /* slowest (300kHz) as fallback */
    int i;

    for (i = 0; i < 8; i++) {
        if (_clk_srcs[i] <= hz) {
            sel = i;
            break;
        }
    }

    /* gate, switch source, ungate */
    REG_CKG_WIFI = CKG_GATE;
    REG_CKG_WIFI = (uint16_t)(sel << CKG_SEL_SHIFT);

    set_nrc_delay(_clk_srcs[sel]);
    return _clk_srcs[sel];
}

void fcie5_set_bus_width(int width) {
    if (width == 4)
        _sd_mode_datline = R_BUS_WIDTH_4;
    else
        _sd_mode_datline = 0;
}

void fcie5_enable_sdio_int(int on) {
    if (on) {
        FCIE_REG_SETBIT(REG_SDIO_DET_ON, R_SDIO_DET_ON_BIT);
        FCIE_REG_SETBIT(REG_MIE_INT_EN, R_SDIO_INT);
    }
    else {
        FCIE_REG_CLRBIT(REG_SDIO_DET_ON, R_SDIO_DET_ON_BIT);
        FCIE_REG_CLRBIT(REG_MIE_INT_EN, R_SDIO_INT);
    }
}

int fcie5_sdio_int_pending(void) {
    return (FCIE_REG(REG_MIE_EVENT) & R_SDIO_INT) ? 1 : 0;
}

void fcie5_clear_sdio_int(void) {
    FCIE_REG(REG_MIE_EVENT) = R_SDIO_INT;
}

int fcie5_init(void) {
    fcie5_reset();

    /*
     * Advance-mode pad timing (kernel Hal_SDMMC_SetBusTiming, EV_BUS_DEF):
     * the SSD20xD pads cannot run bypass mode.
     */
    _ddr_mode = R_PAD_CLK_SEL | R_PAD_IN_SEL | R_FALL_LATCH;
    _ddr_mode_r2n = _ddr_mode | R_PAD_IN_RDY_SEL | R_PRE_FULL_SEL0 | R_PRE_FULL_SEL1;

    FCIE_REG(REG_MIE_FUNC_CTL) = R_SDIO_MODE_EN;
    FCIE_REG(REG_SDIO_MODE) = 0;
    FCIE_REG(REG_SD_MODE) = R_CLK_EN;

    fcie5_set_bus_width(1);
    fcie5_set_clock(300000);

    /* touch CIFD once to avoid the first-access latch issue */
    (void)CIFD_REG(CIFD_RD_FIFO);
    return 0;
}
