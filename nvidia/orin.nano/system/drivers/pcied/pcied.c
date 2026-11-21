/*
 * Orin Nano PCIe host-bus owner (C1 / M.2 Key-E).
 *
 * Cold-owns the C1 root complex through BPMP (power/clock/reset) + P2U +
 * LTSSM, then enumerates the endpoint and publishes an immutable snapshot on
 * /dev/pcie0.  This exercises the full PCIe foundation for the WiFi slot and
 * reports whether a card is present:
 *   - original devkit: Key-E empty -> link training times out (expected).
 *   - Orin Nano Super: RTL8822CE (10ec:c822) populated -> link up + BAR.
 *
 * The RTL8822CE is a softmac device needing an 802.11 stack EwokOS does not
 * have, so this owner only enumerates it; there is no WiFi data path yet.
 * Ethernet (C8) is owned independently by the net driver (inherit-UEFI with
 * a cold-own fallback), so this service deliberately does not touch C8.
 */
#include <bsp/bsp_pcie.h>
#include <ewoksys/klog.h>
#include <ewoksys/proto.h>
#include <ewoksys/vdevice.h>
#include <orin_net.h>
#include <stdint.h>
#include <string.h>

static orin_pcie_dev_t _wifi;
static int _have_wifi;

static int pcie_dcntl(vdevice_t* dev, int from_pid, int cmd,
        proto_t* in, proto_t* ret, void* p) {
    (void)dev; (void)from_pid; (void)p;
    if (cmd == ORIN_PCIE_CMD_GET_COUNT) {
        PF->addi(ret, _have_wifi);
        return 0;
    }
    if (cmd == ORIN_PCIE_CMD_GET_DEVICE) {
        if (!in || !ret)
            return -1;
        int index = proto_read_int(in);
        if (index != 0 || !_have_wifi)
            return -1;
        PF->add(ret, &_wifi, sizeof(_wifi));
        return 0;
    }
    return -1;
}

int main(int argc, char** argv) {
    const char* node = argc > 1 ? argv[1] : ORIN_PCIE_SERVICE_NODE;

    if (dev_get_pid(node) >= 0) {
        slog("orin pcied: %s already owned, skipping\n", node);
        return 0;
    }

    slog("orin pcied: cold-owning PCIe C1 (M.2 Key-E)\n");
    if (bsp_pcie_cold_own(ORIN_PCIE_CTL_C1) != 0) {
        slog("orin pcied: C1 link down (Key-E empty or card absent); "
             "publishing empty snapshot\n");
    } else if (bsp_pcie_find_device(ORIN_PCIE_CTL_C1,
                ORIN_RTL8822CE_VENDOR, ORIN_RTL8822CE_DEVICE, &_wifi) == 0) {
        _have_wifi = 1;
        slog("orin pcied: RTL8822CE WiFi present @%02x:%02x.0 BAR0=%llx "
             "(softmac: needs 802.11 stack, no data path yet)\n",
             _wifi.bus, _wifi.dev, (unsigned long long)_wifi.bar0_phys);
    } else {
        slog("orin pcied: C1 link up but no RTL8822CE found\n");
    }

    vdevice_t dev;
    memset(&dev, 0, sizeof(dev));
    strcpy(dev.desc, "Orin PCIe C1 host bus");
    dev.dev_cntl = pcie_dcntl;
    return device_run(&dev, node, FS_TYPE_CHAR, 0444, false);
}