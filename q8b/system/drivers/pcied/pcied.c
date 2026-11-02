/* Q8B PCIe host-bus owner.
 *
 * This process is the only user-space component allowed to power, train or
 * enumerate PCIe4.  Endpoint drivers receive an immutable snapshot through
 * /dev/pcie0 and map their assigned BARs using the physical address in that
 * snapshot.  Keeping ECAM and root-port writes here prevents one endpoint
 * driver from changing link parameters underneath another endpoint.
 */
#include <bsp/pcie.h>
#include <ewoksys/klog.h>
#include <ewoksys/dma.h>
#include <ewoksys/proto.h>
#include <ewoksys/sys.h>
#include <ewoksys/vdevice.h>
#include <string.h>

#define PCIE_MAX_DEVICES 32

static q8b_pcie_device_t devices[PCIE_MAX_DEVICES];
static int device_count;
static ewokos_addr_t wlan_msi_sink;

static int pcie_dcntl(vdevice_t *dev, int from_pid, int cmd,
        proto_t *in, proto_t *ret, void *p) {
    int index;
    (void)dev;
    (void)from_pid;
    (void)p;

    if (cmd == Q8B_PCIE_CMD_PREPARE_WLAN_DMA) {
        for (int i = 0; i < device_count; i++) {
            q8b_pcie_device_t *d = &devices[i];
            if (!q8b_pcie_is_wlan(d))
                continue;
            /* Retain this page for the lifetime of the bus, including WLAN
             * process exit/retry. No endpoint can choose an arbitrary target. */
            if (!wlan_msi_sink)
                wlan_msi_sink = dma_alloc(0, 0x4000);
            if (!wlan_msi_sink)
                return -1;
            uint64_t pa = dma_phy_addr(0, wlan_msi_sink);
            return pa ? q8b_pcie_prepare_wlan_dma(d, pa) : -1;
        }
        return -1;
    }

    if (cmd == Q8B_PCIE_CMD_GET_COUNT) {
        PF->addi(ret, device_count);
        return 0;
    }
    if (cmd == Q8B_PCIE_CMD_GET_DEVICE) {
        if (!in || !ret)
            return -1;
        index = proto_read_int(in);
        if (index < 0 || index >= device_count)
            return -1;
        PF->add(ret, &devices[index], sizeof(devices[index]));
        return 0;
    }
    return -1;
}

int main(int argc, char **argv) {
    const char *node = argc > 1 ? argv[1] : Q8B_PCIE_SERVICE_NODE;
    vdevice_t dev;
    int ethernet_pf = 0;
    int wifi = 0;

    /* Several Q8B init profiles are usable after switchroot.  The boot
     * initramfs normally owns /dev/pcie0 already, so a profile that lists
     * pcied again must not reset a live WCN6855 link or publish a second
     * snapshot. */
    if (dev_get_pid(node) >= 0) {
        klog("q8b pcied: %s already owned, skipping duplicate power-on\n", node);
        return 0;
    }

    klog("q8b pcied: start, entering PCIe3A/PCIe4 power-on/probe\n");

    /* The WCN6855 is invisible until the E-key rail, radio enables, PHY rails,
     * root complex and link are brought up.  Do this before any ECAM walk so
     * the service cannot accidentally publish a cold PCIe4 endpoint.  The
     * power call is deliberately kept separate from q8b_pcie_probe(): this
     * makes the ownership boundary explicit and lets probe reject ECAM when
     * link training did not complete. */
    int pcie4_ready = q8b_pcie_power_on(Q8B_PCIE4, 2) == 0;
    if (pcie4_ready)
        klog("q8b pcied: PCIe4 power-on complete, probing ECAM after endpoint power\n");
    else
        klog("q8b pcied: PCIe4 power-on failed; PCIe4 ECAM will remain blocked\n");

    /* PCIe3A carries the QPS615 switch and the two Ethernet functions.  It is
     * powered and trained here before any ECAM walk; endpoint drivers only see
     * the immutable snapshot published below. */
    int pcie3a_ready = q8b_pcie_power_on(Q8B_PCIE3A, 3) == 0;
    if (pcie3a_ready)
        klog("q8b pcied: PCIe3A power-on complete, probing switch hierarchy\n");
    else
        klog("q8b pcied: PCIe3A power-on failed; Ethernet ECAM will remain blocked\n");

    /* PCIe4 is the boot-critical path: WLAN must be published before any
     * optional switch hierarchy is touched.  On the current Q8B firmware the
     * PCIe3A ECAM window can be advertised as powered while its user mapping
     * is still stale; walking it then raises an EL2 external abort and takes
     * this owner down before WLAN can attach.  Probe PCIe4 first and retain a
     * complete Wi-Fi snapshot even when the optional Ethernet switch is not
     * safe to walk. */
    device_count = 0;
    if (pcie4_ready && device_count < PCIE_MAX_DEVICES) {
        int n = q8b_pcie_probe(Q8B_PCIE4, devices + device_count,
                    PCIE_MAX_DEVICES - (size_t)device_count,
                    Q8B_PCIE_ENABLE_MEMORY | Q8B_PCIE_ENABLE_MASTER |
                    Q8B_PCIE_DISABLE_ASPM | Q8B_PCIE_ASSIGN_RESOURCES);
        if (n < 0)
            klog("q8b pcied: PCIe4 probe failed; retaining %d firmware device(s)\n",
                    device_count);
        else {
            device_count += n;
            klog("q8b pcied: PCIe4 probe complete, appended %d device(s)\n", n);
        }
    }

    /* PCIe2A/3B remain firmware-managed on Q8B.  PCIe3A is deliberately
     * deferred until its ECAM mapping has a dedicated validation path; a
     * broken optional switch must never prevent the independent WLAN service
     * from coming up.  The Ethernet endpoint can be re-enabled once that
     * controller-specific mapping check is available. */
    for (enum q8b_pcie_controller controller = Q8B_PCIE2A;
            controller < Q8B_PCIE3A && device_count < PCIE_MAX_DEVICES;
            controller++) {
        int n = q8b_pcie_probe(controller, devices + device_count,
                PCIE_MAX_DEVICES - (size_t)device_count,
                Q8B_PCIE_ENABLE_MEMORY | Q8B_PCIE_ENABLE_MASTER |
                Q8B_PCIE_DISABLE_ASPM);
        if (n > 0)
            device_count += n;
        else if (n < 0)
            klog("q8b pcied: controller %u ECAM unavailable\n", controller);
    }
    /* Keep the final immutable snapshot visible on the serial console while
     * validating the host service, including firmware-managed controllers
     * when PCIe4 link training is unavailable. */
    for (int i = 0; i < device_count; i++) {
        q8b_pcie_device_t *d = &devices[i];
        klog("q8b pcied: fn[%d] pcie%u %02x:%02x.%u %04x:%04x class=%06x sub=%04x:%04x\n",
                i, d->controller, d->bus, d->device, d->function,
                d->vendor_id, d->device_id, d->class_code,
                d->subsystem_vendor, d->subsystem_device);
        if (d->controller == Q8B_PCIE3A && d->vendor_id == 0x1179 &&
                d->device_id == 0x0220)
            ethernet_pf++;
        if (q8b_pcie_is_wlan(d))
            wifi++;
    }
    klog("q8b pcied: snapshot ethernet_pf=%d wifi=%d\n",
            ethernet_pf, wifi);
    klog("q8b pcied: host service ready, %d device(s)\n", device_count);

    memset(&dev, 0, sizeof(dev));
    strcpy(dev.desc, "Q8B PCIe host bus");
    dev.dev_cntl = pcie_dcntl;
    return device_run(&dev, node, FS_TYPE_CHAR, 0444, false);
}
