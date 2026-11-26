#ifndef QCOM8GEN3_BSP_PCIE_H
#define QCOM8GEN3_BSP_PCIE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The PCIe host is owned by /drivers/8gen3/pcied.  Clients only query the
 * immutable enumeration snapshot through this character device; they never
 * map ECAM or modify PCI configuration space themselves. */
#define QCOM8GEN3_PCIE_SERVICE_NODE "/dev/pcie0"
enum {
	QCOM8GEN3_PCIE_CMD_GET_COUNT  = 0x50430001,
	QCOM8GEN3_PCIE_CMD_GET_DEVICE = 0x50430002,
	QCOM8GEN3_PCIE_CMD_PREPARE_WLAN_DMA = 0x50430003,
};

/* Controllers named by the SC8280XP QCOM8GEN3 board DTS.  PCIe2A is the TW220
 * NVMe host; PCIe3A (QPS615 Ethernet switch) and PCIe4 (E-key WLAN) are
 * brought up by the independent pcied owner.  PCIe3B remains firmware-
 * managed until a second NVMe slot is present. */
enum qcom8gen3_pcie_controller {
	QCOM8GEN3_PCIE2A = 0,
	QCOM8GEN3_PCIE3A = 1,
	QCOM8GEN3_PCIE3B = 2,
	QCOM8GEN3_PCIE4  = 3,
};

enum {
	QCOM8GEN3_PCIE_POWER_ON      = 1u << 0,
	QCOM8GEN3_PCIE_ENABLE_MEMORY = 1u << 1,
	QCOM8GEN3_PCIE_ENABLE_MASTER = 1u << 2,
	QCOM8GEN3_PCIE_DISABLE_ASPM  = 1u << 3,
	/* Cold, host-owned controllers only; never resize firmware/live BARs. */
	QCOM8GEN3_PCIE_ASSIGN_RESOURCES = 1u << 4,
};

typedef struct {
	uint8_t  controller;       /* enum qcom8gen3_pcie_controller */
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
} qcom8gen3_pcie_device_t;

/* WCN6855 is sold on multiple E-key cards.  The PCI function identity stays
 * 17cb:1103 while the card vendor changes the subsystem ID.  Keep the list
 * explicit because it is also the boundary for DMA/MSI restoration. */
#define QCOM8GEN3_PCIE_WLAN_VENDOR_ID       0x17cbu
#define QCOM8GEN3_PCIE_WLAN_DEVICE_ID       0x1103u
#define QCOM8GEN3_PCIE_WLAN_SUBSYS_QCOM_VID 0x17cbu
#define QCOM8GEN3_PCIE_WLAN_SUBSYS_QCOM_DID 0x0108u
#define QCOM8GEN3_PCIE_WLAN_SUBSYS_ALT_VID  0x105bu
#define QCOM8GEN3_PCIE_WLAN_SUBSYS_ALT_DID  0xe130u

static inline bool qcom8gen3_pcie_wlan_subsystem_supported(uint16_t vendor,
		uint16_t device) {
	return (vendor == QCOM8GEN3_PCIE_WLAN_SUBSYS_QCOM_VID &&
			device == QCOM8GEN3_PCIE_WLAN_SUBSYS_QCOM_DID) ||
		(vendor == QCOM8GEN3_PCIE_WLAN_SUBSYS_ALT_VID &&
			device == QCOM8GEN3_PCIE_WLAN_SUBSYS_ALT_DID);
}

static inline bool qcom8gen3_pcie_is_wlan(const qcom8gen3_pcie_device_t *device) {
	return device && device->controller == QCOM8GEN3_PCIE4 &&
		device->vendor_id == QCOM8GEN3_PCIE_WLAN_VENDOR_ID &&
		device->device_id == QCOM8GEN3_PCIE_WLAN_DEVICE_ID &&
		qcom8gen3_pcie_wlan_subsystem_supported(device->subsystem_vendor,
				device->subsystem_device);
}

/* Bring up one controller.  max_generation is 1..3; PCIe3A can use Gen3,
 * while the QCOM8GEN3 PCIe4 WLAN link is capped at Gen2 by the board wiring.  A
 * live link is left intact. */
int qcom8gen3_pcie_power_on(enum qcom8gen3_pcie_controller controller,
		unsigned max_generation);

/* Enumerate functions below a controller.  Returns 0..max, or a negative
 * value when the ECAM window cannot be mapped or the link is down. */
int qcom8gen3_pcie_probe(enum qcom8gen3_pcie_controller controller,
		qcom8gen3_pcie_device_t *out, size_t max, unsigned flags);

/* PCIe4 convenience entry point for callers that need power-on and probing
 * as one operation.  The pcied service normally calls the two operations
 * separately so the ordering is explicit at its ownership boundary. */
int qcom8gen3_pcie4_start_and_probe(qcom8gen3_pcie_device_t *out, size_t max,
		unsigned max_generation, unsigned flags);

/* Map an assigned BAR into a caller-selected virtual address. */
int qcom8gen3_pcie_map_bar(const qcom8gen3_pcie_device_t *device, unsigned bar,
		uintptr_t virtual_address, uint32_t size);

/* Read the endpoint's standard PCI VPD through the bus owner.  VPD is an
 * optional capability; a negative return means that the capability is absent
 * or the endpoint did not complete the bounded read.  Clients must never
 * access ECAM directly because a VPD transaction temporarily writes the VPD
 * address register and must be serialized with all other config accesses. */
int qcom8gen3_pcie_read_vpd(const qcom8gen3_pcie_device_t *device, unsigned offset,
		void *out, size_t bytes);

/* Read-only client helpers for the independent pcied service. */
int qcom8gen3_pcie_service_get_count(void);
int qcom8gen3_pcie_service_get_device(unsigned index, qcom8gen3_pcie_device_t *out);

/* WCN6855 SOC reset clears PCI command/MSI. The bus owner restores only
 * this function's DMA enable and a service-owned polling MSI sink. */
int qcom8gen3_pcie_service_prepare_wlan_dma(void);
int qcom8gen3_pcie_prepare_wlan_dma(const qcom8gen3_pcie_device_t *device, uint64_t sink);

#endif
