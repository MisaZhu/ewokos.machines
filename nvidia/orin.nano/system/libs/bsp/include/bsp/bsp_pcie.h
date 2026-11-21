#ifndef ORIN_BSP_PCIE_H
#define ORIN_BSP_PCIE_H

#include <stdint.h>

/*
 * Tegra234 PCIe root-complex bring-up + enumeration (C1 WiFi / C8 Ethernet).
 *
 * Cold-own mode brings the controller up via BPMP (power/clock/reset) + P2U
 * PHY + appl/DBI config + LTSSM link training, for when UEFI power-gated it
 * after ExitBootServices.  bsp_pcie_find_device() then programs the DesignWare
 * iATU and scans config space through the 256KB config window (non-ECAM),
 * enables memory/bus-master in the NIC command register, and returns the
 * CPU-physical address of BAR0 so the MAC driver can map it.  See
 * include/orin_net.h for the address map.
 */

typedef struct {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  bus, dev, func;
    uint64_t bar0_phys;     /* CPU physical address of BAR0 (MMIO) */
    uint32_t bar0_size;
} orin_pcie_dev_t;

#define ORIN_PCIE_CTL_C1  0   /* pcie@14100000, M.2 Key-E (WiFi) */
#define ORIN_PCIE_CTL_C8  1   /* pcie@140a0000, Ethernet */
/* Returns 0 on link up. */
int bsp_pcie_cold_own(int ctl);
/* Returns 0 and fills *out when the device is found; -1 otherwise. */
int bsp_pcie_find_device(int ctl, uint16_t vendor, uint16_t device,
        orin_pcie_dev_t* out);

/* Host-bus owner service (/drivers/orin.nano/pcied).  Publishes an immutable
 * enumeration snapshot; endpoint drivers query it rather than walking ECAM
 * themselves (same ownership model as the qcom pcied). */
#define ORIN_PCIE_SERVICE_NODE   "/dev/pcie0"
#define ORIN_PCIE_CMD_GET_COUNT  0x50430001
#define ORIN_PCIE_CMD_GET_DEVICE 0x50430002

#endif