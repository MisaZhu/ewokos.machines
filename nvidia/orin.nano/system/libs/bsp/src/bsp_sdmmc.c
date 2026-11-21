#include <stdint.h>
#include <string.h>
#include <bsp/bsp_sdmmc.h>
#include <bsp/bsp_bpmp.h>
#include <bsp/bsp_gpio.h>
#include <ewoksys/syscall.h>
#include <ewoksys/klog.h>
#include <orin_net.h>

/*
 * Tegra234 SDMMC1 (microSD), standard SDHCI + Tegra vendor regs, polled PIO.
 * Register layout from upstream sdhci.h / sdhci-tegra.c.  Hardware-unverified;
 * every wait is bounded.  Runs the card at legacy/HS 25MHz (no UHS/1.8V
 * tuning) which is sufficient for a rootfs.  BPMP provides clock/reset.
 */

#define SDMMC_VA  0x67000000ULL

/* Standard SDHCI registers */
#define SD_SYS_ADDR     0x00
#define SD_BLK_SIZE     0x04
#define SD_BLK_COUNT    0x06
#define SD_ARG          0x08
#define SD_XFER_MODE    0x0c
#define SD_CMD          0x0e
#define SD_RESP         0x10
#define SD_BUF_DATA     0x20
#define SD_PRESENT      0x24
#define SD_POWER        0x29
#define SD_CLOCK        0x2c
#define SD_SW_RESET     0x2f
#define SD_INT_STATUS   0x30
#define SD_INT_ENABLE   0x34
#define SD_CAPS         0x40
/* Tegra vendor registers (defs from upstream sdhci-tegra.c) */
#define SD_TEGRA_CLOCK_CTRL   0x100
#define SD_TEGRA_MISC_CTRL    0x120
#define SD_TEGRA_PADCTRL      0x1e0
#define SD_TEGRA_AUTOCAL_CFG  0x1e4
#define SD_TEGRA_AUTOCAL_STAT 0x1ec
#define SD_HOST_VERSION       0xfe

#define SD_TAP_MASK           0x00ff0000u
#define SD_TAP_SHIFT          16
#define SD_TRIM_MASK          0x1f000000u
#define SD_TRIM_SHIFT         24
#define SD_SPI_MODE_CLKEN_OVR (1u << 2)

#define SD_MISC_SDR104        0x8u
#define SD_MISC_SDR50         0x10u
#define SD_MISC_SPEC300       0x20u
#define SD_MISC_DDR50         0x200u

#define SD_PADCTRL_VREF_MASK  0x0000000fu
#define SD_PADCTRL_VREF_VAL   0x7u
#define SD_PADCTRL_E_PWRD     (1u << 31)
#define SD_PADCTRL_DRVUPDN_MASK 0x07fff000u

#define SD_AUTOCAL_START      (1u << 31)
#define SD_AUTOCAL_ENABLE     (1u << 29)
#define SD_AUTOCAL_PDPU_MASK  0x0000ffffu
#define SD_AUTOCAL_ACTIVE     (1u << 31)

#define INT_CMD_COMPLETE  (1u << 0)
#define INT_XFER_COMPLETE (1u << 1)
#define INT_BUF_WR_READY  (1u << 4)
#define INT_BUF_RD_READY  (1u << 5)
#define INT_ERROR         (1u << 15)
#define INT_CMD_ERR_MASK  (0x000f0000u)  /* cmd timeout/CRC/endbit/index */
#define INT_DATA_ERR_MASK (0x00f00000u)

#define RESP_NONE 0
#define RESP_136  1
#define RESP_48   2
#define RESP_48B  3

static volatile uint8_t* _sd;
static int _ready;
static int _sdhc;            /* 1 = SDHC/SDXC (block addressing) */
static uint64_t _sectors;
static uint16_t _rca;
static uint32_t _err_st;     /* last INT_STATUS seen by a failing sd_wait */

static inline uint32_t rd32(uint32_t off) {
    return *(volatile uint32_t*)(_sd + off);
}
static inline void wr32(uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(_sd + off) = v;
}
static inline uint16_t rd16(uint32_t off) {
    return *(volatile uint16_t*)(_sd + off);
}
static inline void wr16(uint32_t off, uint16_t v) {
    *(volatile uint16_t*)(_sd + off) = v;
}
static inline uint8_t rd8(uint32_t off) {
    return *(volatile uint8_t*)(_sd + off);
}
static inline void wr8(uint32_t off, uint8_t v) {
    *(volatile uint8_t*)(_sd + off) = v;
}

static void sd_clear_int(void) {
    wr16(SD_INT_STATUS, 0xffff);
}

/* Wait for a status bit (or error).  Returns 0 ok, -1 timeout/error. */
static int sd_wait(uint32_t want) {
    for (uint32_t i = 0; i < 2000000u; i++) {
        uint32_t st = rd16(SD_INT_STATUS);
        if (st & INT_ERROR) {
            _err_st = st;
            sd_clear_int();
            return -1;
        }
        if (st & want)
            return 0;
    }
    _err_st = rd16(SD_INT_STATUS);
    sd_clear_int();
    return -1;
}

static uint16_t cmd_reg(int idx, int resp, int data, int crc, int idxchk) {
    return (uint16_t)((idx << 8) | (data ? 0x20 : 0) |
            (idxchk ? 0x10 : 0) | (crc ? 0x08 : 0) | resp);
}

/* Command with no data phase.  resp_out may be NULL. */
static int sd_cmd(int idx, uint32_t arg, int resp, int crc, int idxchk,
        uint32_t* resp_out) {
    sd_clear_int();
    wr32(SD_ARG, arg);
    wr16(SD_CMD, cmd_reg(idx, resp, 0, crc, idxchk));
    if (sd_wait(INT_CMD_COMPLETE) != 0)
        return -1;
    if (resp_out) {
        resp_out[0] = rd32(SD_RESP);
        if (resp == RESP_136) {
            resp_out[1] = rd32(SD_RESP + 4);
            resp_out[2] = rd32(SD_RESP + 8);
            resp_out[3] = rd32(SD_RESP + 12);
        }
    }
    sd_clear_int();
    return 0;
}

static void sd_set_clock(uint32_t hz) {
    /* Base clock: Tegra CAP_CLOCK_BASE_BROKEN -- assume 204MHz parent. */
    uint32_t base = 204000000u;
    uint32_t div = base / (2u * hz);
    if (div == 0) div = 1;
    if (div > 255) div = 255;
    uint16_t clk = (uint16_t)((div << 8) | 0x01);  /* internal enable */
    wr16(SD_CLOCK, clk);
    for (uint32_t i = 0; i < 100000u; i++)
        if (rd16(SD_CLOCK) & 0x02) break;          /* internal stable */
    wr16(SD_CLOCK, clk | 0x04);                    /* SD clock enable */
}

static void sd_delay_us(uint32_t n) {
    for (volatile uint32_t i = 0; i < n * 500u; i++) { }
}

/* Pad drive-strength auto-calibration (upstream tegra_sdhci_pad_autocalib,
 * 3.3V legacy path).  Gate the card clock, power the cal pad, ENABLE|START,
 * wait STATUS.ACTIVE to clear; on timeout fall back to fixed drive strength. */
static void sd_pad_autocal(void) {
    uint32_t cfg = rd32(SD_TEGRA_AUTOCAL_CFG);
    cfg &= ~SD_AUTOCAL_PDPU_MASK;                  /* PDPU offset 0 (no DT trim) */
    wr32(SD_TEGRA_AUTOCAL_CFG, cfg);

    uint16_t clk = rd16(SD_CLOCK);
    int card_en = (clk & 0x04) != 0;
    wr16(SD_CLOCK, clk & ~0x04);                   /* gate card clock */

    uint32_t pad = rd32(SD_TEGRA_PADCTRL);
    wr32(SD_TEGRA_PADCTRL, pad | SD_PADCTRL_E_PWRD);  /* power cal pad */
    sd_delay_us(2);

    cfg = rd32(SD_TEGRA_AUTOCAL_CFG);
    wr32(SD_TEGRA_AUTOCAL_CFG, cfg | SD_AUTOCAL_ENABLE | SD_AUTOCAL_START);
    sd_delay_us(2);

    int done = 0;
    for (uint32_t i = 0; i < 100000u; i++) {       /* ~10ms bounded */
        if ((rd32(SD_TEGRA_AUTOCAL_STAT) & SD_AUTOCAL_ACTIVE) == 0) {
            done = 1;
            break;
        }
    }
    uint32_t stat = rd32(SD_TEGRA_AUTOCAL_STAT);

    pad = rd32(SD_TEGRA_PADCTRL);
    wr32(SD_TEGRA_PADCTRL, pad & ~SD_PADCTRL_E_PWRD);  /* cal pad off */
    if (card_en)
        wr16(SD_CLOCK, rd16(SD_CLOCK) | 0x04);     /* restore card clock */

    if (!done) {
        cfg = rd32(SD_TEGRA_AUTOCAL_CFG);
        wr32(SD_TEGRA_AUTOCAL_CFG, cfg & ~SD_AUTOCAL_ENABLE);
        pad = rd32(SD_TEGRA_PADCTRL);
        wr32(SD_TEGRA_PADCTRL, pad & ~SD_PADCTRL_DRVUPDN_MASK);  /* fixed drv */
    }
    klog("orin sdmmc: pad autocal %s stat=%08x cfg=%08x\n",
            done ? "ok" : "TIMEOUT(fixed drv)", stat, rd32(SD_TEGRA_AUTOCAL_CFG));
}

static int sd_data_xfer(int write, uint32_t addr, void* buf) {
    sd_clear_int();
    wr16(SD_BLK_SIZE, 512);
    wr16(SD_BLK_COUNT, 1);
    wr16(SD_XFER_MODE, write ? 0x00 : 0x10);       /* PIO, direction */
    wr32(SD_ARG, addr);
    wr16(SD_CMD, cmd_reg(write ? 24 : 17, RESP_48, 1, 1, 1));
    if (sd_wait(INT_CMD_COMPLETE) != 0) {
        klog("orin sdmmc: cmd%d cmd-complete fail st=%04x\n",
                write ? 24 : 17, _err_st);
        return -1;
    }
    if (write) {
        if (sd_wait(INT_BUF_WR_READY) != 0) {
            klog("orin sdmmc: cmd24 buf-wr-ready fail st=%04x\n", _err_st);
            return -1;
        }
        const uint8_t* p = (const uint8_t*)buf;
        for (uint32_t i = 0; i < 512; i += 4) {
            uint32_t w = (uint32_t)p[i] | ((uint32_t)p[i+1] << 8) |
                    ((uint32_t)p[i+2] << 16) | ((uint32_t)p[i+3] << 24);
            wr32(SD_BUF_DATA, w);
        }
    } else {
        if (sd_wait(INT_BUF_RD_READY) != 0) {
            klog("orin sdmmc: cmd17 buf-rd-ready fail st=%04x\n", _err_st);
            return -1;
        }
        uint8_t* p = (uint8_t*)buf;
        for (uint32_t i = 0; i < 512; i += 4) {
            uint32_t w = rd32(SD_BUF_DATA);
            p[i] = (uint8_t)w; p[i+1] = (uint8_t)(w >> 8);
            p[i+2] = (uint8_t)(w >> 16); p[i+3] = (uint8_t)(w >> 24);
        }
    }
    if (sd_wait(INT_XFER_COMPLETE) != 0) {
        klog("orin sdmmc: cmd%d xfer-complete fail st=%04x\n",
                write ? 24 : 17, _err_st);
        return -1;
    }
    sd_clear_int();
    return 0;
}

static uint32_t sector_addr(uint32_t sector) {
    return _sdhc ? sector : sector * 512u;
}

int bsp_sdmmc_present(void) {
    /* Card detect G7 active low; also fall back to PRESENT state bit16. */
    bsp_gpio_init();
    if (bsp_gpio_read(ORIN_SD_CD_GPIO) == 0)
        return 1;
    if (_sd && (rd32(SD_PRESENT) & (1u << 16)))
        return 1;
    return 0;
}

uint64_t bsp_sdmmc_capacity(void) {
    return _sectors;
}

int bsp_sdmmc_init(void) {
    if (_ready)
        return 0;
    if (syscall3(SYS_MEM_MAP, (ewokos_addr_t)SDMMC_VA,
                (ewokos_addr_t)ORIN_SDMMC1_PHY,
                (ewokos_addr_t)ORIN_SDMMC1_SIZE) != (ewokos_addr_t)SDMMC_VA) {
        klog("orin sdmmc: register map failed\n");
        return -1;
    }
    _sd = (volatile uint8_t*)(uintptr_t)SDMMC_VA;
    klog("orin sdmmc: regs mapped\n");

    if (bsp_bpmp_init() != 0) {
        klog("orin sdmmc: bpmp init failed\n");
        return -1;
    }
    bsp_bpmp_reset_assert(ORIN_SDMMC1_RESET);
    bsp_bpmp_reset_deassert(ORIN_SDMMC1_RESET);
    bsp_bpmp_clk_set_rate(ORIN_SDMMC1_CLK, 204000000u);
    bsp_bpmp_clk_enable(ORIN_SDMMC1_CLK);
    bsp_bpmp_clk_set_rate(ORIN_SDMMC1_TMCLK, 12000000u);
    bsp_bpmp_clk_enable(ORIN_SDMMC1_TMCLK);
    klog("orin sdmmc: bpmp clk/reset done\n");

    if (!bsp_sdmmc_present()) {
        klog("orin sdmmc: no card detected (G7)\n");
        return -1;
    }
    klog("orin sdmmc: card present\n");

    /* Full software reset. */
    wr8(SD_SW_RESET, 0x01);
    int rst_done = 0;
    for (uint32_t i = 0; i < 100000u; i++)
        if ((rd8(SD_SW_RESET) & 0x01) == 0) { rst_done = 1; break; }
    klog("orin sdmmc: sw reset %s caps=%08x present=%08x ver=%04x\n",
            rst_done ? "done" : "STUCK", rd32(SD_CAPS), rd32(SD_PRESENT),
            rd16(SD_HOST_VERSION));

    /* Tegra vendor reset setup (upstream tegra_sdhci_reset, RESET_ALL):
     * default tap/trim (0 = no DT), clear UHS mode bits for legacy bring-up,
     * and set the pad-calibration reference voltage (HAS_PADCALIB VREF=0x7).
     * Tegra234 derives spec v3 from the version register, not the vendor bit. */
    uint32_t clk_ctrl = rd32(SD_TEGRA_CLOCK_CTRL);
    clk_ctrl &= ~(SD_TAP_MASK | SD_TRIM_MASK | SD_SPI_MODE_CLKEN_OVR);
    wr32(SD_TEGRA_CLOCK_CTRL, clk_ctrl);

    uint32_t misc = rd32(SD_TEGRA_MISC_CTRL);
    misc &= ~(SD_MISC_SPEC300 | SD_MISC_SDR50 | SD_MISC_DDR50 | SD_MISC_SDR104);
    wr32(SD_TEGRA_MISC_CTRL, misc);

    uint32_t pad = rd32(SD_TEGRA_PADCTRL);
    pad = (pad & ~SD_PADCTRL_VREF_MASK) | SD_PADCTRL_VREF_VAL;
    wr32(SD_TEGRA_PADCTRL, pad);
    klog("orin sdmmc: vendor clk_ctrl=%08x misc=%08x pad=%08x\n",
            rd32(SD_TEGRA_CLOCK_CTRL), rd32(SD_TEGRA_MISC_CTRL),
            rd32(SD_TEGRA_PADCTRL));

    wr16(SD_INT_ENABLE, 0xffff);         /* normal int status enable */
    wr16(SD_INT_ENABLE + 2, 0xffff);     /* error int status enable */
    wr8(SD_POWER, 0x0f);                 /* 3.3V, bus power on */
    sd_set_clock(400000);                /* 400kHz init clock */
    klog("orin sdmmc: clk=%04x power=%02x\n", rd16(SD_CLOCK), rd8(SD_POWER));

    sd_pad_autocal();                    /* calibrate pad drive strength */
    klog("orin sdmmc: begin card init\n");

    /* CMD0 -> idle */
    if (sd_cmd(0, 0, RESP_NONE, 0, 0, NULL) != 0) {
        klog("orin sdmmc: CMD0 failed st=%04x err=%04x present=%08x cmd=%04x clk=%04x\n",
                _err_st, rd16(SD_INT_STATUS + 2), rd32(SD_PRESENT),
                rd16(SD_CMD), rd16(SD_CLOCK));
        return -1;
    }
    /* CMD8 -> voltage check (SDHC/SDXC capable cards echo 0x1AA) */
    uint32_t r7 = 0;
    int v2 = (sd_cmd(8, 0x1aa, RESP_48, 1, 1, &r7) == 0 &&
            (r7 & 0xfff) == 0x1aa);
    klog("orin sdmmc: CMD8 r7=%08x v2=%d\n", r7, v2);
    /* ACMD41 -> send op cond, poll busy (HCS if v2) */
    uint32_t ocr = 0;
    int ok = 0;
    for (uint32_t i = 0; i < 2000u; i++) {
        if (sd_cmd(55, 0, RESP_48, 1, 1, NULL) != 0)
            break;
        if (sd_cmd(41, v2 ? 0x40ff8000u : 0x00ff8000u, RESP_48, 0, 0, &ocr) != 0)
            break;
        if (ocr & 0x80000000u) { ok = 1; break; }
    }
    if (!ok) {
        klog("orin sdmmc: ACMD41 timeout ocr=%08x\n", ocr);
        return -1;
    }
    _sdhc = v2 && (ocr & 0x40000000u);   /* CCS -> block addressing */
    klog("orin sdmmc: ACMD41 ok ocr=%08x sdhc=%d\n", ocr, _sdhc);

    uint32_t cid[4];
    if (sd_cmd(2, 0, RESP_136, 0, 0, cid) != 0) {
        klog("orin sdmmc: CMD2 (CID) failed st=%04x\n", _err_st);
        return -1;
    }
    uint32_t r6 = 0;
    if (sd_cmd(3, 0, RESP_48, 1, 1, &r6) != 0) {
        klog("orin sdmmc: CMD3 (RCA) failed st=%04x\n", _err_st);
        return -1;
    }
    _rca = (uint16_t)(r6 >> 16);
    uint32_t csd[4];
    if (sd_cmd(9, (uint32_t)_rca << 16, RESP_136, 0, 0, csd) != 0) {
        klog("orin sdmmc: CMD9 (CSD) failed st=%04x\n", _err_st);
        return -1;
    }
    if (sd_cmd(7, (uint32_t)_rca << 16, RESP_48B, 1, 1, NULL) != 0) {
        klog("orin sdmmc: CMD7 (select) failed st=%04x\n", _err_st);
        return -1;
    }
    klog("orin sdmmc: card selected rca=%04x\n", _rca);

    /* Capacity from CSD. */
    if ((csd[0] >> 30) == 1) {           /* CSD v2 (SDHC/SDXC) */
        uint32_t c_size = ((csd[1] & 0x3f) << 16) | (csd[2] >> 16);
        _sectors = (uint64_t)(c_size + 1) * 1024u;
    } else {                             /* CSD v1 (SDSC) */
        uint32_t c_size = ((csd[1] & 0x3ff) << 2) | (csd[2] >> 30);
        uint32_t mult = 1u << (((csd[2] >> 15) & 0x7) + 2);
        uint32_t read_bl = (csd[1] >> 16) & 0xf;
        _sectors = (uint64_t)(c_size + 1) * mult * (1u << read_bl) / 512u;
    }

    if (!_sdhc)
        sd_cmd(16, 512, RESP_48, 1, 1, NULL);   /* SET_BLOCKLEN for SDSC */
    sd_set_clock(25000000);              /* 25MHz operational clock */

    _ready = 1;
    klog("orin sdmmc: card ready %s, %llu MB, rca=%04x\n",
            _sdhc ? "SDHC/SDXC" : "SDSC",
            (unsigned long long)(_sectors / 2048u), _rca);
    return 0;
}

int bsp_sdmmc_read_blocks(uint32_t sector, void* buf, uint32_t count) {
    if (!_ready || buf == NULL)
        return -1;
    uint8_t* p = (uint8_t*)buf;
    for (uint32_t i = 0; i < count; i++) {
        if (sd_data_xfer(0, sector_addr(sector + i), p + i * 512u) != 0)
            return -1;
    }
    return 0;
}

int bsp_sdmmc_write_blocks(uint32_t sector, const void* buf, uint32_t count) {
    if (!_ready || buf == NULL)
        return -1;
    const uint8_t* p = (const uint8_t*)buf;
    for (uint32_t i = 0; i < count; i++) {
        if (sd_data_xfer(1, sector_addr(sector + i),
                    (void*)(p + i * 512u)) != 0)
            return -1;
    }
    return 0;
}