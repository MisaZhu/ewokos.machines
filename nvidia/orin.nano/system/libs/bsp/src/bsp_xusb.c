#include <stdint.h>
#include <bsp/bsp_xusb.h>
#include <bsp/bsp_bpmp.h>
#include <ewoksys/syscall.h>
#include <ewoksys/klog.h>
#include <orin_net.h>

/*
 * Tegra234 XUSB host bring-up (usb@3610000), USB2-only host path.  Sequence
 * from upstream xhci-tegra.c probe + xusb-tegra186.c padctl.  BPMP supplies
 * powergate/clock/reset (genpd power-on includes reset).  T234 Falcon uses
 * IFR built-in firmware (no xusb.bin), so we only wait for USBSTS.CNR clear.
 * Hardware-unverified; bounded polls.  The xHCI ring driver takes over from
 * the returned capability base.
 */

#define PADCTL_VA   0x68000000ULL
#define PADCTL_AO_VA 0x68020000ULL
#define FPCI_VA     0x68030000ULL
#define HCD_VA      0x68040000ULL
#define BAR2_VA     0x68080000ULL

/* padctl registers + bit fields (upstream xusb-tegra186.c, T234 soc data) */
#define PADCTL_USB2_PAD_MUX     0x004
#define PADCTL_USB2_PORT_CAP    0x008   /* 4 bits per port */
#define PADCTL_OTG_PAD_CTL0     0x088   /* +0x40 per port */
#define PADCTL_OTG_PAD_CTL1     0x08c
#define PADCTL_BIAS_PAD_CTL0    0x284
#define PADCTL_BIAS_PAD_CTL1    0x288
#define PADCTL_BIAS_PAD_CTL2    0x28c

#define OTG_CTL0_HS_CURR_LEVEL(x)  ((x) & 0x3f)
#define OTG_CTL0_TERM_SEL          (1u << 25)
#define OTG_CTL0_PD                (1u << 26)
#define OTG_CTL0_PD_ZI             (1u << 29)
#define OTG_CTL1_TERM_RANGE_ADJ(x) (((x) & 0xf) << 3)
#define OTG_CTL1_RPD_CTRL(x)       (((x) & 0x1f) << 26)
#define OTG_CTL1_PD_DR             (1u << 2)
#define BIAS_CTL0_PD               (1u << 11)
#define BIAS_CTL0_HS_SQUELCH(x)    ((x) & 0x7)
#define BIAS_CTL0_HS_DISCON(x)     (((x) & 0x7) << 3)
#define BIAS_CTL1_TRK_START_TIMER(x)   (((x) & 0x7f) << 12)
#define BIAS_CTL1_TRK_DONE_RESET_TIMER(x) (((x) & 0x7f) << 19)
#define BIAS_CTL1_PD_TRK           (1u << 26)
#define BIAS_CTL1_TRK_COMPLETED    (1u << 31)
#define BIAS_CTL2_TRK_UPDATE_ON_IDLE (1u << 31)

/* FPCI configuration registers */
#define FPCI_CFG_1              0x004
#define FPCI_CFG_4              0x010   /* BAR0 mask/addr */
#define FPCI_CFG_7              0x01c   /* BAR2 */

/* xHCI capability/operational registers */
#define XHCI_CAPLENGTH          0x00
#define XHCI_USBSTS             0x04
#define XHCI_USBSTS_CNR         (1u << 11)

static volatile uint8_t* _padctl;
static volatile uint8_t* _fpci;
static volatile uint8_t* _hcd;
static uint64_t _cap_va;

static inline uint32_t rd(volatile uint8_t* b, uint32_t off) {
    return *(volatile uint32_t*)(b + off);
}
static inline void wr(volatile uint8_t* b, uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(b + off) = v;
}
static int map(uint64_t va, uint64_t phy, uint32_t size) {
    return syscall3(SYS_MEM_MAP, (ewokos_addr_t)va, (ewokos_addr_t)phy,
            (ewokos_addr_t)size) == (ewokos_addr_t)va ? 0 : -1;
}

static void xusb_clocks(void) {
    /* PLL_E / UTMIP PLL first, then core/host/falcon/fs sources. */
    static const uint32_t clks[] = {
        ORIN_XUSB_CLK_PLLE, ORIN_XUSB_CLK_UTMIP_PLL, ORIN_XUSB_CLK_M,
        ORIN_XUSB_CLK_USB2_TRK, ORIN_XUSB_CLK_FS, ORIN_XUSB_CLK_HOST,
        ORIN_XUSB_CLK_FALCON, ORIN_XUSB_CLK_CORE_SS, ORIN_XUSB_CLK_SS,
    };
    for (uint32_t i = 0; i < sizeof(clks) / sizeof(clks[0]); i++) {
        int r = bsp_bpmp_clk_enable(clks[i]);
        if (r != 0)
            klog("orin xusb: clk %u enable failed %d\n", clks[i], r);
    }
}

static void padctl_usb2_host(void) {
    /* USB2-1 = devkit onboard hub (USB-A ports).  Sequence mirrors upstream
     * tegra186_utmi_phy_init + bias/pad power-on with T234 soc flags
     * (poll_trk_completed, trk_hw_mode=false, trk_update_on_idle,
     * has_per_pad_term). */
    const int port = 1;

    /* eFUSE calibration values are the upstream source, but the T234 fuse
     * block (0x3810000) rejects CCPLEX reads with a RAS slave error
     * (hardware-verified).  Use mid-range defaults; connect detection does
     * not depend on the exact trim. */
    uint32_t curr_level = 0x10;
    uint32_t squelch = 0x4;
    uint32_t term_adj = 0x0;
    uint32_t rpd_ctrl = 0x10;
    klog("orin xusb: phy calib defaults curr=%u squelch=%u term=%u rpd=%u\n",
            curr_level, squelch, term_adj, rpd_ctrl);

    /* Bias pad power-on + impedance tracking (tegra186_utmi_bias_pad_power_on). */
    uint32_t v = rd(_padctl, PADCTL_BIAS_PAD_CTL1);
    v &= ~BIAS_CTL1_TRK_START_TIMER(0x7f);
    v |= BIAS_CTL1_TRK_START_TIMER(0x1e);
    v &= ~BIAS_CTL1_TRK_DONE_RESET_TIMER(0x7f);
    v |= BIAS_CTL1_TRK_DONE_RESET_TIMER(0xa);
    wr(_padctl, PADCTL_BIAS_PAD_CTL1, v);

    v = rd(_padctl, PADCTL_BIAS_PAD_CTL0);
    v &= ~BIAS_CTL0_PD;
    v &= ~BIAS_CTL0_HS_SQUELCH(0x7);
    v |= BIAS_CTL0_HS_SQUELCH(squelch);
    v &= ~BIAS_CTL0_HS_DISCON(0x7);
    v |= BIAS_CTL0_HS_DISCON(0x7);
    wr(_padctl, PADCTL_BIAS_PAD_CTL0, v);

    /* udelay(1) */
    for (volatile int i = 0; i < 1000; i++);

    v = rd(_padctl, PADCTL_BIAS_PAD_CTL1);
    v &= ~BIAS_CTL1_PD_TRK;
    wr(_padctl, PADCTL_BIAS_PAD_CTL1, v);

    int trk_ok = 0;
    for (uint32_t i = 0; i < 200; i++) {
        if (rd(_padctl, PADCTL_BIAS_PAD_CTL1) & BIAS_CTL1_TRK_COMPLETED) {
            trk_ok = 1;
            break;
        }
        for (volatile int d = 0; d < 1000; d++);   /* ~1us */
    }
    v = rd(_padctl, PADCTL_BIAS_PAD_CTL1);
    v |= BIAS_CTL1_TRK_COMPLETED;
    wr(_padctl, PADCTL_BIAS_PAD_CTL1, v);

    v = rd(_padctl, PADCTL_BIAS_PAD_CTL2);
    v &= ~BIAS_CTL2_TRK_UPDATE_ON_IDLE;   /* T234 trk_update_on_idle */
    wr(_padctl, PADCTL_BIAS_PAD_CTL2, v); /* trk_hw_mode=false: leave bit0 */
    klog("orin xusb: bias trk %s ctl0=%08x ctl1=%08x ctl2=%08x\n",
            trk_ok ? "ok" : "TIMEOUT",
            rd(_padctl, PADCTL_BIAS_PAD_CTL0), rd(_padctl, PADCTL_BIAS_PAD_CTL1),
            rd(_padctl, PADCTL_BIAS_PAD_CTL2));

    /* Route the port1 pads to XUSB (mux value 1) and cap = host. */
    v = rd(_padctl, PADCTL_USB2_PAD_MUX);
    v &= ~(3u << (port * 2));
    v |= (1u << (port * 2));
    wr(_padctl, PADCTL_USB2_PAD_MUX, v);

    v = rd(_padctl, PADCTL_USB2_PORT_CAP);
    v &= ~(3u << (port * 4));
    v |= (1u << (port * 4));
    wr(_padctl, PADCTL_USB2_PORT_CAP, v);

    /* OTG pad config: termination select + fuse-calibrated drive levels. */
    v = rd(_padctl, PADCTL_OTG_PAD_CTL0 + port * 0x40);
    v &= ~OTG_CTL0_PD_ZI;
    v |= OTG_CTL0_TERM_SEL;
    v &= ~OTG_CTL0_HS_CURR_LEVEL(0x3f);
    v |= OTG_CTL0_HS_CURR_LEVEL(curr_level);
    wr(_padctl, PADCTL_OTG_PAD_CTL0 + port * 0x40, v);

    v = rd(_padctl, PADCTL_OTG_PAD_CTL1 + port * 0x40);
    v &= ~OTG_CTL1_TERM_RANGE_ADJ(0xf);
    v |= OTG_CTL1_TERM_RANGE_ADJ(term_adj);
    v &= ~OTG_CTL1_RPD_CTRL(0x1f);
    v |= OTG_CTL1_RPD_CTRL(rpd_ctrl);
    wr(_padctl, PADCTL_OTG_PAD_CTL1 + port * 0x40, v);

    /* Pad power-on: clear PD (CTL0 bit26) and PD_DR (CTL1 bit2). */
    v = rd(_padctl, PADCTL_OTG_PAD_CTL0 + port * 0x40);
    v &= ~OTG_CTL0_PD;
    wr(_padctl, PADCTL_OTG_PAD_CTL0 + port * 0x40, v);
    v = rd(_padctl, PADCTL_OTG_PAD_CTL1 + port * 0x40);
    v &= ~OTG_CTL1_PD_DR;
    wr(_padctl, PADCTL_OTG_PAD_CTL1 + port * 0x40, v);

    klog("orin xusb: padctl mux=%08x cap=%08x ctl0=%08x ctl1=%08x\n",
            rd(_padctl, PADCTL_USB2_PAD_MUX), rd(_padctl, PADCTL_USB2_PORT_CAP),
            rd(_padctl, PADCTL_OTG_PAD_CTL0 + port * 0x40),
            rd(_padctl, PADCTL_OTG_PAD_CTL1 + port * 0x40));
}

uint64_t bsp_xusb_init(void) {
    if (_cap_va)
        return _cap_va;
    if (bsp_bpmp_init() != 0) {
        klog("orin xusb: bpmp init failed\n");
        return 0;
    }

    /* Powergate on (genpd power-on includes the XUSB resets). */
    int r_a = bsp_bpmp_pg_on(ORIN_XUSB_PG_A);
    int r_c = bsp_bpmp_pg_on(ORIN_XUSB_PG_C);
    klog("orin xusb: pg A=%d C=%d\n", r_a, r_c);
    if (r_a != 0 || r_c != 0)
        return 0;
    xusb_clocks();
    klog("orin xusb: padctl reset deassert=%d\n",
            bsp_bpmp_reset_deassert(ORIN_XUSB_RESET_PADCTL));

    if (map(PADCTL_VA, ORIN_XUSB_PADCTL_PHY, ORIN_XUSB_PADCTL_SIZE) != 0 ||
            map(PADCTL_AO_VA, ORIN_XUSB_PADCTL_AO_PHY, 0x10000) != 0 ||
            map(FPCI_VA, ORIN_XUSB_FPCI_PHY, 0x10000) != 0 ||
            map(HCD_VA, ORIN_XUSB_HCD_PHY, ORIN_XUSB_HCD_SIZE) != 0 ||
            map(BAR2_VA, ORIN_XUSB_BAR2_PHY, 0x10000) != 0) {
        klog("orin xusb: register map failed\n");
        return 0;
    }
    _padctl = (volatile uint8_t*)(uintptr_t)PADCTL_VA;
    _fpci = (volatile uint8_t*)(uintptr_t)FPCI_VA;
    _hcd = (volatile uint8_t*)(uintptr_t)HCD_VA;

    padctl_usb2_host();

    /* FPCI: expose BAR0 (xHCI regs) and BAR2 (mailbox), enable bus master. */
    wr(_fpci, FPCI_CFG_4, (uint32_t)(ORIN_XUSB_HCD_PHY >> 15));
    wr(_fpci, FPCI_CFG_7, (uint32_t)ORIN_XUSB_BAR2_PHY);
    wr(_fpci, FPCI_CFG_1, rd(_fpci, FPCI_CFG_1) | 0x07u);
    klog("orin xusb: fpci cfg1=%08x cfg4=%08x cfg7=%08x\n",
            rd(_fpci, FPCI_CFG_1), rd(_fpci, FPCI_CFG_4), rd(_fpci, FPCI_CFG_7));

    /* Wait for the xHCI core to leave reset (USBSTS.CNR clears). */
    uint32_t caplen = rd(_hcd, XHCI_CAPLENGTH) & 0xffu;
    volatile uint8_t* op = _hcd + caplen;
    klog("orin xusb: caplen=%u usbsts=%08x\n", caplen, rd(op, XHCI_USBSTS));
    int ready = 0;
    for (uint32_t i = 0; i < 2000000u; i++) {
        if ((rd(op, XHCI_USBSTS) & XHCI_USBSTS_CNR) == 0) {
            ready = 1;
            break;
        }
    }
    if (!ready) {
        klog("orin xusb: controller not ready (USBSTS=%08x)\n",
                rd(op, XHCI_USBSTS));
        return 0;
    }
    _cap_va = HCD_VA;
    klog("orin xusb: host controller ready, cap=%llx caplen=%u usbsts=%08x\n",
            (unsigned long long)_cap_va, caplen, rd(op, XHCI_USBSTS));
    return _cap_va;
}