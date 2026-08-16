/*
 * Raspberry Pi 5 (BCM2712) WiFi platform glue.
 *
 * Same CYW43455 radio as Pi4; only the SDIO bus wiring differs. All facts
 * below come from the official device tree (arch/arm64/boot/dts/broadcom/
 * bcm2712.dtsi + bcm2712-rpi-5-b.dts) and the matching linux drivers
 * (pinctrl-bcm2712.c, gpio-brcmstb.c, sdhci-brcmstb.c).
 */
#include <ewoksys/mmio.h>
#include <ewoksys/syscall.h>
#include <sysinfo.h>
#include <unistd.h>
#include <arch/bcm2712/mmio.h>
#include <utils/log.h>

#include <types.h>
#include "platform.h"

/*
 * BCM2712 SoC pinctrl, D0 stepping (the retail Pi 5). One 4-bit fsel field
 * per pin, fsel 0 = GPIO and fsel N = funcs[N-1] of the per-pin table. On
 * D0 the first alternate function of gpio30-35 is "sd2" (dts group
 * sdio2_30_pins), so fsel 1 muxes the WiFi SDIO bus.
 *
 * Register/bit positions decoded from the linux driver's mux_bit tables
 * (GPIO_REGS(n, mr, mb, ...) -> reg (mr*32+mb)/8*4, shift (mr*32+mb)%32):
 *   gpio30: fsel reg 0x08 shift 24   gpio31: fsel reg 0x08 shift 28
 *   gpio32: fsel reg 0x0c shift  0   gpio33: fsel reg 0x0c shift  4
 *   gpio34: fsel reg 0x0c shift  8   gpio35: fsel reg 0x0c shift 12
 * Pull config is a 2-bit field per pin (0 none, 1 down, 2 up), decoded
 * from the same tables: pad_bit = pr*32 + pb*2, reg = (bit>>5)*4,
 * shift = bit&31. D0 GPIO_REGS(30,2,6,5,12)..(35,3,3,6,2) gives:
 *   gpio30: pull reg 0x14 shift 24   gpio31: pull reg 0x14 shift 26
 *   gpio32: pull reg 0x14 shift 28   gpio33: pull reg 0x18 shift  0
 *   gpio34: pull reg 0x18 shift  2   gpio35: pull reg 0x18 shift  4
 * (The old shifts 10/12/14 landed on gpio23/24/25, leaving CMD/DAT0
 * without host pulls; the bus then died the moment the dongle's
 * internal pulls were dropped via SBSDIO_FUNC1_SDIOPULLUP=0 - exactly
 * the runtime signature: CMD+DAT0 low in PRESENT_STATE, all-zero R5.)
 */
#define PI5_PINCTRL_BASE    (_mmio_base + PI5_PINCTRL_OFF)
#define PI5_FSEL_MASK       0xf
#define PI5_FSEL_SD2        1
#define PI5_PULL_NONE       0
#define PI5_PULL_UP         2
#define PI5_PULL_MASK       0x3

static void pinctrl_field_set(uint32_t reg, uint32_t shift, uint32_t mask,
        uint32_t val)
{
    uint32_t v = readl(PI5_PINCTRL_BASE + reg);
    v &= ~(mask << shift);
    v |= val << shift;
    writel(v, PI5_PINCTRL_BASE + reg);
}

static const struct { uint32_t reg; uint32_t shift; } _sd2_fsel[] = {
    { 0x08, 24 }, { 0x08, 28 },
    { 0x0c, 0 }, { 0x0c, 4 }, { 0x0c, 8 }, { 0x0c, 12 },
};

static const struct { uint32_t reg; uint32_t shift; } _sd2_pull[] = {
    { 0x14, 24 },                 /* clk: bias-disable */
    { 0x14, 26 }, { 0x14, 28 },   /* cmd, dat0: bias-pull-up */
    { 0x18, 0 }, { 0x18, 2 }, { 0x18, 4 },
};

void pi5_platform_pins(void)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(_sd2_fsel); i++) {
        pinctrl_field_set(_sd2_fsel[i].reg, _sd2_fsel[i].shift,
                PI5_FSEL_MASK, PI5_FSEL_SD2);
        pinctrl_field_set(_sd2_pull[i].reg, _sd2_pull[i].shift,
                PI5_PULL_MASK,
                (i == 0) ? PI5_PULL_NONE : PI5_PULL_UP);
    }
}

/*
 * "gio" brcmstb-gpio bank 0 @ 0x7d508500 (main window offset 0x01508500).
 * Per-bank registers at stride 0x20: ODEN 0x00, DATA 0x04, IODIR 0x08;
 * IODIR bit 1 = input. WL_REG_ON is gio GPIO28, active high, and feeds a
 * fixed 3.3V regulator with a 150ms startup delay (wl_on_reg in the dts).
 */
#define PI5_GIO_BASE        (_mmio_base + PI5_GIO_OFF)
#define GIO_ODEN            0x00
#define GIO_DATA            0x04
#define GIO_IODIR           0x08
#define WL_REG_ON_BIT       (1u << 28)

void pi5_platform_reg_on(bool on)
{
    /* push-pull output: open-drain off, direction out */
    writel(readl(PI5_GIO_BASE + GIO_ODEN) & ~WL_REG_ON_BIT,
            PI5_GIO_BASE + GIO_ODEN);
    writel(readl(PI5_GIO_BASE + GIO_IODIR) & ~WL_REG_ON_BIT,
            PI5_GIO_BASE + GIO_IODIR);

    if (on)
        writel(readl(PI5_GIO_BASE + GIO_DATA) | WL_REG_ON_BIT,
                PI5_GIO_BASE + GIO_DATA);
    else
        writel(readl(PI5_GIO_BASE + GIO_DATA) & ~WL_REG_ON_BIT,
                PI5_GIO_BASE + GIO_DATA);
}

/*
 * Map the SDHCI window (both hosts) into this process at the same virtual
 * offset the kernel uses, mirroring the proven i2c/uartd RP1 mapping
 * pattern. check_mem_map_arch() allows exactly this range.
 */
int pi5_platform_map(void)
{
    sys_info_t sysinfo;
    syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&sysinfo);
    _mmio_base = sysinfo.mmio.v_base;

    ewokos_addr_t want = (ewokos_addr_t)_mmio_base + PI5_EMMC_WIN_OFF;
    ewokos_addr_t mapped = syscall3(SYS_MEM_MAP, want,
            (ewokos_addr_t)PI5_EMMC_PHY_WIN,
            (ewokos_addr_t)PI5_EMMC_WIN_SIZE);
    if (mapped != want) {
        brcm_log("wlan: SDIO window map failed got=0x%lx want=0x%lx\n",
                (unsigned long)mapped, (unsigned long)want);
        return -1;
    }
    return 0;
}

/*
 * sdio2 vendor cfg registers, second reg range of the sdhci node (+0x400).
 * The WiFi chip is non-removable; force card presence exactly like
 * sdhci_brcmstb_cfginit_2712() does for MMC_CAP_NONREMOVABLE, otherwise
 * SDHCI_PRESENT_STATE never reports a card and the host stays gated.
 */
#define PI5_SDIO_CFG_BASE   (_mmio_base + PI5_EMMC_WIN_OFF + PI5_WLAN_SDIO_CFG_OFF)
#define SDIO_CFG_CTRL               0x00
#define  SDIO_CFG_CTRL_SDCD_N_TEST_EN  (1u << 31)
#define  SDIO_CFG_CTRL_SDCD_N_TEST_LEV (1u << 30)

void pi5_platform_force_card_present(void)
{
    uint32_t v = readl(PI5_SDIO_CFG_BASE + SDIO_CFG_CTRL);
    v &= ~SDIO_CFG_CTRL_SDCD_N_TEST_LEV;
    v |= SDIO_CFG_CTRL_SDCD_N_TEST_EN;
    writel(v, PI5_SDIO_CFG_BASE + SDIO_CFG_CTRL);
}
