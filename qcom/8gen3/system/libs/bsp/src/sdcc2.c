/*
 * SDCC2 (Qualcomm SC8280XP SDHCI) driver for user space.
 *
 * This is the board's user-space SD backend: bsp_sd registers it with libsd and
 * every process that needs the card drives the controller directly through the
 * MMIO windows mapped by SYS_MEM_MAP.  The kernel keeps its own read-only copy
 * solely to load init from the physical SD before user space starts.
 *
 * Keep the register programming in sync with machines/q8b/kernel/bsp/sd.c.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ewoksys/syscall.h>
#include <ewoksys/proc.h>
#include <ewoksys/dma.h>
#include <sd/sd.h>
#include <bsp/sdcc2.h>

/* SDMA is not yet validated at 50MHz HS on this board: a DMA transfer that
 * never completes hangs the SDCC2 bus and the PMIC warm-resets the SoC after
 * ~45s.  Keep the kernel-verified PIO read path until SDMA is proven here.
 * SDCC2_PIO_BLOCKS=1 is the conservative single-block CMD17 path. */
#ifndef SDCC2_USE_SDMA
#define SDCC2_USE_SDMA 0
#endif
#ifndef SDCC2_PIO_BLOCKS
#define SDCC2_PIO_BLOCKS 1
#endif
/* Restore the pre-HS user-space behaviour: stay at 25MHz/1-bit and let the
 * runtime DMA path work exactly as it did before the high-speed bring-up. */
#ifndef SDCC2_ENABLE_HS
#define SDCC2_ENABLE_HS 1
#endif
/* The boot that worked at 50MHz/4-bit used single-block user reads: the
 * kernel reported multi_ready=0 because its CMD18 probe still used Auto
 * CMD12.  Keep user space on single-block DMA (no CMD12) to reproduce it. */
#ifndef SDCC2_FORCE_SINGLE_BLOCK
#define SDCC2_FORCE_SINGLE_BLOCK 0
#endif
/* This SDCC2 advertises ADMA2 (CAPS bit19) but NOT SDMA (bit22), so the
 * old SDMA path can never complete.  ADMA2 is the real bulk path. */
#ifndef SDCC2_USE_ADMA
#define SDCC2_USE_ADMA 1
#endif
#ifndef SDCC2_ADMA64
#define SDCC2_ADMA64 1
#endif
#define SDCC2_ADMA_BLOCKS 32U

/* MMIO layout, mirrored from kernel/bsp/hw_arch.h.  All windows are mapped at
 * the same offset inside one user-space base so the driver macros below stay
 * identical to the kernel version. */
#define Q8B_SDCC_WIN_PHY    0x08800000ULL
#define Q8B_SDCC_WIN_SIZE   0x00100000U
#define Q8B_SDCC_WIN_OFF    0x05000000ULL
#define Q8B_SDCC2_PHY       0x08804000ULL
#define Q8B_SDCC2_OFF       (Q8B_SDCC_WIN_OFF + 0x4000ULL)

#define Q8B_TLMM_WIN_PHY    0x0f100000ULL
#define Q8B_TLMM_WIN_SIZE   0x00300000U
#define Q8B_TLMM_WIN_OFF    0x05100000ULL

#define Q8B_RSC_WIN_PHY     0x18220000ULL
#define Q8B_RSC_WIN_SIZE    0x00010000U
#define Q8B_RSC_WIN_OFF     0x05400000ULL

#define Q8B_CMD_DB_WIN_PHY  0x80860000ULL
#define Q8B_CMD_DB_WIN_SIZE 0x00020000U
#define Q8B_CMD_DB_WIN_OFF  0x05500000ULL

#define CMD_DB_DATA_OFFSET  144U
#define CMD_DB_RSC_OFFSET   8U
#define CMD_DB_RSC_SIZE     16U
#define CMD_DB_ENTRY_SIZE   24U
#define CMD_DB_MAX_SLAVES   8U
#define CMD_DB_MAGIC        0x0c0330dbU

/* Keep clear of bsp_fb's 0x50000000 GOP window (up to 64MB), bsp_sd's
 * 0x60000000 RAM-disk window and sdprobe's 0x58000000 register dump. */
#define Q8B_SD_MMIO_BASE    0x70000000ULL

static uint64_t sdcc2_mmio_base = Q8B_SD_MMIO_BASE;

static int32_t _physical_sd_ready = 0;
static int32_t _physical_sd_high_capacity = 0;
static int32_t _physical_sd_multi_ready = 0;
static uint64_t _physical_sd_sectors = 0;
static uint32_t _physical_sd_rca = 0;

static inline uint64_t sdcc2_cntvct(void) {
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v) : : "memory");
    return v;
}

static inline uint32_t sdcc2_cntfrq(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v) : : "memory");
    return (uint32_t)v;
}

static void sdcc2_delay_us(uint32_t usec) {
    if(usec == 0)
        return;
    /* Long delays can sleep.  The SD poll loops use 1us steps thousands of
     * times; a sleep syscall there costs a tick each (~1ms) and turns a
     * 100000-iteration timeout into minutes.  Busy-wait on the architected
     * counter instead so the loops behave like the kernel's _delay_usec(),
     * even before the vsyscall fine clock is published. */
    if(usec > 1000U) {
        if(proc_nsleep_precise((uint64_t)usec * 1000ULL) != 0)
            usleep(usec);
        return;
    }
    uint32_t frq = sdcc2_cntfrq();
    if(frq == 0) {
        usleep(usec);
        return;
    }
    uint64_t ticks = ((uint64_t)usec * frq + 999999ULL) / 1000000ULL;
    uint64_t start = sdcc2_cntvct();
    while(sdcc2_cntvct() - start < ticks) {
    }
}

#define GCC_BASE                (sdcc2_mmio_base + 0x00100000ULL)
#define GCC_SDCC2_APPS_CBCR     0x14004
#define GCC_SDCC2_AHB_CBCR      0x14008
#define GCC_SDCC2_APPS_CMD_RCGR 0x1400c
#define GCC_SDCC2_APPS_CFG_RCGR 0x14010
#define GCC_SDCC2_APPS_M        0x14014
#define GCC_SDCC2_APPS_N        0x14018
#define GCC_SDCC2_APPS_D        0x1401c
#define GCC_RCG_UPDATE           (1U << 0)
#define GCC_RCG_ROOT_ENABLE      (1U << 1)
#define GCC_RCG_ROOT_OFF         (1U << 31)
#define GCC_CBCR_CLOCK_OFF       (1U << 31)
#define GCC_SDCC2_CFG_MASK       ((0x1fU << 0) | (0x7U << 8) | \
                                 (0x3U << 12) | (1U << 20))

#define SDCC2_BASE              (sdcc2_mmio_base + Q8B_SDCC2_OFF)
#define TLMM_BASE               (sdcc2_mmio_base + Q8B_TLMM_WIN_OFF)
#define TLMM_SDC2_CTL           0x0e8000
#define TLMM_GPIO131_CTL        (131 * 0x1000)
#define TLMM_GPIO131_IO         (TLMM_GPIO131_CTL + 0x4)

#define CMD_DB_BASE             (sdcc2_mmio_base + Q8B_CMD_DB_WIN_OFF)

#define RPMH_RSC_BASE           (sdcc2_mmio_base + Q8B_RSC_WIN_OFF)
#define RPMH_TCS_BASE           (RPMH_RSC_BASE + 0x0d00U)
#define RPMH_TCS_STRIDE         672U
#define RPMH_CMD_STRIDE         20U
#define RPMH_IRQ_ENABLE         0x00U
#define RPMH_IRQ_STATUS         0x04U
#define RPMH_IRQ_CLEAR          0x08U
#define RPMH_CONTROL            0x14U
#define RPMH_STATUS             0x18U
#define RPMH_CMD_ENABLE         0x1cU
#define RPMH_CMD_MSGID          0x30U
#define RPMH_CMD_ADDR           0x34U
#define RPMH_CMD_DATA           0x38U
#define RPMH_CMD_STATUS         0x3cU
#define RPMH_AMC_ENABLE         (1U << 16)
#define RPMH_AMC_TRIGGER        (1U << 24)
#define RPMH_WRITE_RESP_MSGID   0x00010108U

#define RPMH_VRM_VOLTAGE        0U
#define RPMH_VRM_ENABLE         4U
#define RPMH_VRM_MODE           8U
#define PMIC5_LDO_MODE_HPM      7U

#define SDHCI_DMA_ADDRESS       0x000
#define SDHCI_ADMA_ADDRESS      0x058
#define SDHCI_ADMA_ADDRESS_HI   0x05c
#define SDHCI_BLOCK_SIZE        0x004
#define SDHCI_BLOCK_COUNT       0x006
#define SDHCI_ARGUMENT          0x008
#define SDHCI_TRANSFER_MODE     0x00c
#define SDHCI_COMMAND           0x00e
#define SDHCI_RESPONSE          0x010
#define SDHCI_BUFFER            0x020
#define SDHCI_PRESENT_STATE     0x024
#define SDHCI_HOST_CONTROL      0x028
#define SDHCI_POWER_CONTROL     0x029
#define SDHCI_CLOCK_CONTROL     0x02c
#define SDHCI_TIMEOUT_CONTROL   0x02e
#define SDHCI_SOFTWARE_RESET    0x02f
#define SDHCI_CORE_HC_MODE      0x078
#define SDHCI_INT_STATUS        0x030
#define SDHCI_INT_ENABLE        0x034
#define SDHCI_SIGNAL_ENABLE     0x038
#define SDHCI_CAPABILITIES      0x040
#define SDHCI_CAPABILITIES_1    0x044
#define SDHCI_HOST_VERSION      0x0fe
#define SDHCI_CORE_VENDOR_SPEC  0x20c
#define SDHCI_CORE_VENDOR_CAPS0 0x21c
#define SDHCI_CORE_PWR_STATUS   0x240
#define SDHCI_CORE_PWR_MASK     0x244
#define SDHCI_CORE_PWR_CLEAR    0x248
#define SDHCI_CORE_PWR_CTL      0x24c
#define SDHCI_CORE_MCI_VERSION  0x318
#define SDCC2_PIO_MAX_BLOCKS    32U
#define SDHCI_CORE_HC_MODE_EN   (1U << 0)
#define SDHCI_CORE_FF_CLK_SW_RST_DIS (1U << 13)
#define SDHCI_MSM_IO_PAD_ENABLE (1U << 15)
#define SDHCI_MSM_IO_PAD_1P8V   (1U << 16)

static uint8_t _sd_probe_buf[512];
static uint8_t _sd_probe_multi_buf[4U * 512U];

static inline uint32_t mmio_read32(uint64_t base, uint32_t off) {
    return *(volatile uint32_t*)(base + off);
}

static inline void mmio_write32(uint64_t base, uint32_t off, uint32_t val) {
    *(volatile uint32_t*)(base + off) = val;
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint32_t mmio_read16(uint64_t base, uint32_t off) {
    uint32_t val = mmio_read32(base, off & ~3U);
    return (val >> ((off & 2U) * 8U)) & 0xffffU;
}

static uint32_t mmio_read8(uint64_t base, uint32_t off) {
    uint32_t val = mmio_read32(base, off & ~3U);
    return (val >> ((off & 3U) * 8U)) & 0xffU;
}

static void mmio_write16(uint64_t base, uint32_t off, uint32_t val) {
    *(volatile uint16_t*)(base + off) = (uint16_t)val;
    __asm__ volatile("dsb sy" ::: "memory");
}

static void mmio_write8(uint64_t base, uint32_t off, uint32_t val) {
    *(volatile uint8_t*)(base + off) = (uint8_t)val;
    __asm__ volatile("dsb sy" ::: "memory");
}

static void sd_set_diag(uint32_t state, uint32_t detail);
static int32_t sd_fail(uint32_t stage, uint32_t detail);
static int32_t sdcc2_enable_card_clock(void);

static int32_t cmd_db_id_matches(uint32_t entry, const char* id) {
    for (uint32_t i = 0; i < 8U; i++) {
        uint8_t expected = (uint8_t)id[i];
        if (mmio_read8(CMD_DB_BASE, entry + i) != expected)
            return 0;
        if (expected == 0)
            return 1;
    }
    return id[8] == 0;
}

static uint32_t cmd_db_read_addr(const char* id) {
    if (mmio_read32(CMD_DB_BASE, 4U) != CMD_DB_MAGIC)
        return 0;

    for (uint32_t i = 0; i < CMD_DB_MAX_SLAVES; i++) {
        uint32_t rsc = CMD_DB_RSC_OFFSET + i * CMD_DB_RSC_SIZE;
        uint32_t slave_id = mmio_read16(CMD_DB_BASE, rsc);
        uint32_t header_off;
        uint32_t count;
        uint32_t entries;

        if (slave_id == 0)
            break;
        header_off = mmio_read16(CMD_DB_BASE, rsc + 2U);
        count = mmio_read16(CMD_DB_BASE, rsc + 6U);
        entries = CMD_DB_DATA_OFFSET + header_off;
        if (entries >= Q8B_CMD_DB_WIN_SIZE ||
                count > (Q8B_CMD_DB_WIN_SIZE - entries) /
                        CMD_DB_ENTRY_SIZE)
            return 0;

        for (uint32_t j = 0; j < count; j++) {
            uint32_t entry = entries + j * CMD_DB_ENTRY_SIZE;
            if (cmd_db_id_matches(entry, id))
                return mmio_read32(CMD_DB_BASE, entry + 16U);
        }
    }
    return 0;
}

static uint64_t rpmh_tcs_reg(uint32_t tcs, uint32_t reg) {
    return RPMH_TCS_BASE + tcs * RPMH_TCS_STRIDE + reg;
}

static uint64_t rpmh_tcs_cmd(uint32_t tcs, uint32_t cmd, uint32_t reg) {
    return rpmh_tcs_reg(tcs, reg) + cmd * RPMH_CMD_STRIDE;
}

static int32_t rpmh_write_tcs_reg_sync(uint32_t tcs, uint32_t reg,
        uint32_t value) {
    uint64_t base = rpmh_tcs_reg(tcs, 0);

    mmio_write32(base, reg, value);
    for (uint32_t i = 0; i < 100000U; i++) {
        if (mmio_read32(base, reg) == value)
            return 0;
        sdcc2_delay_us(1);
    }
    return -1;
}

static void rpmh_finish_tcs(uint32_t tcs) {
    uint32_t control = mmio_read32(rpmh_tcs_reg(tcs, 0), RPMH_CONTROL);

    (void)rpmh_write_tcs_reg_sync(tcs, RPMH_CONTROL,
            control & ~RPMH_AMC_TRIGGER);
    (void)rpmh_write_tcs_reg_sync(tcs, RPMH_CONTROL, 0);
    (void)rpmh_write_tcs_reg_sync(tcs, RPMH_CMD_ENABLE, 0);
    mmio_write32(RPMH_TCS_BASE, RPMH_IRQ_CLEAR, 1U << tcs);
}

static int32_t rpmh_write_sync(uint32_t address, uint32_t data,
        uint32_t* detail) {
    const uint32_t tcs = 0;
    const uint32_t irq_bit = 1U << tcs;
    uint32_t control;

    control = mmio_read32(rpmh_tcs_reg(tcs, 0), RPMH_CONTROL);
    control &= ~RPMH_AMC_TRIGGER;
    if (rpmh_write_tcs_reg_sync(tcs, RPMH_CONTROL, control) != 0)
        goto sync_failed;
    control &= ~RPMH_AMC_ENABLE;
    if (rpmh_write_tcs_reg_sync(tcs, RPMH_CONTROL, control) != 0)
        goto sync_failed;

    if (rpmh_write_tcs_reg_sync(tcs, RPMH_CMD_ENABLE, 0) != 0)
        goto sync_failed;
    mmio_write32(RPMH_TCS_BASE, RPMH_IRQ_CLEAR, irq_bit);
    mmio_write32(RPMH_TCS_BASE, RPMH_IRQ_ENABLE,
            mmio_read32(RPMH_TCS_BASE, RPMH_IRQ_ENABLE) | irq_bit);
    mmio_write32(rpmh_tcs_cmd(tcs, 0, 0), RPMH_CMD_MSGID,
            RPMH_WRITE_RESP_MSGID);
    mmio_write32(rpmh_tcs_cmd(tcs, 0, 0), RPMH_CMD_ADDR, address);
    mmio_write32(rpmh_tcs_cmd(tcs, 0, 0), RPMH_CMD_DATA, data);
    if (rpmh_write_tcs_reg_sync(tcs, RPMH_CMD_ENABLE, 1U) != 0)
        goto sync_failed;
    if (rpmh_write_tcs_reg_sync(tcs, RPMH_CONTROL,
                RPMH_AMC_ENABLE) != 0)
        goto sync_failed;
    mmio_write32(rpmh_tcs_reg(tcs, 0), RPMH_CONTROL,
            RPMH_AMC_ENABLE | RPMH_AMC_TRIGGER);

    for (uint32_t i = 0; i < 1000000U; i++) {
        uint32_t irq = mmio_read32(RPMH_TCS_BASE, RPMH_IRQ_STATUS);
        if (irq & irq_bit) {
            if (detail)
                *detail = mmio_read32(rpmh_tcs_cmd(tcs, 0, 0),
                        RPMH_CMD_STATUS);
            rpmh_finish_tcs(tcs);
            return 0;
        }
        sdcc2_delay_us(1);
    }

    if (detail) {
        *detail = (mmio_read32(rpmh_tcs_reg(tcs, 0), RPMH_STATUS) &
                0xffffU) |
                ((mmio_read32(rpmh_tcs_cmd(tcs, 0, 0),
                        RPMH_CMD_STATUS) & 0xffffU) << 16);
    }
    rpmh_finish_tcs(tcs);
    return -1;

sync_failed:
    if (detail)
        *detail = 0xf0000000U |
                (mmio_read32(rpmh_tcs_reg(tcs, 0), RPMH_CONTROL) &
                        0x0fffffffU);
    rpmh_finish_tcs(tcs);
    return -1;
}

static int32_t rpmh_enable_regulator(uint32_t address, uint32_t* detail) {
    if (rpmh_write_sync(address + RPMH_VRM_VOLTAGE, 2960U, detail) != 0)
        return -1;
    if (rpmh_write_sync(address + RPMH_VRM_MODE,
                PMIC5_LDO_MODE_HPM, detail) != 0)
        return -1;
    if (rpmh_write_sync(address + RPMH_VRM_ENABLE, 1U, detail) != 0)
        return -1;
    return 0;
}

static int32_t rpmh_disable_regulator(uint32_t address, uint32_t* detail) {
    return rpmh_write_sync(address + RPMH_VRM_ENABLE, 0, detail);
}

static int32_t sdcc2_enable_rails(void) {
    uint32_t ldoc6;
    uint32_t ldoc9;
    uint32_t detail = 0;

    /* Resolve the two PM8350C SD-card rails from the firmware Command DB,
     * exactly like the kernel probe does.  No platform_data dependency. */
    if (mmio_read32(CMD_DB_BASE, 4U) != CMD_DB_MAGIC)
        return sd_fail(10, mmio_read32(CMD_DB_BASE, 4U));
    ldoc6 = cmd_db_read_addr("ldoc6");
    ldoc9 = cmd_db_read_addr("ldoc9");
    if (ldoc6 == 0 || ldoc9 == 0)
        return sd_fail(11, ldoc6 != 0 ? ldoc6 : ldoc9);
    /* A warm reboot does not remove card power.  Start from the same state as
     * Linux mmc_power_cycle() so a timed-out transfer cannot leak into the
     * next kernel's identification sequence. */
    if (rpmh_disable_regulator(ldoc6, &detail) != 0 ||
            rpmh_disable_regulator(ldoc9, &detail) != 0)
        return sd_fail(12, detail);
    /* Allow the card to discharge after a warm firmware handoff. */
    sdcc2_delay_us(100000);
    /* Match the MMC core power-up order: card supply before I/O supply. */
    if (rpmh_enable_regulator(ldoc9, &detail) != 0)
        return sd_fail(12, detail);
    sdcc2_delay_us(30000);
    if (rpmh_enable_regulator(ldoc6, &detail) != 0)
        return sd_fail(13, detail);
    sd_set_diag(10, detail);
    sdcc2_delay_us(30000);
    return 0;
}

static void sd_set_diag(uint32_t state, uint32_t detail) {
    (void)state;
    (void)detail;
}

static int32_t sd_fail(uint32_t stage, uint32_t detail) {
    sd_set_diag(0x8000U | (stage & 0x7fffU), detail);
    return -1;
}

static int32_t sdcc2_wait_gcc_clock(uint32_t reg) {
    for (uint32_t i = 0; i < 1000U; i++) {
        if ((mmio_read32(GCC_BASE, reg) & GCC_CBCR_CLOCK_OFF) == 0)
            return 0;
        sdcc2_delay_us(1);
    }
    return -1;
}

static int32_t sdcc2_config_gcc_clock(uint32_t m, uint32_t n,
        uint32_t d, uint32_t cfg_value) {
    uint32_t cmd;
    uint32_t cfg;

    mmio_write32(GCC_BASE, GCC_SDCC2_AHB_CBCR,
            mmio_read32(GCC_BASE, GCC_SDCC2_AHB_CBCR) | 1U);
    if (sdcc2_wait_gcc_clock(GCC_SDCC2_AHB_CBCR) != 0)
        return -1;

    cmd = mmio_read32(GCC_BASE, GCC_SDCC2_APPS_CMD_RCGR);
    mmio_write32(GCC_BASE, GCC_SDCC2_APPS_CMD_RCGR,
            cmd | GCC_RCG_ROOT_ENABLE);
    for (uint32_t i = 0; i < 1000U; i++) {
        if ((mmio_read32(GCC_BASE, GCC_SDCC2_APPS_CMD_RCGR) &
                    GCC_RCG_ROOT_OFF) == 0)
            break;
        if (i == 999U)
            return -1;
        sdcc2_delay_us(1);
    }

    mmio_write32(GCC_BASE, GCC_SDCC2_APPS_M, m);
    mmio_write32(GCC_BASE, GCC_SDCC2_APPS_N, n);
    mmio_write32(GCC_BASE, GCC_SDCC2_APPS_D, d);
    cfg = mmio_read32(GCC_BASE, GCC_SDCC2_APPS_CFG_RCGR);
    cfg = (cfg & ~GCC_SDCC2_CFG_MASK) |
            (cfg_value & GCC_SDCC2_CFG_MASK);
    mmio_write32(GCC_BASE, GCC_SDCC2_APPS_CFG_RCGR, cfg);

    cmd = mmio_read32(GCC_BASE, GCC_SDCC2_APPS_CMD_RCGR);
    mmio_write32(GCC_BASE, GCC_SDCC2_APPS_CMD_RCGR,
            cmd | GCC_RCG_UPDATE);
    for (uint32_t i = 0; i < 1000U; i++) {
        if ((mmio_read32(GCC_BASE, GCC_SDCC2_APPS_CMD_RCGR) &
                    GCC_RCG_UPDATE) == 0)
            break;
        if (i == 999U)
            return -1;
        sdcc2_delay_us(1);
    }

    mmio_write32(GCC_BASE, GCC_SDCC2_APPS_CBCR,
            mmio_read32(GCC_BASE, GCC_SDCC2_APPS_CBCR) | 1U);
    if (sdcc2_wait_gcc_clock(GCC_SDCC2_APPS_CBCR) != 0)
        return -1;

    cmd = mmio_read32(GCC_BASE, GCC_SDCC2_APPS_CMD_RCGR);
    mmio_write32(GCC_BASE, GCC_SDCC2_APPS_CMD_RCGR,
            cmd & ~GCC_RCG_ROOT_ENABLE);
    return 0;
}

static int32_t sdcc2_set_clock_400khz(void) {

    /* 19.2MHz TCXO / 12, then M/N = 1/4 => 400kHz. */
    return sdcc2_config_gcc_clock(1U, 0xfcU, 0xfbU, 0x2017U);
}

static int32_t sdcc2_set_clock_25mhz(void) {
    /* gcc_sdcc2_apps_clk_src table: F(25000000, P_GCC_GPLL0_OUT_EVEN, 12).
     * GPLL0_OUT_EVEN is 300MHz, so the RCG divider field is 11 (0x060b).
     * The old 0x0617 value divided by 24 and produced 12.5MHz. */
    mmio_write16(SDCC2_BASE, SDHCI_CLOCK_CONTROL, 0);
    if (sdcc2_config_gcc_clock(0, 0, 0, 0x060bU) != 0)
        return -1;
    return sdcc2_enable_card_clock();
}

static int32_t sdcc2_set_clock_50mhz(void) {
    /* F(50000000, P_GCC_GPLL0_OUT_EVEN, 6): 300MHz / 6 = 50MHz,
     * RCG divider field 5 (0x0605). */
    mmio_write16(SDCC2_BASE, SDHCI_CLOCK_CONTROL, 0);
    if (sdcc2_config_gcc_clock(0, 0, 0, 0x0605U) != 0)
        return -1;
    return sdcc2_enable_card_clock();
}

static void sdcc2_restore_clock_400khz(void) {
    mmio_write16(SDCC2_BASE, SDHCI_CLOCK_CONTROL, 0);
    (void)sdcc2_set_clock_400khz();
    (void)sdcc2_enable_card_clock();
    sdcc2_delay_us(1000);
}

static void sdcc2_config_pins(void) {
    uint32_t val = mmio_read32(TLMM_BASE, TLMM_SDC2_CTL);

    /* SDC2 clock: 16mA, no pull. Command/data: 16mA, pull-up. */
    val &= ~((7U << 6) | (3U << 14) |
            (7U << 3) | (3U << 11) |
            (7U << 0) | (3U << 9));
    val |= (7U << 6) | (7U << 3) | (3U << 11) |
            (7U << 0) | (3U << 9);
    mmio_write32(TLMM_BASE, TLMM_SDC2_CTL, val);

    /* GPIO131 remains an input with no pull for active-low card detect. */
    val = mmio_read32(TLMM_BASE, TLMM_GPIO131_CTL);
    val &= ~((7U << 6) | (0xfU << 2) | 3U | (1U << 9));
    mmio_write32(TLMM_BASE, TLMM_GPIO131_CTL, val);
}

static void sdcc2_handle_power(void) {
    uint32_t status = mmio_read32(SDCC2_BASE, SDHCI_CORE_PWR_STATUS) & 0xfU;
    uint32_t ack = 0;

    if (status == 0)
        return;
    for (uint32_t retry = 0; retry < 10U; retry++) {
        mmio_write32(SDCC2_BASE, SDHCI_CORE_PWR_CLEAR, status);
        if ((mmio_read32(SDCC2_BASE, SDHCI_CORE_PWR_STATUS) & status) == 0)
            break;
        sdcc2_delay_us(10);
    }
    if (status & 0x3U)
        ack |= 0x1U;
    if (status & 0xcU)
        ack |= 0x4U;
    mmio_write32(SDCC2_BASE, SDHCI_CORE_PWR_CTL, ack);
}

static int32_t sdcc2_reset(void) {
    mmio_write8(SDCC2_BASE, SDHCI_SOFTWARE_RESET, 1U);
    for (uint32_t i = 0; i < 100U; i++) {
        sdcc2_handle_power();
        if ((mmio_read8(SDCC2_BASE, SDHCI_SOFTWARE_RESET) & 1U) == 0)
            return 0;
        sdcc2_delay_us(1000);
    }
    return -1;
}

static int32_t sdcc2_enable_card_clock(void) {
    uint32_t clk;

    mmio_write16(SDCC2_BASE, SDHCI_CLOCK_CONTROL, 0);
    mmio_write16(SDCC2_BASE, SDHCI_CLOCK_CONTROL, 1U);
    for (uint32_t i = 0; i < 20U; i++) {
        clk = mmio_read16(SDCC2_BASE, SDHCI_CLOCK_CONTROL);
        if (clk & 2U) {
            mmio_write16(SDCC2_BASE, SDHCI_CLOCK_CONTROL, clk | 4U);
            return 0;
        }
        sdcc2_delay_us(1000);
    }
    return -1;
}

static void sdcc2_reset_lines(uint32_t mask) {
    mmio_write8(SDCC2_BASE, SDHCI_SOFTWARE_RESET, mask & 0x06U);
    /* SDHCI specifies up to 100 ms for command/data line reset. */
    for (uint32_t i = 0; i < 100000U; i++) {
        if ((mmio_read8(SDCC2_BASE, SDHCI_SOFTWARE_RESET) & mask) == 0)
            return;
        sdcc2_delay_us(1);
    }
}

static int32_t sdcc2_wait_idle(void) {
    for (uint32_t i = 0; i < 100000U; i++) {
        if ((mmio_read32(SDCC2_BASE, SDHCI_PRESENT_STATE) & 3U) == 0)
            return 0;
        sdcc2_delay_us(1);
    }
    return -1;
}

static void sdcc2_recover_transfer(void) {
    mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, 0xffffffffU);
    sdcc2_reset_lines(0x06U);
    /* A command timeout can leave the card clock gated by the host. */
    (void)sdcc2_enable_card_clock();
    sdcc2_delay_us(2000);
}

static int32_t sdcc2_cmd(uint32_t index, uint32_t arg, uint32_t flags,
        uint32_t* response, uint32_t* status_out) {
    uint32_t status = 0;
    uint32_t inhibit = 1U;

    if (flags & 0x20U)
        inhibit |= 2U;

    for (uint32_t i = 0; i < 100000U; i++) {
        if ((mmio_read32(SDCC2_BASE, SDHCI_PRESENT_STATE) & inhibit) == 0)
            break;
        if (i == 99999U) {
            if (status_out)
                *status_out = mmio_read32(SDCC2_BASE, SDHCI_PRESENT_STATE);
            return -1;
        }
        sdcc2_delay_us(1);
    }

    mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, 0xffffffffU);
    mmio_write32(SDCC2_BASE, SDHCI_ARGUMENT, arg);
    mmio_write16(SDCC2_BASE, SDHCI_COMMAND,
            ((index & 0x3fU) << 8) | (flags & 0xffU));

    for (uint32_t i = 0; i < 100000U; i++) {
        status = mmio_read32(SDCC2_BASE, SDHCI_INT_STATUS);
        if (status & (1U | 0xffff0000U))
            break;
        sdcc2_delay_us(1);
    }
    if (status_out)
        *status_out = status;
    if ((status & 1U) == 0 || (status & 0xffff0000U) != 0) {
        mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, status);
        sdcc2_reset_lines((flags & 0x20U) ? 0x06U : 0x02U);
        return -1;
    }
    if (response)
        *response = mmio_read32(SDCC2_BASE, SDHCI_RESPONSE);
    mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, status);
    if ((flags & 3U) == 3U) {
        for (uint32_t i = 0; i < 1000000U; i++) {
            if ((mmio_read32(SDCC2_BASE, SDHCI_PRESENT_STATE) & 2U) == 0)
                return 0;
            sdcc2_delay_us(1);
        }
        if (status_out)
            *status_out = mmio_read32(SDCC2_BASE, SDHCI_PRESENT_STATE);
        sdcc2_reset_lines(0x04U);
        return -1;
    }
    return 0;
}

static int32_t sdcc2_cmd_retry(uint32_t index, uint32_t arg,
        uint32_t flags, uint32_t* response, uint32_t* status_out) {
    for (uint32_t i = 0; i < 4U; i++) {
        if (sdcc2_cmd(index, arg, flags, response, status_out) == 0)
            return 0;
        sdcc2_delay_us(1000);
    }
    return -1;
}

static int32_t sdcc2_wait_transfer_state(uint32_t rca,
        uint32_t* card_status) {
    uint32_t response = 0;
    uint32_t host_status = 0;

    for (uint32_t i = 0; i < 100U; i++) {
        if (sdcc2_cmd(13U, rca << 16, 0x1aU,
                    &response, &host_status) == 0) {
            if (card_status)
                *card_status = response;
            if ((response & (1U << 8)) != 0 &&
                    ((response >> 9) & 0xfU) == 4U)
                return 0;
        }
        sdcc2_delay_us(1000);
    }
    if (card_status)
        *card_status = host_status != 0 ? host_status : response;
    return -1;
}

static void sdcc2_response136(uint32_t response[4]) {
    for (uint32_t i = 0; i < 4U; i++)
        response[i] = mmio_read32(SDCC2_BASE,
                SDHCI_RESPONSE + (3U - i) * 4U);
    for (uint32_t i = 0; i < 4U; i++) {
        response[i] <<= 8;
        if (i != 3U)
            response[i] |= response[i + 1U] >> 24;
    }
}

static uint32_t sd_unstuff_bits(const uint32_t response[4], uint32_t start,
        uint32_t size) {
    uint32_t off = 3U - start / 32U;
    uint32_t shift = start & 31U;
    uint32_t mask = size < 32U ? (1U << size) - 1U : 0xffffffffU;
    uint32_t result = response[off] >> shift;

    if (size + shift > 32U)
        result |= response[off - 1U] << (32U - shift);
    return result & mask;
}

static uint64_t sdcc2_csd_capacity_sectors(const uint32_t csd[4]) {
    uint32_t structure = sd_unstuff_bits(csd, 126U, 2U);
    uint64_t capacity;

    if (structure == 1U || structure == 2U) {
        uint32_t bits = structure == 1U ? 22U : 28U;
        return ((uint64_t)sd_unstuff_bits(csd, 48U, bits) + 1ULL)
                << 10;
    }
    if (structure != 0U)
        return 0;

    capacity = ((uint64_t)sd_unstuff_bits(csd, 62U, 12U) + 1ULL)
            << (sd_unstuff_bits(csd, 47U, 3U) + 2U);
    uint32_t read_block_bits = sd_unstuff_bits(csd, 80U, 4U);
    if (read_block_bits >= 9U)
        return capacity << (read_block_bits - 9U);
    return capacity >> (9U - read_block_bits);
}

static void sdcc2_stop_transmission(void) {
    uint32_t response;
    uint32_t status;

    (void)sdcc2_cmd(12U, 0, 0x1bU, &response, &status);
}


/*
 * SDMA.  The sys_dma pool is physically contiguous, non-cacheable and below
 * 4GB, so SDHCI's 32-bit DMA address register can point straight at it.  PIO
 * at 4-bit/50MHz is marginal; SDMA lets the controller move a whole chunk and
 * leaves the CPU to copy the result once.
 */
#define SDCC2_DMA_BLOCKS 8U

static uint8_t* _dma_va = NULL;
static uint32_t _dma_pa = 0;
static int32_t _dma_tried = 0;
static int32_t _dma_ok = 0;

static __attribute__((unused)) int32_t sdcc2_dma_setup(void) {
    ewokos_addr_t va;
    ewokos_addr_t pa;

    if(_dma_tried)
        return _dma_ok ? 0 : -1;
    _dma_tried = 1;

    va = dma_alloc(0, SDCC2_DMA_BLOCKS * 512U);
    if(va == 0)
        return -1;
    pa = dma_phy_addr(0, va);
    if(pa == 0 || pa > 0xffffffffULL) {
        dma_free(0, va);
        return -1;
    }
    _dma_va = (uint8_t*)(uintptr_t)va;
    _dma_pa = (uint32_t)pa;
    _dma_ok = 1;
    return 0;
}

static __attribute__((unused)) int32_t sdcc2_dma_xfer(int write, uint32_t address,
        uint8_t* buf, uint32_t count) {
    uint32_t status = 0;
    uint32_t opcode;
    uint32_t transfer_mode;
    uint32_t i;

    if(count == 0 || count > SDCC2_DMA_BLOCKS || _dma_va == NULL)
        return -1;
    if(sdcc2_wait_idle() != 0)
        return -1;
    if(write)
        memcpy(_dma_va, buf, count * 512U);

    mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, 0xffffffffU);
    /* HOST_CONTROL[3] = 0 selects SDMA (not ADMA2). */
    mmio_write8(SDCC2_BASE, SDHCI_HOST_CONTROL,
            mmio_read8(SDCC2_BASE, SDHCI_HOST_CONTROL) & ~0x08U);
    mmio_write32(SDCC2_BASE, SDHCI_DMA_ADDRESS, _dma_pa);
    mmio_write16(SDCC2_BASE, SDHCI_BLOCK_SIZE, 512U);
    mmio_write16(SDCC2_BASE, SDHCI_BLOCK_COUNT, count);
    mmio_write32(SDCC2_BASE, SDHCI_ARGUMENT, address);

    opcode = write ? (count == 1U ? 24U : 25U) :
            (count == 1U ? 17U : 18U);
    transfer_mode = 0x01U | 0x02U;             /* DMA, block count */
    if(count > 1U)
        transfer_mode |= 0x20U;                /* multi-block */
    if(!write)
        transfer_mode |= 0x10U;                /* card -> host */
    mmio_write16(SDCC2_BASE, SDHCI_TRANSFER_MODE, transfer_mode);
    mmio_write16(SDCC2_BASE, SDHCI_COMMAND, (opcode << 8) | 0x3aU);

    for(i = 0; i < 2000000U; i++) {
        status = mmio_read32(SDCC2_BASE, SDHCI_INT_STATUS);
        if(status & (2U | 0xffff0000U))
            break;
        sdcc2_delay_us(1);
    }
    if((status & 2U) == 0 || (status & 0xffff0000U) != 0) {
        printf("sddma: fail addr=%u cnt=%u st=%08x\n", (unsigned)address,
                (unsigned)count, (unsigned)status);
        mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, status);
        sdcc2_reset_lines(0x06U);
        if(count > 1U)
            sdcc2_stop_transmission();
        return -1;
    }
    printf("sddma: ok addr=%u cnt=%u\n", (unsigned)address, (unsigned)count);
    mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, 2U);

    /* Historically this driver did not send CMD12 after SDMA.  The manual
     * CMD12 added during the HS work is what hangs the bus on this board,
     * so keep the pre-HS behaviour for the restore. */
    if(!write)
        memcpy(buf, _dma_va, count * 512U);
    return 0;
}

/*
 * ADMA2.  caps0 = 0x362dc8b2: ADMA2 (bit19) and 64-bit (bit28) are set,
 * SDMA (bit22) is clear, which is why the old SDMA path hung.  One 16-byte
 * descriptor covers the whole chunk (<=16KB); the descriptor table and data
 * buffer both live in the non-cacheable sys_dma pool.
 */
#if SDCC2_USE_ADMA
#if SDCC2_ADMA64
typedef struct {
    uint16_t attr;
    uint16_t len;
    uint32_t lo;
    uint32_t hi;
} __attribute__((packed)) sdcc2_adma2_desc_t;
#else
typedef struct {
    uint16_t attr;
    uint16_t len;
    uint32_t lo;
} __attribute__((packed)) sdcc2_adma2_desc_t;
#endif

#define SDCC2_ADMA2_ATTR (0x01U | 0x02U | 0x20U)  /* valid|end|tran */

static uint8_t* _adma_data_va;
static uint32_t _adma_data_pa;
static uint8_t* _adma_desc_va;
static uint32_t _adma_desc_pa;
static int32_t _adma_tried;
static int32_t _adma_ok;

static __attribute__((unused)) int32_t sdcc2_adma_setup(void) {
    ewokos_addr_t va;
    ewokos_addr_t pa;
    ewokos_addr_t dva;
    ewokos_addr_t dpa;

    if (_adma_tried)
        return _adma_ok ? 0 : -1;
    _adma_tried = 1;

    va = dma_alloc(0, SDCC2_ADMA_BLOCKS * 512U);
    if (va == 0)
        return -1;
    pa = dma_phy_addr(0, va);
    if (pa == 0 || pa > 0xffffffffULL) {
        dma_free(0, va);
        return -1;
    }
    dva = dma_alloc(0, 4096U);
    if (dva == 0) {
        dma_free(0, va);
        return -1;
    }
    dpa = dma_phy_addr(0, dva);
    if (dpa == 0 || dpa > 0xffffffffULL) {
        dma_free(0, dva);
        dma_free(0, va);
        return -1;
    }
    _adma_data_va = (uint8_t*)(uintptr_t)va;
    _adma_data_pa = (uint32_t)pa;
    _adma_desc_va = (uint8_t*)(uintptr_t)dva;
    _adma_desc_pa = (uint32_t)dpa;
    _adma_ok = 1;
    return 0;
}

static __attribute__((unused)) int32_t sdcc2_adma_xfer(int write,
        uint32_t address, uint8_t* buf, uint32_t count) {
    sdcc2_adma2_desc_t* d;
    uint32_t status = 0;
    uint32_t opcode;
    uint32_t mode;
    uint32_t i;

    if (count == 0 || count > SDCC2_ADMA_BLOCKS || _adma_data_pa == 0)
        return -1;
    if (sdcc2_wait_idle() != 0)
        return -1;
    if (write)
        memcpy(_adma_data_va, buf, count * 512U);

    d = (sdcc2_adma2_desc_t*)_adma_desc_va;
    d->attr = SDCC2_ADMA2_ATTR;
    d->len = (uint16_t)(count * 512U);
    d->lo = _adma_data_pa;
#if SDCC2_ADMA64
    d->hi = 0;
#endif
    __asm__ volatile("dsb sy" ::: "memory");

    mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, 0xffffffffU);
    mmio_write8(SDCC2_BASE, SDHCI_HOST_CONTROL,
            mmio_read8(SDCC2_BASE, SDHCI_HOST_CONTROL) | 0x08U);
    mmio_write32(SDCC2_BASE, SDHCI_ADMA_ADDRESS, _adma_desc_pa);
#if SDCC2_ADMA64
    mmio_write32(SDCC2_BASE, SDHCI_ADMA_ADDRESS_HI, 0U);
#endif
    mmio_write16(SDCC2_BASE, SDHCI_BLOCK_SIZE, 512U);
    mmio_write16(SDCC2_BASE, SDHCI_BLOCK_COUNT, count);
    mmio_write32(SDCC2_BASE, SDHCI_ARGUMENT, address);

    opcode = write ? (count == 1U ? 24U : 25U) :
            (count == 1U ? 17U : 18U);
    mode = 0x01U | 0x02U;                 /* ADMA2 | block count */
    if (count > 1U)
        mode |= 0x20U;                    /* multi-block */
    if (!write)
        mode |= 0x10U;                    /* card -> host */
    mmio_write16(SDCC2_BASE, SDHCI_TRANSFER_MODE, mode);
    mmio_write16(SDCC2_BASE, SDHCI_COMMAND, (opcode << 8) | 0x3aU);

    for (i = 0; i < 2000000U; i++) {
        status = mmio_read32(SDCC2_BASE, SDHCI_INT_STATUS);
        if (status & (2U | 0xffff0000U))
            break;
        sdcc2_delay_us(1);
    }
    if ((status & 2U) == 0 || (status & 0xffff0000U) != 0) {
        printf("sdadma: fail addr=%u cnt=%u st=%08x\n", (unsigned)address,
                (unsigned)count, (unsigned)status);
        mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, status);
        sdcc2_reset_lines(0x06U);
        if (count > 1U)
            sdcc2_stop_transmission();
        return -1;
    }
    mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, status);
    if (count > 1U)
        sdcc2_stop_transmission();
    if (!write)
        memcpy(buf, _adma_data_va, count * 512U);
    return 0;
}

static __attribute__((unused)) int32_t sdcc2_write_blocks_adma(
        uint32_t address, const uint8_t* buf, uint32_t count) {
    return sdcc2_adma_xfer(1, address, (uint8_t*)(uintptr_t)buf, count);
}
#endif /* SDCC2_USE_ADMA */

static int32_t sdcc2_read_blocks_pio(uint32_t address, uint8_t* buf,

        uint32_t count, uint32_t* status_out) {
    uint32_t status = 0;
    uint32_t command;
    uint32_t response;
    uint32_t opcode;
    uint32_t transfer_mode;

    if (buf == NULL || count == 0 || count > 0xffffU)
        return -1;
    opcode = count == 1U ? 17U : 18U;
    /* SDHCI_TRNS_READ (0x10) selects card-to-host transfer.  Without it the
     * controller reports BUFFER_WRITE_READY (0x10) for CMD17/CMD18 and the
     * PIO read loop times out before receiving any data.
     *
     * Multi-block uses 0x32 (read | block count | multi), not 0x36: bit 2 is
     * Auto CMD12, which this SDCC2 does not advertise (CAPABILITIES bit 6
     * clear), so the transfer never completes.  STOP_TRANSMISSION is sent
     * manually after TRANSFER_COMPLETE instead. */
    transfer_mode = count == 1U ? 0x12U : 0x32U;

    if (sdcc2_wait_idle() != 0) {
        if (status_out)
            *status_out = mmio_read32(SDCC2_BASE, SDHCI_PRESENT_STATE);
        return -1;
    }

    for (uint32_t attempt = 0; attempt < 3U; attempt++) {
        mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, 0xffffffffU);
        mmio_write16(SDCC2_BASE, SDHCI_BLOCK_SIZE, 512U);
        mmio_write16(SDCC2_BASE, SDHCI_BLOCK_COUNT, count);
        mmio_write32(SDCC2_BASE, SDHCI_ARGUMENT, address);
        command = (opcode << 8) | 0x3aU;
        /* Match Linux sdhci_send_command(): program TRANSFER_MODE first and
         * then COMMAND.  They are adjacent 16-bit registers; a single 32-bit
         * write can make SDCC2 latch the command before its data mode. */
        mmio_write16(SDCC2_BASE, SDHCI_TRANSFER_MODE, transfer_mode);
        mmio_write16(SDCC2_BASE, SDHCI_COMMAND, command);

        for (uint32_t i = 0; i < 1000000U; i++) {
            status = mmio_read32(SDCC2_BASE, SDHCI_INT_STATUS);
            /* SDCC2 can expose BUFFER_READ_READY after consuming the command
             * completion bit.  Data readiness is sufficient proof that the
             * data command and its response were accepted. */
            if (status & (1U | 0x20U | 0xffff0000U))
                break;
            sdcc2_delay_us(1);
        }
        if ((status & (1U | 0x20U)) != 0 &&
                (status & 0xffff0000U) == 0)
            break;

        response = mmio_read32(SDCC2_BASE, SDHCI_RESPONSE);
        if (attempt == 2U)
            goto failed;

        /* Recover from both command-index errors and command timeouts. */
        sdcc2_recover_transfer();
        if (count > 1U)
            sdcc2_stop_transmission();
        if (sdcc2_wait_idle() != 0)
            goto failed;
        if (_physical_sd_rca != 0 &&
                sdcc2_wait_transfer_state(_physical_sd_rca,
                    &response) != 0)
            goto failed;
    }
    if ((status & (1U | 0x20U)) == 0 ||
            (status & 0xffff0000U) != 0)
        goto failed;
    if (status & 1U)
        mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, 1U);

    for (uint32_t block = 0; block < count; block++) {
        for (uint32_t i = 0; i < 1000000U; i++) {
            status = mmio_read32(SDCC2_BASE, SDHCI_INT_STATUS);
            if (status & (0x20U | 0xffff0000U))
                break;
            sdcc2_delay_us(1);
        }
        if ((status & 0x20U) == 0 || (status & 0xffff0000U) != 0)
            goto failed;
        for (uint32_t i = 0; i < 128U; i++) {
            uint32_t word = mmio_read32(SDCC2_BASE, SDHCI_BUFFER);
            uint32_t offset = block * 512U + i * 4U;
            buf[offset] = (uint8_t)word;
            buf[offset + 1U] = (uint8_t)(word >> 8);
            buf[offset + 2U] = (uint8_t)(word >> 16);
            buf[offset + 3U] = (uint8_t)(word >> 24);
        }
    }
    /* Keep BUFFER_READ_READY asserted while draining the FIFO (clearing it
     * per block stalls SDCC2 at high clock rates), acknowledge it once, then
     * wait for TRANSFER_COMPLETE. */
    mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, 0x20U);
    for (uint32_t i = 0; i < 1000000U; i++) {
        status = mmio_read32(SDCC2_BASE, SDHCI_INT_STATUS);
        if (status & (2U | 0xffff0000U))
            break;
        sdcc2_delay_us(1);
    }
    if ((status & 2U) == 0 || (status & 0xffff0000U) != 0)
        goto failed;
    mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, 2U);
    if (count > 1U)
        sdcc2_stop_transmission();
    if (status_out)
        *status_out = status;
    return 0;

failed:
    if (status_out)
        *status_out = status;
    mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, status);
    sdcc2_reset_lines(0x06U);
    if (count > 1U)
        sdcc2_stop_transmission();
    return -1;
}

static int32_t sdcc2_read_block(uint32_t address, uint8_t* buf,
        uint32_t* status_out) {
    return sdcc2_read_blocks_pio(address, buf, 1U, status_out);
}

static int32_t sdcc2_write_blocks_pio(uint32_t address, const uint8_t* buf,
        uint32_t count, uint32_t* status_out) {
    uint32_t status = 0;
    uint32_t opcode;
    uint32_t transfer_mode;
    uint32_t command;

    if (buf == NULL || count == 0 || count > 32U)
        return -1;
    opcode = count == 1U ? 24U : 25U;
    /* Writes must leave SDHCI_TRNS_READ clear: card receives host data.
     * 0x22 = block count | multi-block; bit 2 (Auto CMD12) is not supported
     * by this SDCC2, so CMD12 is issued after TRANSFER_COMPLETE. */
    transfer_mode = count == 1U ? 0x02U : 0x22U;

    if (sdcc2_wait_idle() != 0)
        return -1;

    for (uint32_t attempt = 0; attempt < 3U; attempt++) {
        mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, 0xffffffffU);
        mmio_write16(SDCC2_BASE, SDHCI_BLOCK_SIZE, 512U);
        mmio_write16(SDCC2_BASE, SDHCI_BLOCK_COUNT, count);
        mmio_write32(SDCC2_BASE, SDHCI_ARGUMENT, address);
        command = (opcode << 8) | 0x3aU;
        mmio_write16(SDCC2_BASE, SDHCI_TRANSFER_MODE, transfer_mode);
        mmio_write16(SDCC2_BASE, SDHCI_COMMAND, command);

        for (uint32_t i = 0; i < 1000000U; i++) {
            status = mmio_read32(SDCC2_BASE, SDHCI_INT_STATUS);
            if (status & (1U | 0x10U | 0xffff0000U))
                break;
            sdcc2_delay_us(1);
        }
        if ((status & (1U | 0x10U)) != 0 &&
                (status & 0xffff0000U) == 0)
            break;
        if (attempt == 2U)
            goto failed;
        sdcc2_recover_transfer();
        if (count > 1U)
            sdcc2_stop_transmission();
        if (sdcc2_wait_idle() != 0)
            goto failed;
        if (_physical_sd_rca != 0 &&
                sdcc2_wait_transfer_state(_physical_sd_rca, NULL) != 0)
            goto failed;
    }
    if ((status & (1U | 0x10U)) == 0 ||
            (status & 0xffff0000U) != 0)
        goto failed;
    if (status & 1U)
        mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, 1U);

    for (uint32_t block = 0; block < count; block++) {
        for (uint32_t i = 0; i < 1000000U; i++) {
            status = mmio_read32(SDCC2_BASE, SDHCI_INT_STATUS);
            if (status & (0x10U | 0xffff0000U))
                break;
            sdcc2_delay_us(1);
        }
        if ((status & 0x10U) == 0 || (status & 0xffff0000U) != 0)
            goto failed;
        for (uint32_t i = 0; i < 128U; i++) {
            uint32_t offset = block * 512U + i * 4U;
            uint32_t word = (uint32_t)buf[offset] |
                    ((uint32_t)buf[offset + 1U] << 8) |
                    ((uint32_t)buf[offset + 2U] << 16) |
                    ((uint32_t)buf[offset + 3U] << 24);
            mmio_write32(SDCC2_BASE, SDHCI_BUFFER, word);
        }
        mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, 0x10U);
    }

    for (uint32_t i = 0; i < 1000000U; i++) {
        status = mmio_read32(SDCC2_BASE, SDHCI_INT_STATUS);
        if (status & (2U | 0xffff0000U))
            break;
        sdcc2_delay_us(1);
    }
    if ((status & 2U) == 0 || (status & 0xffff0000U) != 0)
        goto failed;
    mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, 2U);
    if (count > 1U)
        sdcc2_stop_transmission();
    if (status_out)
        *status_out = status;
    return 0;

failed:
    if (status_out)
        *status_out = status;
    mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, status);
    sdcc2_reset_lines(0x06U);
    if (count > 1U)
        sdcc2_stop_transmission();
    return -1;
}

static uint32_t sdcc2_sector_address(uint32_t sector) {
    return _physical_sd_high_capacity ? sector : sector * 512U;
}

/* CMD6 returns a 64-byte (512-bit) function-switch status block.  Linux's
 * mmc_sd_switch() uses mmc_send_adtc_data(..., 64); programming 512 bytes
 * makes SDCC2 wait for data the card never sends.  Byte 16 low nibble is the
 * selected function and byte 13 bit 1 is "High Speed supported". */
static int32_t sdcc2_cmd6_switch(uint32_t arg, uint8_t* switch_status) {
    uint32_t status = 0;
    uint32_t i;

    if (sdcc2_wait_idle() != 0)
        return -1;
    mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, 0xffffffffU);
    sdcc2_delay_us(2000);
    mmio_write16(SDCC2_BASE, SDHCI_BLOCK_SIZE, 64U);
    mmio_write16(SDCC2_BASE, SDHCI_BLOCK_COUNT, 1U);
    mmio_write32(SDCC2_BASE, SDHCI_ARGUMENT, arg);
    /* One 64-byte block: leave Block Count Enable clear. */
    mmio_write16(SDCC2_BASE, SDHCI_TRANSFER_MODE, 0x10U);
    mmio_write16(SDCC2_BASE, SDHCI_COMMAND, (6U << 8) | 0x3aU);

    for (i = 0; i < 1000000U; i++) {
        status = mmio_read32(SDCC2_BASE, SDHCI_INT_STATUS);
        if (status & (1U | 0x20U | 0xffff0000U))
            break;
        sdcc2_delay_us(1);
    }
    if ((status & (1U | 0x20U)) == 0 || (status & 0xffff0000U) != 0) {
        mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, status);
        sdcc2_reset_lines(0x06U);
        return -2;
    }

    sdcc2_delay_us(2000);
    for (i = 0; i < 16U; i++) {
        uint32_t word = mmio_read32(SDCC2_BASE, SDHCI_BUFFER);
        if (switch_status != NULL) {
            switch_status[i * 4U] = (uint8_t)word;
            switch_status[i * 4U + 1U] = (uint8_t)(word >> 8);
            switch_status[i * 4U + 2U] = (uint8_t)(word >> 16);
            switch_status[i * 4U + 3U] = (uint8_t)(word >> 24);
        }
    }
    /* Wait for TRANSFER_COMPLETE before acknowledging BUFFER_READ_READY:
     * clearing it immediately after the FIFO drain can cancel the
     * completion edge and leave the switch half-finished. */
    sdcc2_delay_us(2000);
    for (i = 0; i < 1000000U; i++) {
        status = mmio_read32(SDCC2_BASE, SDHCI_INT_STATUS);
        if (status & (2U | 0xffff0000U))
            break;
        sdcc2_delay_us(1);
    }
    if ((status & 2U) == 0 || (status & 0xffff0000U) != 0) {
        mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, status);
        sdcc2_reset_lines(0x06U);
        return -3;
    }
    mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, 0x22U);
    return 0;
}

static uint8_t _hs_switch_status[64];

static __attribute__((unused)) int32_t sdcc2_try_fast(uint32_t rca) {
    uint32_t response = 0;
    uint32_t status = 0;
    uint16_t clk;
    int32_t switched;

    /* Select 4-bit mode first, then request function group 1 = High Speed. */
    printf("sdhs: acmd6 4bit\n");
    if (sdcc2_cmd_retry(55, rca << 16, 0x1aU,
                &response, &status) != 0)
        return -1;
    if (sdcc2_cmd_retry(6, 2U, 0x1aU,
                &response, &status) != 0)
        return -1;
    mmio_write8(SDCC2_BASE, SDHCI_HOST_CONTROL,
            mmio_read8(SDCC2_BASE, SDHCI_HOST_CONTROL) | 2U);
    sdcc2_delay_us(10000);
    if (sdcc2_wait_transfer_state(rca, &response) != 0)
        return -1;
    sdcc2_delay_us(2000);

    /* CMD6's 64-byte data phase is occasionally lost on a warm handoff.
     * Abort, recover to transfer state and retry; the request is idempotent. */
    switched = 0;
    printf("sdhs: cmd6 hs\n");
    for (uint32_t attempt = 0; attempt < 3U; attempt++) {
        if (attempt != 0) {
            sdcc2_recover_transfer();
            (void)sdcc2_cmd_retry(12, 0, 0x1bU, &response, &status);
            if (sdcc2_wait_transfer_state(rca, &response) != 0)
                break;
            sdcc2_delay_us(5000);
        }
        if (sdcc2_cmd6_switch(0x80fffff1U, _hs_switch_status) == 0 &&
                (_hs_switch_status[16] & 0xfU) == 1U) {
            switched = 1;
            break;
        }
    }
    if (!switched) {
        printf("sdhs: cmd6 failed\n");
        return -1;
    }
    printf("sdhs: switch ok, set 50MHz\n");
    if (sdcc2_wait_transfer_state(rca, &response) != 0)
        return -1;

    /* SDHCI spec v3.00: disable the card clock before changing the High
     * Speed Enable bit, then re-enable it through the GCC divider. */
    clk = mmio_read16(SDCC2_BASE, SDHCI_CLOCK_CONTROL);
    if (clk & 4U)
        mmio_write16(SDCC2_BASE, SDHCI_CLOCK_CONTROL, clk & ~4U);
    mmio_write8(SDCC2_BASE, SDHCI_HOST_CONTROL,
            mmio_read8(SDCC2_BASE, SDHCI_HOST_CONTROL) | 4U);
    if (sdcc2_set_clock_50mhz() != 0)
        return -1;
    sdcc2_delay_us(2000);
    if (sdcc2_wait_transfer_state(rca, &response) != 0)
        return -1;
    printf("sdhs: cmd17 50MHz\n");
    if (sdcc2_read_block(0, _sd_probe_buf, &status) != 0)
        return -1;
    printf("sdhs: hs ok\n");
    return 0;
}

static __attribute__((unused)) void sdcc2_fast_to_slow(uint32_t rca) {
    uint32_t response = 0;
    uint32_t status = 0;

    /* A failed high-speed data command can leave the controller's data
     * inhibit bit set.  Reset the command/data lines before recovery. */
    sdcc2_reset_lines(0x06U);

    /* ACMD6(0) only changes bus width.  CMD6 function 1 must be explicitly
     * switched back before lowering the host clock. */
    if (sdcc2_cmd6_switch(0x80fffff0U, NULL) != 0)
        sdcc2_reset_lines(0x06U);

    if (sdcc2_cmd_retry(55, rca << 16, 0x1aU, &response, &status) == 0)
        (void)sdcc2_cmd_retry(6, 0U, 0x1aU, &response, &status);
    mmio_write8(SDCC2_BASE, SDHCI_HOST_CONTROL,
            mmio_read8(SDCC2_BASE, SDHCI_HOST_CONTROL) & ~6U);
    sdcc2_delay_us(10000);
    (void)sdcc2_set_clock_25mhz();
    if (sdcc2_wait_transfer_state(rca, &response) != 0) {
        sdcc2_reset_lines(0x06U);
        (void)sdcc2_set_clock_400khz();
        (void)sdcc2_enable_card_clock();
    }
}

static int32_t sdcc2_identify(void) {
    uint32_t response = 0;
    uint32_t status = 0;
    uint32_t cid[4];
    uint32_t csd[4];
    uint32_t ocr = 0;
    uint32_t rca;
    uint32_t detail;
    uint32_t init_errors = 0;
    int32_t ready = 0;
    uint32_t cd = mmio_read32(TLMM_BASE, TLMM_GPIO131_IO) & 1U;

    _physical_sd_ready = 0;
    _physical_sd_high_capacity = 0;
    _physical_sd_multi_ready = 0;
    _physical_sd_sectors = 0;
    _physical_sd_rca = 0;
    sd_set_diag(1, cd);
    /* GPIO131 can be stale after a warm reset; let CMD0/ACMD41 decide
     * whether a card is actually responding. */

    /* Linux restores this Qualcomm v5 register to its POR value at probe. */
    mmio_write32(SDCC2_BASE, SDHCI_CORE_VENDOR_SPEC,
            (0x00000a9cU | SDHCI_MSM_IO_PAD_ENABLE) &
            ~SDHCI_MSM_IO_PAD_1P8V);
    if (sdcc2_reset() != 0)
        return sd_fail(2, mmio_read8(SDCC2_BASE, SDHCI_SOFTWARE_RESET));
    uint32_t hc_mode = mmio_read32(SDCC2_BASE, SDHCI_CORE_HC_MODE);
    hc_mode |= SDHCI_CORE_HC_MODE_EN | SDHCI_CORE_FF_CLK_SW_RST_DIS;
    mmio_write32(SDCC2_BASE, SDHCI_CORE_HC_MODE, hc_mode);
    sd_set_diag(2, 0);

    mmio_write32(SDCC2_BASE, SDHCI_CORE_VENDOR_CAPS0,
            mmio_read32(SDCC2_BASE, SDHCI_CAPABILITIES) |
            (1U << 25) | (1U << 18));
    /* Start in the protocol's 1-bit mode.  The card and the host can retain
     * different bus-width state across a warm UEFI -> kernel handoff; issuing
     * ACMD6 and immediately doing CMD17 is intermittently rejected by the
     * SDCC2 v5 controller.  1-bit mode is sufficient for boot and gives us a
     * deterministic command/data path. */
    mmio_write8(SDCC2_BASE, SDHCI_HOST_CONTROL,
            mmio_read8(SDCC2_BASE, SDHCI_HOST_CONTROL) & ~6U);
    mmio_write32(SDCC2_BASE, SDHCI_INT_ENABLE, 0xffffffffU);
    mmio_write32(SDCC2_BASE, SDHCI_SIGNAL_ENABLE, 0);
    mmio_write8(SDCC2_BASE, SDHCI_TIMEOUT_CONTROL, 0xeU);
    mmio_write32(SDCC2_BASE, SDHCI_CORE_PWR_MASK, 0xfU);
    mmio_write8(SDCC2_BASE, SDHCI_POWER_CONTROL, 0x0dU);
    for (uint32_t i = 0; i < 10U; i++) {
        sdcc2_handle_power();
        sdcc2_delay_us(100);
    }
    sd_set_diag(3, mmio_read32(SDCC2_BASE, SDHCI_CORE_PWR_STATUS));

    if (sdcc2_enable_card_clock() != 0)
        return sd_fail(4, mmio_read16(SDCC2_BASE, SDHCI_CLOCK_CONTROL));
    sd_set_diag(4, mmio_read16(SDCC2_BASE, SDHCI_CLOCK_CONTROL));
    sdcc2_reset_lines(0x06U);
    mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, 0xffffffffU);
    sdcc2_delay_us(10000);

    if (sdcc2_cmd(0, 0, 0x00, NULL, &status) != 0)
        return sd_fail(5, status);
    sd_set_diag(5, status);
    /* A card left in transfer state by the UEFI driver occasionally ignores
     * the first GO_IDLE_STATE; a second one after the bus settles recovers it. */
    sdcc2_delay_us(10000);
    (void)sdcc2_cmd(0, 0, 0x00, NULL, &status);
    sdcc2_delay_us(10000);

    /* R7: short response + CRC + command index.  A legacy SD card may
     * reject CMD8; continue with the pre-SDHC ACMD41 argument in that case.
     * A transient post-power-up timeout is recoverable as well. */
    int32_t v2_card = 1;
    if (sdcc2_cmd_retry(8, 0x1aaU, 0x1aU, &response, &status) != 0 ||
            (response & 0xfffU) != 0x1aaU) {
        v2_card = 0;
        sd_set_diag(6, status != 0 ? status : response);
        sdcc2_reset_lines(0x06U);
        (void)sdcc2_cmd(0, 0, 0x00, NULL, &status);
        sdcc2_delay_us(10000);
    } else {
        sd_set_diag(6, response);
    }
    sdcc2_delay_us(10000);

    for (uint32_t i = 0; i < 1000U; i++) {
        if (sdcc2_cmd(55, 0, 0x1aU, &response, &status) != 0) {
            if (++init_errors >= 8U)
                return sd_fail(7, status);
            sdcc2_reset_lines(0x06U);
            (void)sdcc2_cmd(0, 0, 0x00, NULL, &status);
            sdcc2_delay_us(2000);
            if (v2_card)
                (void)sdcc2_cmd(8, 0x1aaU, 0x1aU, &response, &status);
            sdcc2_delay_us(5000);
            continue;
        }
        /* R3 has no CRC or command-index check. */
        if (sdcc2_cmd(41, v2_card ? 0x40ff8000U : 0x00ff8000U, 0x02U,
                    &response, &status) != 0) {
            if (++init_errors >= 8U)
                return sd_fail(8, status);
            sdcc2_delay_us(5000);
            continue;
        }
        init_errors = 0;
        if (response & 0x80000000U) {
            ocr = response;
            ready = 1;
        break;
        }
        sdcc2_delay_us(1000);
    }
    if (!ready)
        return sd_fail(9, response);
    _physical_sd_high_capacity = (ocr & (1U << 30)) != 0;
    sd_set_diag(9, ocr);

    /* R2 responses are 136 bits; SDHCI strips CRC and needs the Linux
     * left-shift reconstruction before CID/CSD fields are consumed. */
    if (sdcc2_cmd_retry(2, 0, 0x09U, NULL, &status) != 0)
        return sd_fail(14, status);
    sdcc2_response136(cid);
    sd_set_diag(14, cid[0]);

    if (sdcc2_cmd_retry(3, 0, 0x1aU, &response, &status) != 0)
        return sd_fail(15, status);
    rca = response >> 16;
    if (rca == 0)
        return sd_fail(15, response);
    _physical_sd_rca = rca;
    sd_set_diag(15, response);

    if (sdcc2_cmd_retry(9, rca << 16, 0x09U, NULL, &status) != 0)
        return sd_fail(16, status);
    sdcc2_response136(csd);
    _physical_sd_sectors = sdcc2_csd_capacity_sectors(csd);
    if (_physical_sd_sectors == 0)
        return sd_fail(16, csd[0]);
    sd_set_diag(16, csd[0]);

    if (sdcc2_cmd_retry(7, rca << 16, 0x1bU, &response, &status) != 0)
        return sd_fail(17, status);
    if (sdcc2_wait_transfer_state(rca, &response) != 0)
        return sd_fail(17, response);
    sd_set_diag(17, response);

    /* Keep the card in 1-bit mode during the initial data probe.  On this
     * board ACMD6/4-bit immediately before CMD17 is not reliable after a
     * warm boot, while 1-bit mode is sufficient to load the root filesystem.
     */
    sd_set_diag(18, 1U);

    if ((ocr & (1U << 30)) == 0 &&
            sdcc2_cmd_retry(16, 512U, 0x1aU,
                &response, &status) != 0)
        return sd_fail(19, status);
    if (sdcc2_wait_transfer_state(rca, &response) != 0)
        return sd_fail(19, response);
    if (sdcc2_read_block(0, _sd_probe_buf, &status) != 0) {
        /* Re-assert the card's 1-bit mode before giving up.  This also
         * recovers cards left in 4-bit mode by the preceding firmware. */
        if (sdcc2_cmd_retry(55, rca << 16, 0x1aU,
                    &response, &status) == 0 &&
                sdcc2_cmd_retry(6, 0U, 0x1aU,
                    &response, &status) == 0) {
            mmio_write8(SDCC2_BASE, SDHCI_HOST_CONTROL,
                    mmio_read8(SDCC2_BASE, SDHCI_HOST_CONTROL) & ~6U);
            sdcc2_delay_us(10000);
            if (sdcc2_read_block(0, _sd_probe_buf, &status) == 0) {
                sd_set_diag(20, 1U);
                goto first_block_ok;
            }
        }
        return sd_fail(20, status);
    }
first_block_ok:
    detail = ((uint32_t)_sd_probe_buf[450] << 16) |
            ((uint32_t)_sd_probe_buf[510] << 8) |
            _sd_probe_buf[511];
    sd_set_diag(20, detail);

    if (sdcc2_set_clock_25mhz() != 0)
        return sd_fail(21,
                mmio_read32(GCC_BASE, GCC_SDCC2_APPS_CMD_RCGR));
    if (sdcc2_wait_transfer_state(rca, &response) != 0) {
        sdcc2_restore_clock_400khz();
        sd_set_diag(21, response);
        return 0;
    }
    if (sdcc2_read_block(0, _sd_probe_buf, &status) != 0) {
        sdcc2_restore_clock_400khz();
        sd_set_diag(21, status);
        return 0;
    }
    detail = ((uint32_t)_sd_probe_buf[450] << 16) |
            ((uint32_t)_sd_probe_buf[510] << 8) |
            _sd_probe_buf[511];
    sd_set_diag(22, detail);

#if SDCC2_ENABLE_HS
    if (sdcc2_try_fast(rca) != 0)
        sdcc2_fast_to_slow(rca);
#else
    /* SDCC2_ENABLE_HS=0: leave the bus at 25MHz/1-bit, the configuration
     * that previously booted the rootfs and passed the user-space sdprobe. */
#endif

    if (sdcc2_read_block(sdcc2_sector_address(1U),
                _sd_probe_buf, &status) != 0)
        return sd_fail(23, status);
    detail = (uint32_t)_sd_probe_buf[0] |
            ((uint32_t)_sd_probe_buf[1] << 8) |
            ((uint32_t)_sd_probe_buf[2] << 16) |
            ((uint32_t)_sd_probe_buf[3] << 24);
    sd_set_diag(23, detail);

    if (sdcc2_read_blocks_pio(sdcc2_sector_address(1U),
                _sd_probe_multi_buf, 4U, &status) != 0) {
        sd_set_diag(23, detail);
        _physical_sd_ready = 1;
        return 0;
    }
    if (((uint32_t)_sd_probe_multi_buf[0] |
                ((uint32_t)_sd_probe_multi_buf[1] << 8) |
                ((uint32_t)_sd_probe_multi_buf[2] << 16) |
                ((uint32_t)_sd_probe_multi_buf[3] << 24)) != 0x20494645U)
        return sd_fail(24, (uint32_t)_sd_probe_multi_buf[0] |
                ((uint32_t)_sd_probe_multi_buf[1] << 8) |
                ((uint32_t)_sd_probe_multi_buf[2] << 16) |
                ((uint32_t)_sd_probe_multi_buf[3] << 24));
    _physical_sd_multi_ready = SDCC2_FORCE_SINGLE_BLOCK ? 0 : 1;
    _physical_sd_ready = 1;
    sd_set_diag(24, (uint32_t)_physical_sd_sectors);
    return 0;
}

static int32_t sdcc2_map_window(uint64_t va, uint64_t pa, uint32_t size) {
    ewokos_addr_t ret = syscall3(SYS_MEM_MAP, (ewokos_addr_t)va,
            (ewokos_addr_t)pa, (ewokos_addr_t)size);
    return ret == (ewokos_addr_t)va ? 0 : -1;
}

static int32_t sdcc2_map_windows(void) {
    /* GCC (controller clocks) lives in the low 64MB window; SDCC2, TLMM and
     * RPMh are separate physical windows but are mapped at the same offset
     * inside our base so the register macros match the kernel driver. */
    if(sdcc2_map_window(sdcc2_mmio_base + 0x00100000ULL,
                0x00100000ULL, 0x00020000U) != 0)
        return -1;
    if(sdcc2_map_window(sdcc2_mmio_base + Q8B_SDCC_WIN_OFF,
                Q8B_SDCC_WIN_PHY, Q8B_SDCC_WIN_SIZE) != 0)
        return -1;
    if(sdcc2_map_window(sdcc2_mmio_base + Q8B_TLMM_WIN_OFF,
                Q8B_TLMM_WIN_PHY, Q8B_TLMM_WIN_SIZE) != 0)
        return -1;
    if(sdcc2_map_window(sdcc2_mmio_base + Q8B_RSC_WIN_OFF,
                Q8B_RSC_WIN_PHY, Q8B_RSC_WIN_SIZE) != 0)
        return -1;
    if(sdcc2_map_window(sdcc2_mmio_base + Q8B_CMD_DB_WIN_OFF,
                Q8B_CMD_DB_WIN_PHY, Q8B_CMD_DB_WIN_SIZE) != 0)
        return -1;
    return 0;
}

/* Re-establish the MMIO mappings in a forked child.  Device mappings made
 * with SYS_MEM_MAP are not inherited across fork, but the SD controller and
 * libsd state are already initialized in the parent; the child only needs the
 * same virtual windows before it can read the rootfs. */
int32_t sdcc2_remap(void) {
    return sdcc2_map_windows();
}

/* Adopt a card that another process (init) already identified.  Only the
 * controller windows need to be re-mapped; the card stays powered and in the
 * transfer state, so no CMD0/ACMD41 sequence is repeated. */
void sdcc2_inherit(uint32_t high_capacity, uint32_t multi_ready,
        uint64_t sectors) {
    _physical_sd_ready = 1;
    _physical_sd_high_capacity = high_capacity ? 1 : 0;
    _physical_sd_multi_ready =
            SDCC2_FORCE_SINGLE_BLOCK ? 0 : (multi_ready ? 1 : 0);
    _physical_sd_sectors = sectors;
    _physical_sd_rca = 0;
    printf("sdcc2: inherit hc=%u multi=%u sectors=%u dma=%u\n",
            (unsigned)high_capacity, (unsigned)multi_ready,
            (unsigned)sectors, (unsigned)SDCC2_USE_SDMA);
}

void sdcc2_get_state(uint32_t* high_capacity, uint32_t* multi_ready,
        uint64_t* sectors) {
    if(high_capacity != NULL)
        *high_capacity = (uint32_t)_physical_sd_high_capacity;
    if(multi_ready != NULL)
        *multi_ready = (uint32_t)_physical_sd_multi_ready;
    if(sectors != NULL)
        *sectors = _physical_sd_sectors;
}

int32_t sdcc2_init(void) {
    if(sdcc2_map_windows() != 0)
        return sd_fail(0, 0);

    for (uint32_t attempt = 0; attempt < 3U; attempt++) {
        _physical_sd_ready = 0;
        printf("sdinit: attempt %u\n", (unsigned)attempt);
        if (sdcc2_set_clock_400khz() != 0) {
            (void)sd_fail(25,
                    mmio_read32(GCC_BASE, GCC_SDCC2_APPS_CMD_RCGR));
            sdcc2_delay_us(50000);
            continue;
        }
        sdcc2_config_pins();
        printf("sdinit: clock/pins ok\n");
        /* PXE does not use the SD device, so the firmware may already have
         * the card powered.  Try the existing rails first; only power-cycle
         * on later attempts (RPMH writes can time out after a warm EBS). */
        if (attempt != 0U && sdcc2_enable_rails() != 0) {
            sdcc2_delay_us(50000);
            continue;
        }
        printf("sdinit: identify\n");
        (void)sdcc2_identify();
        printf("sdinit: identify done ready=%u\n",
                (unsigned)_physical_sd_ready);
        if (_physical_sd_ready)
            return 0;
        sdcc2_delay_us(50000);
    }
    return -1;
}

int32_t sdcc2_ready(void) {
    return _physical_sd_ready;
}

uint64_t sdcc2_capacity_sectors(void) {
    return _physical_sd_sectors;
}

int32_t sdcc2_read_blocks_test(int32_t sector, void* buf, uint32_t count,
        uint32_t allow_multi) {
    int32_t old = _physical_sd_multi_ready;
    int32_t rc;

    _physical_sd_multi_ready = allow_multi ? 1 : 0;
    rc = sdcc2_read_blocks(sector, buf, count);
    _physical_sd_multi_ready = old;
    return rc;
}

int32_t sdcc2_read_blocks(int32_t sector, void* buf, uint32_t count) {
    uint8_t* out = (uint8_t*)buf;
    uint32_t block;
    uint32_t remaining;
    static uint32_t _read_log;

    if (sector < 0 || buf == NULL || count == 0 || !_physical_sd_ready)
        return -1;
#if SDCC2_USE_ADMA
    (void)sdcc2_adma_setup();
#endif
    if (_read_log < 12U) {
        printf("sdread: sec=%d cnt=%u multi=%u adma=%u\n", sector,
                (unsigned)count, (unsigned)_physical_sd_multi_ready,
                (unsigned)_adma_ok);
        _read_log++;
    }
    block = (uint32_t)sector;
    if ((uint64_t)block + count > _physical_sd_sectors)
        return -1;

#if SDCC2_USE_SDMA
    (void)sdcc2_dma_setup();
#endif
    remaining = count;
    while (remaining != 0) {
        /* PIO multi-block is never safe at HS; only ADMA2 may use a
         * chunk larger than one block. */
        uint32_t limit = 1U;
        uint32_t chunk;
        int32_t done = 0;

#if SDCC2_USE_ADMA
        if (_adma_ok)
            limit = SDCC2_ADMA_BLOCKS;
#endif
        chunk = remaining > limit ? limit : remaining;
        if (!_physical_sd_multi_ready)
            chunk = 1U;
#if SDCC2_USE_ADMA
        if (_adma_ok && chunk > 0U) {
            if (sdcc2_adma_xfer(0, sdcc2_sector_address(block),
                        out, chunk) == 0)
                done = 1;
            else {
                _adma_ok = 0;
                chunk = 1U;
            }
        }
#endif
#if SDCC2_USE_SDMA
        if (_dma_ok) {
            if (sdcc2_dma_xfer(0, sdcc2_sector_address(block),
                        out, chunk) == 0)
                done = 1;
            else
                _dma_ok = 0;
        }
#endif
        if (!done) {
            if (sdcc2_read_blocks_pio(sdcc2_sector_address(block),
                        out, chunk, NULL) != 0) {
                /* SDCC2's multi-block PIO path can stall; retry this chunk
                 * with one CMD17 before giving up. */
                printf("sdcc2: pio fail sector=%u count=%u multi=%u\n",
                        (unsigned)block, (unsigned)chunk,
                        (unsigned)_physical_sd_multi_ready);
                if (chunk <= 1U)
                    return -1;
                _physical_sd_multi_ready = 0;
                chunk = 1U;
                if (sdcc2_read_blocks_pio(sdcc2_sector_address(block),
                            out, 1U, NULL) != 0)
                    return -1;
            }
        }
        block += chunk;
        remaining -= chunk;
        out += chunk * 512U;
    }
    return 0;
}

int32_t sdcc2_write_blocks(int32_t sector, const void* buf, uint32_t count) {
    const uint8_t* in = (const uint8_t*)buf;

    if (sector < 0 || buf == NULL || count == 0 || !_physical_sd_ready)
        return -1;
    if ((uint64_t)(uint32_t)sector + count > _physical_sd_sectors)
        return -1;

    /* CMD25 remains unreliable on this SDCC2 controller: the data interrupt
     * can stay asserted after the first block.  Use the well-tested CMD24
     * path for each sector; SDMA moves the single 512-byte block. */
#if SDCC2_USE_SDMA
    (void)sdcc2_dma_setup();
#endif
    for (uint32_t i = 0; i < count; i++) {
        uint32_t addr = sdcc2_sector_address((uint32_t)sector + i);
        int32_t done = 0;

#if SDCC2_USE_SDMA
        if (_dma_ok) {
            if (sdcc2_dma_xfer(1, addr, (uint8_t*)(in + i * 512U),
                        1U) == 0)
                done = 1;
            else
                _dma_ok = 0;
        }
#endif
        if (!done && sdcc2_write_blocks_pio(addr,
                    in + i * 512U, 1U, NULL) != 0)
            return -1;
    }
    return 0;
}

int32_t sdcc2_write_blocks_test(int32_t sector, const void* buf,
        uint32_t count, uint32_t allow_multi) {
    const uint8_t* in = (const uint8_t*)buf;

    if (sector < 0 || buf == NULL || count == 0 || !_physical_sd_ready)
        return -1;
    if ((uint64_t)(uint32_t)sector + count > _physical_sd_sectors)
        return -1;
#if SDCC2_USE_ADMA
    (void)sdcc2_adma_setup();
    if (_adma_ok && allow_multi) {
        if (sdcc2_write_blocks_adma(sdcc2_sector_address((uint32_t)sector),
                    in, count) == 0)
            return 0;
        _adma_ok = 0;
    }
#else
    (void)allow_multi;
#endif
    return sdcc2_write_blocks(sector, buf, count);
}

int32_t sdcc2_read_sector(int32_t sector, void* buf) {
    return sdcc2_read_blocks(sector, buf, 1U);
}

int32_t sdcc2_write_sector(int32_t sector, const void* buf) {
    return sdcc2_write_blocks(sector, buf, 1U);
}
