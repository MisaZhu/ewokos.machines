#ifndef Q8B_BSP_PCIE_H
#define Q8B_BSP_PCIE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The PCIe host is owned by /drivers/q8b/pcied.  Clients only query the
 * immutable enumeration snapshot through this character device; they never
 * map ECAM or modify PCI configuration space themselves. */
#define Q8B_PCIE_SERVICE_NODE "/dev/pcie0"
enum {
	Q8B_PCIE_CMD_GET_COUNT  = 0x50430001,
	Q8B_PCIE_CMD_GET_DEVICE = 0x50430002,
	Q8B_PCIE_CMD_PREPARE_WLAN_DMA = 0x50430003,
};

/* Controllers named by the SC8280XP Q8B board DTS.  PCIe3A (QPS615 Ethernet
 * switch) and PCIe4 (E-key WLAN) are brought up by the independent pcied
 * owner; PCIe2A/3B remain firmware-managed. */
enum q8b_pcie_controller {
	Q8B_PCIE2A = 0,
	Q8B_PCIE3A = 1,
	Q8B_PCIE3B = 2,
	Q8B_PCIE4  = 3,
};

enum {
	Q8B_PCIE_POWER_ON      = 1u << 0,
	Q8B_PCIE_ENABLE_MEMORY = 1u << 1,
	Q8B_PCIE_ENABLE_MASTER = 1u << 2,
	Q8B_PCIE_DISABLE_ASPM  = 1u << 3,
	/* Cold, host-owned controllers only; never resize firmware/live BARs. */
	Q8B_PCIE_ASSIGN_RESOURCES = 1u << 4,
};

typedef struct {
	uint8_t  controller;       /* enum q8b_pcie_controller */
	uint8_t  bus;
	uint8_t  device;
	uint8_t  function;
	uint8_t  header_type;
	uint16_t vendor_id;
	uint16_t device_id;
	uint16_t subsystem_vendor;
	uint16_t subsystem_device;
	uint32_t class_code;       /* base class << 16 | subclass << 8 | prog-if */
	uint8_t  revision;
	uint16_t command;
	uint64_t bar[6];            /* assigned PCI bus addresses */
	uint64_t bar_phys[6];      /* CPU physical address through the RC window */
	uint32_t bar_size[6];
	uint8_t  pcie_cap;          /* offset of PCIe capability, or zero */
	uint8_t  link_speed;
	uint8_t  link_width;
	uint8_t  vpd_cap;           /* offset of VPD capability, or zero */
	uint8_t  vpd_mac_valid;
	uint8_t  vpd_mac[6];        /* decoded module address, when present */
	uint8_t  serial_valid;      /* PCIe Device Serial Number capability */
	uint8_t  _serial_pad[7];
	uint64_t serial_number;     /* immutable endpoint serial, when present */
} q8b_pcie_device_t;

/* WCN6855 is sold on multiple E-key cards.  The PCI function identity stays
 * 17cb:1103 while the card vendor changes the subsystem ID.  Keep the list
 * explicit because it is also the boundary for DMA/MSI restoration. */
#define Q8B_PCIE_WLAN_VENDOR_ID       0x17cbu
#define Q8B_PCIE_WLAN_DEVICE_ID       0x1103u
#define Q8B_PCIE_WLAN_SUBSYS_QCOM_VID 0x17cbu
#define Q8B_PCIE_WLAN_SUBSYS_QCOM_DID 0x0108u
#define Q8B_PCIE_WLAN_SUBSYS_ALT_VID  0x105bu
#define Q8B_PCIE_WLAN_SUBSYS_ALT_DID  0xe130u

static inline bool q8b_pcie_wlan_subsystem_supported(uint16_t vendor,
		uint16_t device) {
	return (vendor == Q8B_PCIE_WLAN_SUBSYS_QCOM_VID &&
			device == Q8B_PCIE_WLAN_SUBSYS_QCOM_DID) ||
		(vendor == Q8B_PCIE_WLAN_SUBSYS_ALT_VID &&
			device == Q8B_PCIE_WLAN_SUBSYS_ALT_DID);
}

static inline bool q8b_pcie_is_wlan(const q8b_pcie_device_t *device) {
	return device && device->controller == Q8B_PCIE4 &&
		device->vendor_id == Q8B_PCIE_WLAN_VENDOR_ID &&
		device->device_id == Q8B_PCIE_WLAN_DEVICE_ID &&
		q8b_pcie_wlan_subsystem_supported(device->subsystem_vendor,
				device->subsystem_device);
}

/* Bring up one controller.  max_generation is 1..3; PCIe3A can use Gen3,
 * while the Q8B PCIe4 WLAN link is capped at Gen2 by the board wiring.  A
 * live link is left intact. */
int q8b_pcie_power_on(enum q8b_pcie_controller controller,
		unsigned max_generation);

/* Enumerate functions below a controller.  Returns 0..max, or a negative
 * value when the ECAM window cannot be mapped or the link is down. */
int q8b_pcie_probe(enum q8b_pcie_controller controller,
		q8b_pcie_device_t *out, size_t max, unsigned flags);

/* PCIe4 convenience entry point for callers that need power-on and probing
 * as one operation.  The pcied service normally calls the two operations
 * separately so the ordering is explicit at its ownership boundary. */
int q8b_pcie4_start_and_probe(q8b_pcie_device_t *out, size_t max,
		unsigned max_generation, unsigned flags);

/* Map an assigned BAR into a caller-selected virtual address. */
int q8b_pcie_map_bar(const q8b_pcie_device_t *device, unsigned bar,
		uintptr_t virtual_address, uint32_t size);

/* Read the endpoint's standard PCI VPD through the bus owner.  VPD is an
 * optional capability; a negative return means that the capability is absent
 * or the endpoint did not complete the bounded read.  Clients must never
 * access ECAM directly because a VPD transaction temporarily writes the VPD
 * address register and must be serialized with all other config accesses. */
int q8b_pcie_read_vpd(const q8b_pcie_device_t *device, unsigned offset,
		void *out, size_t bytes);

/* Read-only client helpers for the independent pcied service. */
int q8b_pcie_service_get_count(void);
int q8b_pcie_service_get_device(unsigned index, q8b_pcie_device_t *out);

/* WCN6855 SOC reset clears PCI command/MSI. The bus owner restores only
 * this function's DMA enable and a service-owned polling MSI sink. */
int q8b_pcie_service_prepare_wlan_dma(void);
int q8b_pcie_prepare_wlan_dma(const q8b_pcie_device_t *device, uint64_t sink);

#endif
