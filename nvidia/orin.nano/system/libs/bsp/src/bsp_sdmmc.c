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
/* Tegra vendor registers */
#define SD_TEGRA_CLOCK_CTRL   0x100
#define SD_TEGRA_MISC_CTRL    0x120
#define SD_TEGRA_AUTOCAL_CFG  0x1e4
#define SD_TEGRA_AUTOCAL_STAT 0x1ec

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
            sd_clear_int();
            return -1;
        }
        if (st & want)
            return 0;
    }
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

static int sd_data_xfer(int write, uint32_t addr, void* buf) {
    sd_clear_int();
    wr16(SD_BLK_SIZE, 512);
    wr16(SD_BLK_COUNT, 1);
    wr16(SD_XFER_MODE, write ? 0x00 : 0x10);       /* PIO, direction */
    wr32(SD_ARG, addr);
    wr16(SD_CMD, cmd_reg(write ? 24 : 17, RESP_48, 1, 1, 1));
    if (sd_wait(INT_CMD_COMPLETE) != 0)
        return -1;
    if (write) {
        if (sd_wait(INT_BUF_WR_READY) != 0)
            return -1;
        const uint8_t* p = (const uint8_t*)buf;
        for (uint32_t i = 0; i < 512; i += 4) {
            uint32_t w = (uint32_t)p[i] | ((uint32_t)p[i+1] << 8) |
                    ((uint32_t)p[i+2] << 16) | ((uint32_t)p[i+3] << 24);
            wr32(SD_BUF_DATA, w);
        }
    } else {
        if (sd_wait(INT_BUF_RD_READY) != 0)
            return -1;
        uint8_t* p = (uint8_t*)buf;
        for (uint32_t i = 0; i < 512; i += 4) {
            uint32_t w = rd32(SD_BUF_DATA);
            p[i] = (uint8_t)w; p[i+1] = (uint8_t)(w >> 8);
            p[i+2] = (uint8_t)(w >> 16); p[i+3] = (uint8_t)(w >> 24);
        }
    }
    if (sd_wait(INT_XFER_COMPLETE) != 0)
        return -1;
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
        slog("orin sdmmc: register map failed\n");
        return -1;
    }
    _sd = (volatile uint8_t*)(uintptr_t)SDMMC_VA;

    if (bsp_bpmp_init() != 0)
        return -1;
    bsp_bpmp_reset_assert(ORIN_SDMMC1_RESET);
    bsp_bpmp_reset_deassert(ORIN_SDMMC1_RESET);
    bsp_bpmp_clk_set_rate(ORIN_SDMMC1_CLK, 204000000u);
    bsp_bpmp_clk_enable(ORIN_SDMMC1_CLK);
    bsp_bpmp_clk_set_rate(ORIN_SDMMC1_TMCLK, 12000000u);
    bsp_bpmp_clk_enable(ORIN_SDMMC1_TMCLK);

    if (!bsp_sdmmc_present()) {
        slog("orin sdmmc: no card detected (G7)\n");
        return -1;
    }

    /* Full software reset. */
    wr8(SD_SW_RESET, 0x01);
    for (uint32_t i = 0; i < 100000u; i++)
        if ((rd8(SD_SW_RESET) & 0x01) == 0) break;

    /* Tegra vendor: default tap/trim, enable SPEC300, run auto-cal. */
    wr32(SD_TEGRA_CLOCK_CTRL, (14u << 16) | (8u << 24));
    wr32(SD_TEGRA_MISC_CTRL, rd32(SD_TEGRA_MISC_CTRL) | 0x1u);
    wr32(SD_TEGRA_AUTOCAL_CFG, rd32(SD_TEGRA_AUTOCAL_CFG) | 0x1u);
    for (uint32_t i = 0; i < 100000u; i++)
        if (rd32(SD_TEGRA_AUTOCAL_STAT) & 0x1u) break;

    wr16(SD_INT_ENABLE, 0);
    wr8(SD_POWER, 0x0f);                 /* 3.3V, bus power on */
    sd_set_clock(400000);                /* 400kHz init clock */

    /* CMD0 -> idle */
    if (sd_cmd(0, 0, RESP_NONE, 0, 0, NULL) != 0) {
        slog("orin sdmmc: CMD0 failed\n");
        return -1;
    }
    /* CMD8 -> voltage check (SDHC/SDXC capable cards echo 0x1AA) */
    uint32_t r7 = 0;
    int v2 = (sd_cmd(8, 0x1aa, RESP_48, 1, 1, &r7) == 0 &&
            (r7 & 0xfff) == 0x1aa);
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
        slog("orin sdmmc: ACMD41 timeout ocr=%08x\n", ocr);
        return -1;
    }
    _sdhc = v2 && (ocr & 0x40000000u);   /* CCS -> block addressing */

    uint32_t cid[4];
    if (sd_cmd(2, 0, RESP_136, 0, 0, cid) != 0) return -1;   /* ALL_SEND_CID */
    uint32_t r6 = 0;
    if (sd_cmd(3, 0, RESP_48, 1, 1, &r6) != 0) return -1;     /* SEND_REL_ADDR */
    _rca = (uint16_t)(r6 >> 16);
    uint32_t csd[4];
    if (sd_cmd(9, (uint32_t)_rca << 16, RESP_136, 0, 0, csd) != 0) return -1;
    if (sd_cmd(7, (uint32_t)_rca << 16, RESP_48B, 1, 1, NULL) != 0) return -1;

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
    slog("orin sdmmc: card ready %s, %llu MB, rca=%04x\n",
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