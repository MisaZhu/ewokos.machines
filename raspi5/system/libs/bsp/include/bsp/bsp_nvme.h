#ifndef BSP_NVME_H
#define BSP_NVME_H

#include <stdint.h>

/*
 * Initialise the NVMe subsystem on Raspberry Pi 5.
 *
 * Maps PCIe MMIO windows, probes the bus for an NVMe controller,
 * initialises it, and registers a block-device backend with the
 * system's generic storage layer.
 *
 * Returns 0 on success, negative on error.
 */
int bsp_nvme_init(void);

/*
 * Read blocks from NVMe storage into buf.
 *   start_lba  — logical block address (in NVMe blocks, typically 512 B)
 *   buf        — destination buffer
 *   count      — number of blocks to read
 * Returns the number of blocks read, or negative on error.
 */
int32_t bsp_nvme_read(uint64_t start_lba, void *buf, uint32_t count);

/*
 * Write blocks to NVMe storage from buf.
 *   start_lba  — logical block address
 *   buf        — source buffer
 *   count      — number of blocks to write
 * Returns the number of blocks written, or negative on error.
 */
int32_t bsp_nvme_write(uint64_t start_lba, const void *buf, uint32_t count);

/*
 * Return the total number of addressable LBA blocks.
 * Only valid after bsp_nvme_init() succeeds.
 */
uint64_t bsp_nvme_get_block_count(void);

/*
 * Return the block size in bytes (power of two, typically 512).
 * Only valid after bsp_nvme_init() succeeds.
 */
uint32_t bsp_nvme_get_block_size(void);

#endif
