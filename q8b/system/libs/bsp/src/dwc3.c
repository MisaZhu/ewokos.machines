/* Q8B USB-C DWC3 host handoff. Register definitions follow the Synopsys
 * DWC3 programming interface. GCC, PHY calibration, CC/VBUS negotiation and
 * SuperSpeed orientation are inherited from UEFI; do not reset the PHYs or
 * change the USB/DP lane mux during the host takeover. */
#include <stdint.h>
#include <unistd.h>
#include <ewoksys/mmio.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/klog.h>
#include <bsp/dwc3.h>

#define DWC3_GCTL             0xc110u
#define DWC3_GSNPSID          0xc120u
#define DWC3_GUSB2PHYCFG      0xc200u
#define DWC3_GUSB3PIPECTL     0xc2c0u
#define DWC3_DCTL             0xc704u
#define DWC3_DSTS             0xc70cu
#define GCTL_CORESOFTRESET    (1u << 11)
#define GCTL_PRTCAP_MASK      (3u << 12)
#define GCTL_PRTCAP_HOST      (1u << 12)
#define GUSB2PHYCFG_SUSPHY    (1u << 6)
#define GUSB3PIPECTL_SUSPHY   (1u << 17)
#define DCTL_RUN_STOP         (1u << 31)
#define DSTS_DEVCTRLHLT       (1u << 22)

int dwc3_host_prepare(int id, ewokos_addr_t base) {
    uint32_t snpsid = get32(base + DWC3_GSNPSID);
    uint32_t ip = snpsid >> 16;
    uint32_t gctl;

    if (ip != 0x5533u && ip != 0x3331u && ip != 0x3332u) {
        klog("dwc3%d: invalid core id=%08x\n", id, snpsid);
        return -1;
    }
    gctl = get32(base + DWC3_GCTL);
    klog("dwc3%d: id=%08x gctl=%08x role=%u\n", id, snpsid,
            gctl, (gctl & GCTL_PRTCAP_MASK) >> 12);

    if ((gctl & GCTL_PRTCAP_MASK) != GCTL_PRTCAP_HOST) {
        /* Stop firmware gadget DMA before switching the DRD core. */
        uint32_t dctl = get32(base + DWC3_DCTL);
        if (dctl & DCTL_RUN_STOP) {
            uint64_t deadline = kernel_tic_ms(0) + 500;
            put32(base + DWC3_DCTL, dctl & ~DCTL_RUN_STOP);
            while ((get32(base + DWC3_DSTS) & DSTS_DEVCTRLHLT) == 0) {
                if (kernel_tic_ms(0) > deadline) {
                    klog("dwc3%d: device halt timeout dsts=%08x\n", id,
                            get32(base + DWC3_DSTS));
                    return -1;
                }
                usleep(100);
            }
        }

        /* Synchronize the DRD core's internal clocks across the role change.
         * This resets the controller only; PHY calibration stays intact. */
        put32(base + DWC3_GCTL, gctl | GCTL_CORESOFTRESET);
        (void)get32(base + DWC3_GCTL);
        usleep(100000);
        gctl &= ~GCTL_CORESOFTRESET;
        put32(base + DWC3_GCTL, gctl);
    }

    /* SUSPHY must be clear while changing PRTCAPDIR. Wake the two PHY
     * interfaces even when UEFI left the core in host mode but suspended. */
    put32(base + DWC3_GUSB2PHYCFG,
            get32(base + DWC3_GUSB2PHYCFG) & ~GUSB2PHYCFG_SUSPHY);
    put32(base + DWC3_GUSB3PIPECTL,
            get32(base + DWC3_GUSB3PIPECTL) & ~GUSB3PIPECTL_SUSPHY);
    put32(base + DWC3_GCTL,
            (gctl & ~GCTL_PRTCAP_MASK) | GCTL_PRTCAP_HOST);
    gctl = get32(base + DWC3_GCTL);
    if ((gctl & GCTL_PRTCAP_MASK) != GCTL_PRTCAP_HOST) {
        klog("dwc3%d: host mode not latched gctl=%08x\n", id, gctl);
        return -1;
    }
    klog("dwc3%d: host ready usb2phy=%08x usb3pipe=%08x\n", id,
            get32(base + DWC3_GUSB2PHYCFG),
            get32(base + DWC3_GUSB3PIPECTL));
    return 0;
}
