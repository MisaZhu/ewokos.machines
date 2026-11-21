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

/* padctl registers (xusb-tegra186.c) */
#define PADCTL_USB2_PAD_MUX     0x004
#define PADCTL_USB2_PORT_CAP    0x008
#define PADCTL_SS_PORT_CAP      0x00c
#define PADCTL_ELPG_PROGRAM     0x020
#define PADCTL_ELPG_PROGRAM_1   0x024
#define PADCTL_SS_PORT_CFG      0x02c
#define PADCTL_OTG_PAD0_CTL0    0x088   /* +0x40 per port */
#define PADCTL_OTG_PAD0_CTL1    0x08c
#define PADCTL_BIAS_PAD_CTL0    0x284
#define PADCTL_BIAS_PAD_CTL1    0x288
#define PADCTL_BIAS_PAD_CTL2    0x28c
#define PADCTL_USB2_BIAS_PAD_CTL2 0x28c

/* FPCI configuration registers */
#define FPCI_CFG_1              0x004
#define FPCI_CFG_4              0x010   /* BAR0 mask/addr */
#define FPCI_CFG_7              0x01c   /* BAR2 */

/* xHCI capability/operational registers */
#define XHCI_CAPLENGTH          0x00
#define XHCI_USBSTS             0x04
#define XHCI_USBSTS_CNR         (1u << 11)
#define XHCI_PORTSC             0x400

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
    bsp_bpmp_clk_enable(ORIN_XUSB_CLK_PLLE);
    bsp_bpmp_clk_enable(ORIN_XUSB_CLK_UTMIP_PLL);
    bsp_bpmp_clk_enable(ORIN_XUSB_CLK_M);
    bsp_bpmp_clk_enable(ORIN_XUSB_CLK_USB2_TRK);
    bsp_bpmp_clk_enable(ORIN_XUSB_CLK_FS);
    bsp_bpmp_clk_enable(ORIN_XUSB_CLK_HOST);
    bsp_bpmp_clk_enable(ORIN_XUSB_CLK_FALCON);
    bsp_bpmp_clk_enable(ORIN_XUSB_CLK_CORE_SS);
    bsp_bpmp_clk_enable(ORIN_XUSB_CLK_SS);
}

static void padctl_usb2_host(void) {
    /* Route USB2 pads to XUSB (mux 0) and set port capabilities to host.
     * Devkit: usb2-0 = USB-C (OTG/recovery), usb2-1 = onboard hub (host),
     * usb2-2 = M.2 Key-E.  Bring usb2-1 up as host for the USB-A ports. */
    uint32_t mux = rd(_padctl, PADCTL_USB2_PAD_MUX);
    mux &= ~(3u << 2);                 /* port1 mux = xusb (0) */
    wr(_padctl, PADCTL_USB2_PAD_MUX, mux);

    uint32_t cap = rd(_padctl, PADCTL_USB2_PORT_CAP);
    cap &= ~(3u << 2);
    cap |= (1u << 2);                  /* port1 = host */
    wr(_padctl, PADCTL_USB2_PORT_CAP, cap);

    /* OTG pad control for port1: clear power-down, enable terminations. */
    uint32_t ctl0 = rd(_padctl, PADCTL_OTG_PAD0_CTL0 + 0x40);
    ctl0 &= ~0x3u;                     /* clear pd */
    wr(_padctl, PADCTL_OTG_PAD0_CTL0 + 0x40, ctl0);
    uint32_t ctl1 = rd(_padctl, PADCTL_OTG_PAD0_CTL1 + 0x40);
    ctl1 |= (1u << 0);                 /* term select */
    wr(_padctl, PADCTL_OTG_PAD0_CTL1 + 0x40, ctl1);

    /* Bias pad: minimal calibration enable (full USB2_TRK trim is a
     * hardware-tuned follow-up). */
    wr(_padctl, PADCTL_BIAS_PAD_CTL0, rd(_padctl, PADCTL_BIAS_PAD_CTL0) | 1u);

    /* Enable ELPG for the USB2 ports. */
    wr(_padctl, PADCTL_ELPG_PROGRAM, rd(_padctl, PADCTL_ELPG_PROGRAM) | 0x1u);
}

uint64_t bsp_xusb_init(void) {
    if (_cap_va)
        return _cap_va;
    if (bsp_bpmp_init() != 0)
        return 0;

    /* Powergate on (genpd power-on includes the XUSB resets). */
    if (bsp_bpmp_pg_on(ORIN_XUSB_PG_A) != 0 ||
            bsp_bpmp_pg_on(ORIN_XUSB_PG_C) != 0) {
        slog("orin xusb: powergate on failed\n");
        return 0;
    }
    xusb_clocks();
    bsp_bpmp_reset_deassert(ORIN_XUSB_RESET_PADCTL);

    if (map(PADCTL_VA, ORIN_XUSB_PADCTL_PHY, ORIN_XUSB_PADCTL_SIZE) != 0 ||
            map(PADCTL_AO_VA, ORIN_XUSB_PADCTL_AO_PHY, 0x10000) != 0 ||
            map(FPCI_VA, ORIN_XUSB_FPCI_PHY, 0x10000) != 0 ||
            map(HCD_VA, ORIN_XUSB_HCD_PHY, ORIN_XUSB_HCD_SIZE) != 0 ||
            map(BAR2_VA, ORIN_XUSB_BAR2_PHY, 0x10000) != 0) {
        slog("orin xusb: register map failed\n");
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

    /* Wait for the xHCI core to leave reset (USBSTS.CNR clears). */
    uint32_t caplen = rd(_hcd, XHCI_CAPLENGTH) & 0xffu;
    volatile uint8_t* op = _hcd + caplen;
    int ready = 0;
    for (uint32_t i = 0; i < 2000000u; i++) {
        if ((rd(op, XHCI_USBSTS) & XHCI_USBSTS_CNR) == 0) {
            ready = 1;
            break;
        }
    }
    if (!ready) {
        slog("orin xusb: controller not ready (USBSTS=%08x)\n",
                rd(op, XHCI_USBSTS));
        return 0;
    }
    _cap_va = HCD_VA;
    slog("orin xusb: host controller ready, cap=%llx caplen=%u\n",
            (unsigned long long)_cap_va, caplen);
    return _cap_va;
}

uint32_t bsp_xusb_portsc(int port) {
    if (!_cap_va || port < 1)
        return 0xffffffffu;
    uint32_t caplen = rd(_hcd, XHCI_CAPLENGTH) & 0xffu;
    return rd(_hcd + caplen, XHCI_PORTSC + (uint32_t)(port - 1) * 0x10u);
}