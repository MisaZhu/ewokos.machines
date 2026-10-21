#ifndef __Q8B_NVME_H
#define __Q8B_NVME_H

/*
 * User-space NVMe driver for the Q8B (SC8280XP) M.2 slots.
 *
 * The firmware performs PCIe controller bring-up and link training for
 * pcie2a (M-key Gen3 x4) and pcie3b (M-key Gen3 x2) before
 * ExitBootServices, so the OS only has to:
 *   - walk the root complex ECAM for the NVMe function,
 *   - map BAR0 through the matching outbound aperture,
 *   - drive the controller with polled admin + I/O queues.
 *
 * DMA buffers come from the uncached sys_dma pool.  pcie2a/3b have no
 * iommus property in the board DTS (SMMU bypass), so the device consumes
 * CPU physical addresses directly (same model as the xHCI driver).
 */

#include <stdint.h>
#include <stdbool.h>
#include <ewoksys/ewokdef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NVME_SLOT_PCIE2A 0   /* M.2 M-key Gen3 x4 */
#define NVME_SLOT_PCIE3B 1   /* M.2 M-key Gen3 x2 */

#define NVME_MAX_XFER     (256U * 1024U)
#define NVME_IO_QSIZE     64U
#define NVME_ADMIN_QSIZE  16U

typedef struct {
	int      slot;            /* NVME_SLOT_* */
	int      bus, dev, fn;
	uint16_t vid, did;
	uint32_t class_code;      /* 24-bit base<<16 | sub<<8 | prog-if */

	uint64_t bar0_bus;        /* BAR0 value as seen on the PCI bus */
	uint64_t bar0_phys;       /* translated CPU physical address */
	uint32_t bar0_size;
	bool     bar0_64bit;

	uint16_t command;         /* PCI command register */
	uint16_t link_status;     /* PCIe link status (root port) */
} nvme_pci_dev_t;

typedef struct {
	bool     used;
	int      slot;
	int      bus, dev, fn;
	char     model[41];
	char     serial[21];
	char     fw_rev[9];
	uint64_t nsze;            /* namespace size, logical blocks */
	uint32_t block_size;      /* bytes per logical block */
	uint32_t lba_shift;
	uint8_t  mdts;            /* max data transfer size (power of 2 pages) */
	uint32_t max_xfer;        /* bytes per command, capped by mdts/MPS */
	uint32_t dstrd;
	uint32_t io_qsize;
	uint16_t nn;              /* namespace count */
} nvme_info_t;

/* Scan one root complex for NVMe functions.  Returns the number found
 * (0..max) or a negative errno. */
int q8b_pcie_scan(int slot, nvme_pci_dev_t* out, int max);

/* PCIe link status (negotiated speed/width) of one function. */
int q8b_pcie_link(int slot, int bus, int dev, int fn,
		uint32_t* speed, uint32_t* width);

/* Initialize the controller on the given device (admin queues + identify
 * + one I/O queue pair).  Returns 0 on success. */
int nvme_open(nvme_info_t* info, const nvme_pci_dev_t* dev);

/* Polled block I/O.  Blocks are logical blocks (info->block_size). */
int nvme_read(uint64_t lba, void* dst, uint32_t blocks);
int nvme_write(uint64_t lba, const void* src, uint32_t blocks);

/* Batched read: issue `depth` commands of `chunk` bytes starting at lba,
 * walk the whole range, optionally copying each chunk to dst.  When dst is
 * NULL the data stays in the DMA arena (raw link benchmark).  Returns the
 * number of bytes read, or a negative errno. */
int nvme_read_stream(uint64_t lba, void* dst, uint64_t bytes,
		uint32_t chunk, uint32_t depth, bool copy_out);

/* Close the controller and release DMA memory. */
void nvme_close(void);

extern nvme_info_t* nvme_current(void);

#ifdef __cplusplus
}
#endif

#endif
