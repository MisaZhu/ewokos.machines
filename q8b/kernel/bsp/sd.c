#include <dev/sd.h>
#include <kstring.h>
#include <stdint.h>
#include <stddef.h>
#include <kernel/hw_info.h>
#include <kernel/system.h>
#include <mm/mmu.h>
#include "hw_arch.h"
#include "q8b_platform.h"

extern uint64_t timer_read_sys_usec(void);

/*
 * The UEFI stub may load ROOTFS.IMG into RAM and report its physical range
 * in bootinfo.  That RAM disk remains the preferred backend for development;
 * when it is absent, the kernel reads init from the physical SDCC2 card.
 * The kernel is read-only here: all runtime card reads and writes are driven
 * by user space (system/libs/bsp/src/sdcc2.c), which owns the controller after
 * boot.  There is no SD block syscall.
 */

static int32_t _pending_sector = -1;
static uint8_t _write_buf[512];
static int32_t _physical_sd_ready = 0;
static int32_t _physical_sd_high_capacity = 0;
static int32_t _physical_sd_multi_ready = 0;
static uint64_t _physical_sd_sectors = 0;
static uint32_t _physical_sd_rca = 0;

#ifndef Q8B_QEMU_TEST
#define GCC_BASE                (MMIO_BASE + 0x00100000ULL)
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

#define SDCC2_BASE              (MMIO_BASE + Q8B_SDCC2_OFF)
#define TLMM_BASE               (MMIO_BASE + Q8B_TLMM_WIN_OFF)
#define TLMM_SDC2_CTL           0x0e8000
#define TLMM_GPIO131_CTL        (131 * 0x1000)
#define TLMM_GPIO131_IO         (TLMM_GPIO131_CTL + 0x4)

#define CMD_DB_BASE             (MMIO_BASE + Q8B_CMD_DB_WIN_OFF)
#define CMD_DB_DATA_OFFSET      144U
#define CMD_DB_RSC_OFFSET       8U
#define CMD_DB_RSC_SIZE         16U
#define CMD_DB_ENTRY_SIZE       24U
#define CMD_DB_MAX_SLAVES       8U
#define CMD_DB_MAGIC            0x0c0330dbU

#define RPMH_RSC_BASE           (MMIO_BASE + Q8B_RSC_WIN_OFF)
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
static uint8_t _sd_verify_buf[32U * 512U];

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
        _delay_usec(1);
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
        _delay_usec(1);
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
    /* Give the card enough time to discharge after a warm UEFI handoff. */
    _delay_usec(100000);
    /* Match the MMC core power-up order: card supply before I/O supply. */
    if (rpmh_enable_regulator(ldoc9, &detail) != 0)
        return sd_fail(12, detail);
    _delay_usec(30000);
    if (rpmh_enable_regulator(ldoc6, &detail) != 0)
        return sd_fail(13, detail);
    sd_set_diag(10, detail);
    _delay_usec(30000);
    return 0;
}

static void sd_set_diag(uint32_t state, uint32_t detail) {
    q8b_platform_data_t* pd =
        (q8b_platform_data_t*)(void*)_sys_info.platform_data;
    pd->sd.state = 0x53440000U | (state & 0xffffU);
    pd->sd.detail = detail;
}

static int32_t sd_fail(uint32_t stage, uint32_t detail) {
    sd_set_diag(0x8000U | (stage & 0x7fffU), detail);
    return -1;
}

static int32_t sdcc2_wait_gcc_clock(uint32_t reg) {
    for (uint32_t i = 0; i < 1000U; i++) {
        if ((mmio_read32(GCC_BASE, reg) & GCC_CBCR_CLOCK_OFF) == 0)
            return 0;
        _delay_usec(1);
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
        _delay_usec(1);
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
        _delay_usec(1);
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
    /* gcc_sdcc2_apps_clk_src: F(25000000, P_GCC_GPLL0_OUT_EVEN, 12).
     * GPLL0_OUT_EVEN is 300MHz, so the RCG divider field is 11 (0x060b).
     * The old 0x0617 value divided by 24 and produced 12.5MHz. */
    mmio_write16(SDCC2_BASE, SDHCI_CLOCK_CONTROL, 0);
    if (sdcc2_config_gcc_clock(0, 0, 0, 0x060bU) != 0)
        return -1;
    return sdcc2_enable_card_clock();
}

static int32_t sdcc2_set_clock_50mhz(void) {
    /* F(50000000, P_GCC_GPLL0_OUT_EVEN, 6): 300MHz / 6 = 50MHz,
     * RCG divider field 5 (0x0605).  The old 0x060b value produced 25MHz. */
    mmio_write16(SDCC2_BASE, SDHCI_CLOCK_CONTROL, 0);
    if (sdcc2_config_gcc_clock(0, 0, 0, 0x0605U) != 0)
        return -1;
    return sdcc2_enable_card_clock();
}

static void sdcc2_restore_clock_400khz(void) {
    mmio_write16(SDCC2_BASE, SDHCI_CLOCK_CONTROL, 0);
    (void)sdcc2_set_clock_400khz();
    (void)sdcc2_enable_card_clock();
    _delay_usec(1000);
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
        _delay_usec(10);
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
        _delay_usec(1000);
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
        _delay_usec(1000);
    }
    return -1;
}

static void sdcc2_reset_lines(uint32_t mask) {
    mmio_write8(SDCC2_BASE, SDHCI_SOFTWARE_RESET, mask & 0x06U);
    /* SDHCI specifies up to 100 ms for command/data line reset. */
    for (uint32_t i = 0; i < 100000U; i++) {
        if ((mmio_read8(SDCC2_BASE, SDHCI_SOFTWARE_RESET) & mask) == 0)
            return;
        _delay_usec(1);
    }
}

static int32_t sdcc2_wait_idle(void) {
    for (uint32_t i = 0; i < 100000U; i++) {
        if ((mmio_read32(SDCC2_BASE, SDHCI_PRESENT_STATE) & 3U) == 0)
            return 0;
        _delay_usec(1);
    }
    return -1;
}

static void sdcc2_recover_transfer(void) {
    mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, 0xffffffffU);
    sdcc2_reset_lines(0x06U);
    /* A command timeout can leave the card clock gated by the host. */
    (void)sdcc2_enable_card_clock();
    _delay_usec(2000);
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
        _delay_usec(1);
    }

    mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, 0xffffffffU);
    mmio_write32(SDCC2_BASE, SDHCI_ARGUMENT, arg);
    mmio_write16(SDCC2_BASE, SDHCI_COMMAND,
            ((index & 0x3fU) << 8) | (flags & 0xffU));

    for (uint32_t i = 0; i < 100000U; i++) {
        status = mmio_read32(SDCC2_BASE, SDHCI_INT_STATUS);
        if (status & (1U | 0xffff0000U))
            break;
        _delay_usec(1);
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
            _delay_usec(1);
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
        _delay_usec(1000);
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
        _delay_usec(1000);
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

static uint64_t sdcc2_capacity_sectors(const uint32_t csd[4]) {
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
     * Multi-block uses 0x32 (read | block count | multi), NOT 0x36.  Bit 2
     * is SDHCI_TRNS_AUTO_CMD12 and this SDCC2 does not advertise Auto CMD12
     * support (CAPABILITIES bit 6 is clear); setting it leaves the transfer
     * without a TRANSFER_COMPLETE and every CMD18 times out after the first
     * block.  The STOP_TRANSMISSION is issued manually at the end instead. */
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
            _delay_usec(1);
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
            _delay_usec(1);
        }
        if ((status & 0x20U) == 0 || (status & 0xffff0000U) != 0) {
            q8b_dbg_puts("sd: pio wait fail blk/cnt/st/present=");
            q8b_dbg_hex(block);
            q8b_dbg_puts("/");
            q8b_dbg_hex(count);
            q8b_dbg_puts("/");
            q8b_dbg_hex(status);
            q8b_dbg_puts("/");
            q8b_dbg_hex(mmio_read32(SDCC2_BASE, SDHCI_PRESENT_STATE));
            q8b_dbg_puts("\n");
            goto failed;
        }
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
     * per block stalls SDCC2 at high clock rates), then acknowledge it once
     * and wait for TRANSFER_COMPLETE. */
    mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, 0x20U);
    for (uint32_t i = 0; i < 1000000U; i++) {
        status = mmio_read32(SDCC2_BASE, SDHCI_INT_STATUS);
        if (status & (2U | 0xffff0000U))
            break;
        _delay_usec(1);
    }
    if ((status & 2U) == 0 || (status & 0xffff0000U) != 0) {
        q8b_dbg_puts("sd: pio tc fail st=");
        q8b_dbg_hex(status);
        q8b_dbg_puts("\n");
        goto failed;
    }
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

static uint32_t sdcc2_sector_address(uint32_t sector);

/* CMD6 returns a 64-byte (512-bit) function-switch status block.  Linux's
 * mmc_sd_switch() uses mmc_send_adtc_data(..., 64); programming 512 bytes
 * makes SDCC2 wait for data the card never sends and the switch times out.
 * The block is captured (when `switch_status` is given) so the caller can
 * verify that function group 1 really selected High Speed: byte 16 low
 * nibble is the selected function and byte 13 bit 1 is "supported". */
static uint8_t _cmd6_status[64];

static int32_t sdcc2_cmd6_switch(uint32_t arg, uint8_t* switch_status) {
    uint32_t status = 0;

    if (sdcc2_wait_idle() != 0)
        return -1;
    mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, 0xffffffffU);
    _delay_usec(2000);
    mmio_write16(SDCC2_BASE, SDHCI_BLOCK_SIZE, 64U);
    mmio_write16(SDCC2_BASE, SDHCI_BLOCK_COUNT, 1U);
    mmio_write32(SDCC2_BASE, SDHCI_ARGUMENT, arg);
    /* CMD6's data phase is one 64-byte block.  Do not set Block Count
     * Enable for a single block: SDCC2 waits for another block and never
     * raises TRANSFER_COMPLETE on some boots. */
    mmio_write16(SDCC2_BASE, SDHCI_TRANSFER_MODE, 0x10U);
    mmio_write16(SDCC2_BASE, SDHCI_COMMAND, (6U << 8) | 0x3aU);

    for (uint32_t i = 0; i < 1000000U; i++) {
        status = mmio_read32(SDCC2_BASE, SDHCI_INT_STATUS);
        if (status & (1U | 0x20U | 0xffff0000U))
            break;
        _delay_usec(1);
    }
    if ((status & (1U | 0x20U)) == 0 ||
            (status & 0xffff0000U) != 0) {
        q8b_dbg_puts("sd: cmd6 phase st=");
        q8b_dbg_hex(status);
        q8b_dbg_puts("\n");
        mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, status);
        sdcc2_reset_lines(0x06U);
        return -2;
    }

    _delay_usec(2000);
    for (uint32_t i = 0; i < 16U; i++) {
        uint32_t word = mmio_read32(SDCC2_BASE, SDHCI_BUFFER);
        if (switch_status != NULL) {
            switch_status[i * 4U] = (uint8_t)word;
            switch_status[i * 4U + 1U] = (uint8_t)(word >> 8);
            switch_status[i * 4U + 2U] = (uint8_t)(word >> 16);
            switch_status[i * 4U + 3U] = (uint8_t)(word >> 24);
        }
    }
    _delay_usec(2000);
    /* Let the controller finish the block before acknowledging the buffer
     * bit.  Clearing BUFFER_READ_READY right after the FIFO drain can cancel
     * the completion edge, so wait for TRANSFER_COMPLETE first. */
    for (uint32_t i = 0; i < 1000000U; i++) {
        status = mmio_read32(SDCC2_BASE, SDHCI_INT_STATUS);
        if (status & (2U | 0xffff0000U))
            break;
        _delay_usec(1);
    }
    if ((status & 2U) == 0 || (status & 0xffff0000U) != 0) {
        mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, status);
        sdcc2_reset_lines(0x06U);
        return -3;
    }
    mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, 0x22U);
    return 0;
}

static int32_t sdhs_fail(uint32_t stage) {
    q8b_dbg_puts("sd: hs fail stage=");
    q8b_dbg_hex(stage);
    q8b_dbg_puts("\n");
    return -1;
}

static int32_t sdcc2_try_fast(uint32_t rca) {
    uint32_t response = 0;
    uint32_t status = 0;
    uint16_t clk;

    /* Select 4-bit mode first, then request function group 1 = High Speed. */
    if (sdcc2_cmd_retry(55, rca << 16, 0x1aU,
                &response, &status) != 0)
        return sdhs_fail(1);
    if (sdcc2_cmd_retry(6, 2U, 0x1aU,
                &response, &status) != 0)
        return sdhs_fail(2);
    mmio_write8(SDCC2_BASE, SDHCI_HOST_CONTROL,
            mmio_read8(SDCC2_BASE, SDHCI_HOST_CONTROL) | 2U);
    _delay_usec(10000);
    if (sdcc2_wait_transfer_state(rca, &response) != 0)
        return sdhs_fail(3);
    _delay_usec(2000);

    /* CMD6's 64-byte data phase is occasionally lost on a warm handoff
     * (the controller reports BUFFER_READ_READY but the block reads back as
     * zeros).  Abort, recover the card to transfer state and retry; the
     * switch request is idempotent. */
    int32_t switched = 0;
    for (uint32_t attempt = 0; attempt < 3U; attempt++) {
        if (attempt != 0) {
            q8b_dbg_puts("sd: cmd6 retry\n");
            sdcc2_recover_transfer();
            (void)sdcc2_cmd_retry(12, 0, 0x1bU, &response, &status);
            if (sdcc2_wait_transfer_state(rca, &response) != 0)
                break;
            _delay_usec(5000);
        }
        if (sdcc2_cmd6_switch(0x80fffff1U, _cmd6_status) == 0 &&
                (_cmd6_status[16] & 0xfU) == 1U) {
            switched = 1;
            break;
        }
    }
    if (!switched)
        return sdhs_fail(4);
    if (sdcc2_wait_transfer_state(rca, &response) != 0)
        return sdhs_fail(6);

    /* SDHCI spec v3.00: disable the card clock before changing the High
     * Speed Enable bit, then re-enable it through the GCC divider. */
    clk = mmio_read16(SDCC2_BASE, SDHCI_CLOCK_CONTROL);
    if (clk & 4U)
        mmio_write16(SDCC2_BASE, SDHCI_CLOCK_CONTROL, clk & ~4U);
    mmio_write8(SDCC2_BASE, SDHCI_HOST_CONTROL,
            mmio_read8(SDCC2_BASE, SDHCI_HOST_CONTROL) | 4U);
    if (sdcc2_set_clock_50mhz() != 0)
        return sdhs_fail(7);
    /* Let the card see at least 74 clocks at the new speed before CMD13. */
    _delay_usec(2000);
    if (sdcc2_wait_transfer_state(rca, &response) != 0)
        return sdhs_fail(8);
    if (sdcc2_read_block(sdcc2_sector_address(1U),
                _sd_probe_buf, &status) != 0)
        return sdhs_fail(9);
    if (((uint32_t)_sd_probe_buf[0] |
                ((uint32_t)_sd_probe_buf[1] << 8) |
                ((uint32_t)_sd_probe_buf[2] << 16) |
                ((uint32_t)_sd_probe_buf[3] << 24)) != 0x20494645U)
        return sdhs_fail(10);
    return 0;
}

static void sdcc2_fast_to_slow(uint32_t rca) {
    uint32_t response = 0;
    uint32_t status = 0;

    /* A failed high-speed data command can leave the controller's data
     * inhibit bit set.  Reset the command/data lines before issuing the
     * function-switch command, otherwise the recovery command is silently
     * dropped by SDCC2. */
    sdcc2_reset_lines(0x06U);

    /* ACMD6(0) only changes the bus width.  It does not undo CMD6 function
     * 1, so explicitly switch the card back to the default-speed function
     * before lowering the host clock. */
    if (sdcc2_cmd6_switch(0x80fffff0U, NULL) != 0)
        sdcc2_reset_lines(0x06U);

    if (sdcc2_cmd_retry(55, rca << 16, 0x1aU,
                &response, &status) == 0)
        (void)sdcc2_cmd_retry(6, 0U, 0x1aU,
                &response, &status);
    mmio_write8(SDCC2_BASE, SDHCI_HOST_CONTROL,
            mmio_read8(SDCC2_BASE, SDHCI_HOST_CONTROL) & ~6U);
    _delay_usec(10000);
    (void)sdcc2_set_clock_25mhz();
    if (sdcc2_wait_transfer_state(rca, &response) != 0) {
        sdcc2_reset_lines(0x06U);
        (void)sdcc2_set_clock_400khz();
        (void)sdcc2_enable_card_clock();
    }
}

static uint32_t sdcc2_sector_address(uint32_t sector) {
    return _physical_sd_high_capacity ? sector : sector * 512U;
}

static int32_t sdcc2_try_4bit(uint32_t rca) {
    uint32_t response = 0;
    uint32_t status = 0;

    q8b_dbg_puts("sd: 4bit try\n");
    if (sdcc2_cmd_retry(55, rca << 16, 0x1aU, &response, &status) != 0) {
        q8b_dbg_puts("sd: 4bit acmd55 fail\n");
        return -1;
    }
    if (sdcc2_cmd_retry(6, 2U, 0x1aU, &response, &status) != 0) {
        q8b_dbg_puts("sd: 4bit acmd6 fail\n");
        return -1;
    }
    mmio_write8(SDCC2_BASE, SDHCI_HOST_CONTROL,
            mmio_read8(SDCC2_BASE, SDHCI_HOST_CONTROL) | 2U);
    _delay_usec(10000);
    if (sdcc2_wait_transfer_state(rca, &response) != 0) {
        q8b_dbg_puts("sd: 4bit cmd13 fail\n");
        return -1;
    }

    /* A 4-block CMD18 is enough to prove the 4-bit bus; if SDCC2 stalls in
     * the multi-block path, fall back to a single CMD17 instead of dropping
     * the whole 4-bit mode.  High Speed below re-validates multi-block. */
    int32_t rc = sdcc2_read_blocks_pio(sdcc2_sector_address(1U),
            _sd_verify_buf, 4U, &status);
    if (rc != 0) {
        q8b_dbg_puts("sd: 4bit read4 fail, retry single\n");
        rc = sdcc2_read_blocks_pio(sdcc2_sector_address(1U),
                _sd_verify_buf, 1U, &status);
    }
    if (rc != 0) {
        q8b_dbg_puts("sd: 4bit read fail status=");
        q8b_dbg_hex(status);
        q8b_dbg_puts("\n");
        return -1;
    }
    if (((uint32_t)_sd_verify_buf[0] |
                ((uint32_t)_sd_verify_buf[1] << 8) |
                ((uint32_t)_sd_verify_buf[2] << 16) |
                ((uint32_t)_sd_verify_buf[3] << 24)) != 0x20494645U) {
        q8b_dbg_puts("sd: 4bit read32 bad signature\n");
        return -1;
    }
    q8b_dbg_puts("sd: 4bit ok\n");
    return 0;
}

static void sdcc2_4bit_to_1bit(uint32_t rca) {
    uint32_t response = 0;
    uint32_t status = 0;

    if (sdcc2_cmd_retry(55, rca << 16, 0x1aU, &response, &status) == 0)
        (void)sdcc2_cmd_retry(6, 0U, 0x1aU, &response, &status);
    mmio_write8(SDCC2_BASE, SDHCI_HOST_CONTROL,
            mmio_read8(SDCC2_BASE, SDHCI_HOST_CONTROL) & ~6U);
    _delay_usec(10000);
    (void)sdcc2_set_clock_25mhz();
    (void)sdcc2_wait_transfer_state(rca, &response);
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
    /* GPIO131 is a board-level card-detect input, but it can remain high
     * across a warm UEFI reset even while the card is electrically present.
     * Do not make a stale detect level fatal; CMD0/ACMD41 below is the
     * authoritative presence test. */
    if (cd != 0)
        q8b_dbg_puts("sd: card-detect high, probing anyway\n");

    /* Linux restores this Qualcomm v5 register to its POR value at probe. */
    mmio_write32(SDCC2_BASE, SDHCI_CORE_VENDOR_SPEC,
            (0x00000a9cU | SDHCI_MSM_IO_PAD_ENABLE) &
            ~SDHCI_MSM_IO_PAD_1P8V);
    if (sdcc2_reset() != 0)
        return sd_fail(2, mmio_read8(SDCC2_BASE, SDHCI_SOFTWARE_RESET));
    /* SDCC v5 starts in legacy MCI mode after a PXE/EBS handoff.  Linux
     * explicitly enables the SDHCI host-controller mode and disables the
     * clock-controlled FIFO reset before issuing any card command. */
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
        _delay_usec(100);
    }
    sd_set_diag(3, mmio_read32(SDCC2_BASE, SDHCI_CORE_PWR_STATUS));

    if (sdcc2_enable_card_clock() != 0)
        return sd_fail(4, mmio_read16(SDCC2_BASE, SDHCI_CLOCK_CONTROL));
    sd_set_diag(4, mmio_read16(SDCC2_BASE, SDHCI_CLOCK_CONTROL));
    /* EBS can leave command/data inhibit asserted; clear it after the card
     * clock is running and before the identification sequence. */
    sdcc2_reset_lines(0x06U);
    mmio_write32(SDCC2_BASE, SDHCI_INT_STATUS, 0xffffffffU);
    _delay_usec(10000);

    if (sdcc2_cmd(0, 0, 0x00, NULL, &status) != 0)
        return sd_fail(5, status);
    sd_set_diag(5, status);
    /* A card left in transfer state by the UEFI driver occasionally ignores
     * the first GO_IDLE_STATE; a second one after the bus settles recovers it. */
    _delay_usec(10000);
    (void)sdcc2_cmd(0, 0, 0x00, NULL, &status);
    _delay_usec(10000);

    /* R7: short response + CRC + command index.  A legacy SD card may
     * reject CMD8; continue with the pre-SDHC ACMD41 argument in that case.
     * This also makes a transient post-power-up CMD8 timeout recoverable. */
    int32_t v2_card = 1;
    if (sdcc2_cmd_retry(8, 0x1aaU, 0x1aU, &response, &status) != 0 ||
            (response & 0xfffU) != 0x1aaU) {
        v2_card = 0;
        sd_set_diag(6, status != 0 ? status : response);
        sdcc2_reset_lines(0x06U);
        (void)sdcc2_cmd(0, 0, 0x00, NULL, &status);
        _delay_usec(10000);
    } else {
        sd_set_diag(6, response);
    }
    _delay_usec(10000);

    for (uint32_t i = 0; i < 1000U; i++) {
        if (sdcc2_cmd(55, 0, 0x1aU, &response, &status) != 0) {
            if (++init_errors >= 8U)
                return sd_fail(7, status);
            /* A firmware handoff can leave the card in transfer state.  A
             * delay alone cannot make CMD55 valid there; re-enter idle and
             * repeat CMD8 before retrying the ACMD preamble. */
            sdcc2_reset_lines(0x06U);
            (void)sdcc2_cmd(0, 0, 0x00, NULL, &status);
            _delay_usec(2000);
            if (v2_card)
                (void)sdcc2_cmd(8, 0x1aaU, 0x1aU, &response, &status);
            _delay_usec(5000);
            continue;
        }
        /* R3 has no CRC or command-index check. */
        if (sdcc2_cmd(41, v2_card ? 0x40ff8000U : 0x00ff8000U, 0x02U,
                    &response, &status) != 0) {
            if (++init_errors >= 8U)
                return sd_fail(8, status);
            _delay_usec(5000);
            continue;
        }
        init_errors = 0;
        if (response & 0x80000000U) {
            ocr = response;
            ready = 1;
        break;
        }
        _delay_usec(1000);
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
    _physical_sd_sectors = sdcc2_capacity_sectors(csd);
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
            _delay_usec(10000);
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

    if (sdcc2_try_4bit(rca) != 0)
        sdcc2_4bit_to_1bit(rca);

    /* Legacy High Speed: CMD6 function 1, 4-bit, 50MHz.  Validate it with a
     * single-block read; fall back to 25MHz/1-bit if the card or the board
     * cannot sustain it. */
    if (sdcc2_try_fast(rca) != 0)
        sdcc2_fast_to_slow(rca);
    {
        uint32_t cfg = mmio_read32(GCC_BASE, GCC_SDCC2_APPS_CFG_RCGR);
        uint32_t hc = mmio_read8(SDCC2_BASE, SDHCI_HOST_CONTROL);
        q8b_dbg_puts("sd: bus ");
        if ((cfg & 0x1fU) == 5U && (hc & 6U) == 6U)
            q8b_dbg_puts("50MHz 4-bit high speed");
        else
            q8b_dbg_puts("25MHz 1-bit fallback");
        q8b_dbg_puts("\n");
    }

    if (sdcc2_read_block(sdcc2_sector_address(1U),
                _sd_probe_buf, &status) != 0)
        return sd_fail(23, status);
    detail = (uint32_t)_sd_probe_buf[0] |
            ((uint32_t)_sd_probe_buf[1] << 8) |
            ((uint32_t)_sd_probe_buf[2] << 16) |
            ((uint32_t)_sd_probe_buf[3] << 24);
    sd_set_diag(23, detail);

    int32_t multi_rc = sdcc2_read_blocks_pio(sdcc2_sector_address(1U),
            _sd_probe_multi_buf, 4U, &status);
    if (multi_rc != 0) {
        q8b_dbg_puts("sd: CMD18 multi unavailable, single-block reads\n");
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
    /* Multi-block PIO is not reliable at 50MHz: a 16-block CMD18 was seen
     * returning silently corrupted data, so keep the kernel's init load on
     * the same single-block path the userspace driver verified. */
    _physical_sd_multi_ready = 0;
    _physical_sd_ready = 1;
    sd_set_diag(24, (uint32_t)_physical_sd_sectors);
    {
        uint32_t cfg = mmio_read32(GCC_BASE, GCC_SDCC2_APPS_CFG_RCGR);
        uint32_t hc = mmio_read8(SDCC2_BASE, SDHCI_HOST_CONTROL);
        q8b_dbg_puts("sd: ");
        if ((cfg & 0x1fU) == 5U && (hc & 6U) == 6U)
            q8b_dbg_puts("high speed 50MHz 4-bit");
        else
            q8b_dbg_puts("normal speed fallback");
        q8b_dbg_puts(", single-block reads, ");
        q8b_dbg_hex(_physical_sd_sectors);
        q8b_dbg_puts(" sectors\n");
    }

    /* Single-block CMD17 throughput: 512 blocks = 256KB.  Multi-block PIO
     * is not reliable on this controller; user space uses SDMA for bulk IO,
     * so this mainly validates the 50MHz clock and command path. */
    {
        uint64_t t0 = timer_read_sys_usec();
        uint32_t ok = 1;
        for (uint32_t i = 0; i < 512U; i++) {
            if (sdcc2_read_blocks_pio(sdcc2_sector_address(2048U + i),
                        _sd_probe_buf, 1U, &status) != 0) {
                ok = 0;
                break;
            }
        }
        uint64_t us = timer_read_sys_usec() - t0;
        if (!ok) {
            q8b_dbg_puts("sd: 256KB read benchmark failed\n");
        }
        else {
            q8b_dbg_puts("sd: read 256KB in ");
            q8b_dbg_hex(us);
            q8b_dbg_puts(" us (");
            q8b_dbg_hex(us != 0 ? (256ULL * 1000000ULL) / us : 0);
            q8b_dbg_puts(" KB/s)\n");
        }
    }
    return 0;
}

static void sdcc2_probe(void) {
    /* A warm UEFI -> kernel handoff can leave the card in a state where the
     * first ACMD41 preamble times out (stage 7, CMD_TIMEOUT).  The failure is
     * intermittent, so retry the complete power-cycle/identify sequence a few
     * times before giving up on the physical rootfs. */
    for (uint32_t attempt = 0; attempt < 3U; attempt++) {
        if (sdcc2_set_clock_400khz() != 0) {
            (void)sd_fail(25,
                    mmio_read32(GCC_BASE, GCC_SDCC2_APPS_CMD_RCGR));
            _delay_usec(50000);
            continue;
        }
        sdcc2_config_pins();

        /* PXE does not use the SD device, and firmware may already have the
         * card rails in the correct state.  Preserve that state for the
         * first probe; only force a PMIC power-cycle on later attempts. */
        if (attempt != 0U && sdcc2_enable_rails() != 0) {
            _delay_usec(50000);
            continue;
        }
        (void)sdcc2_identify();
        if (_physical_sd_ready)
            return;

        _delay_usec(50000);
    }
}
#endif

static void* rd_ptr(int32_t sector, uint32_t count) {
    uint64_t off = (uint64_t)(uint32_t)sector * 512ULL;
    uint64_t end = off + (uint64_t)count * 512ULL;
    if (q8b_ramdisk_size == 0 || end > q8b_ramdisk_size)
        return NULL;
    return (void*)P2V(q8b_ramdisk_base + off);
}

int32_t sd_init(void) {
#ifndef Q8B_QEMU_TEST
    /* With a firmware RAM disk the kernel does not need the card at all;
     * user space owns the controller and runs the full probe itself. */
    if (q8b_ramdisk_size == 0)
        sdcc2_probe();
#endif
    if (q8b_ramdisk_size != 0 || _physical_sd_ready)
        return 0;

    /* Keep the failure actionable on boards where the normal banner only
     * reports a generic [FAILED].  The stage is encoded by sd_fail(). */
    q8b_platform_data_t* pd =
        (q8b_platform_data_t*)(void*)_sys_info.platform_data;
    q8b_dbg_puts("sd: probe failed stage=");
    q8b_dbg_hex(pd->sd.state);
    q8b_dbg_puts(" detail=");
    q8b_dbg_hex(pd->sd.detail);
    q8b_dbg_puts("\n");
    return -1;
}

int32_t sd_dev_read(int32_t sector) {
    _pending_sector = sector;
    return 0;
}

int32_t sd_dev_read_done(void* buf) {
    if (_pending_sector < 0)
        return -1;
    int32_t sector = _pending_sector;
    _pending_sector = -1;
    return sd_dev_read_blocks(sector, buf, 1U);
}

static int32_t sd_dev_read_physical_blocks(int32_t sector, void* buf,
        uint32_t count) {
    if (sector < 0 || buf == NULL || count == 0)
        return -1;
#ifndef Q8B_QEMU_TEST
    if (_physical_sd_ready) {
        uint8_t* out = (uint8_t*)buf;
        uint32_t block = (uint32_t)sector;
        uint32_t remaining = count;

        if ((uint64_t)block + remaining > _physical_sd_sectors)
            return -1;
        while (remaining != 0) {
            uint32_t chunk = remaining;
            uint32_t read_chunk;
            int32_t read_rc;
            if (chunk > SDCC2_PIO_MAX_BLOCKS)
                chunk = SDCC2_PIO_MAX_BLOCKS;
            if (!_physical_sd_multi_ready)
                chunk = 1U;

            /* Some SDCC2/card combinations pass a short multi-block probe
             * but fail at the larger transfer size used while loading the
             * filesystem.  Keep the card at high speed and retry the same
             * range with progressively smaller commands before giving up.
             * A failed PIO command has already reset the data lines, so each
             * retry starts from the original block and buffer address. */
            read_chunk = chunk;
            read_rc = sdcc2_read_blocks_pio(sdcc2_sector_address(block), out,
                    read_chunk, NULL);
            if (read_rc != 0 && read_chunk > 4U) {
                read_chunk = 4U;
                if (read_chunk > remaining)
                    read_chunk = remaining;
                read_rc = sdcc2_read_blocks_pio(sdcc2_sector_address(block),
                        out, read_chunk, NULL);
            } else if (read_rc != 0 && read_chunk > 1U) {
                read_chunk = 1U;
                read_rc = sdcc2_read_blocks_pio(sdcc2_sector_address(block),
                        out, read_chunk, NULL);
            }
            if (read_rc != 0 && read_chunk > 1U) {
                read_chunk = 1U;
                read_rc = sdcc2_read_blocks_pio(sdcc2_sector_address(block),
                        out, 1U, NULL);
            }
            if (read_rc != 0 && _physical_sd_rca != 0) {
                /* A failed CMD18 can leave the card and host disagreeing
                 * about the transfer state.  Restore the conservative
                 * 25MHz/1-bit protocol before the final single-block retry.
                 */
                sdcc2_fast_to_slow(_physical_sd_rca);
                _physical_sd_multi_ready = 0;
                read_chunk = 1U;
                read_rc = sdcc2_read_blocks_pio(sdcc2_sector_address(block),
                        out, 1U, NULL);
            }
            if (read_rc != 0) {
                _physical_sd_multi_ready = 0;
                return -1;
            }
            if (read_chunk != chunk)
                _physical_sd_multi_ready = 0;
            block += read_chunk;
            remaining -= read_chunk;
            out += read_chunk * 512U;
        }
        return 0;
    }
#endif
    return -1;
}

int32_t sd_dev_read_blocks(int32_t sector, void* buf, uint32_t count) {
    if (sector < 0 || buf == NULL || count == 0)
        return -1;
    if (q8b_ramdisk_size != 0) {
        void* src = rd_ptr(sector, count);
        if (src == NULL)
            return -1;
        memcpy(buf, src, (size_t)count * 512);
        return 0;
    }
    return sd_dev_read_physical_blocks(sector, buf, count);
}

int32_t sd_dev_write(int32_t sector, const void* buf) {
    if (q8b_ramdisk_size == 0 || sector < 0 || buf == NULL)
        return -1;
    _pending_sector = sector;
    memcpy(_write_buf, buf, sizeof(_write_buf));
    return 0;
}

int32_t sd_dev_write_done(void) {
    if (_pending_sector < 0)
        return -1;
    void* dst = rd_ptr(_pending_sector, 1);
    _pending_sector = -1;
    if (dst == NULL)
        return -1;
    memcpy(dst, _write_buf, sizeof(_write_buf));
    return 0;
}

void sd_dev_handle(void) {
}
