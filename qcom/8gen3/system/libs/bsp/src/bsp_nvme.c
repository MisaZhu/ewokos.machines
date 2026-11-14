/*
 * BSP NVMe glue for the Q8B (SC8280XP) M.2 slots.
 *
 * Forwards the shared <bsp/bsp_nvme.h> block API onto the machine's
 * user-space NVMe driver (<bsp/nvme.h>).  The driver polls its queues,
 * so every call here is synchronous; transfers larger than the
 * controller's MDTS limit are chunked inside nvme_read/nvme_write.
 */
#include <bsp/bsp_nvme.h>
#include <bsp/nvme.h>
#include <pthread.h>
#include <string.h>

/* The underlying driver keeps ONE shared DMA arena and queue state for
 * all commands.  Callers (e.g. sdfsd's multi_task workers under the
 * shared fs lock) issue concurrent block requests, so every entry point
 * must serialize; the polled queues make the serialized throughput
 * sufficient. */
static pthread_mutex_t _nvme_lock = PTHREAD_MUTEX_INITIALIZER;

int bsp_nvme_init(void) {
	nvme_pci_dev_t dev;
	nvme_info_t info;
	int found;

	memset(&dev, 0, sizeof(dev));
	/* pcie2a first; pcie3b only when pcie2a is empty.  With no card in
	 * the 3b slot the firmware powers that root complex down and
	 * config-space reads raise an external abort (same order nvprobe
	 * uses on real hardware). */
	pthread_mutex_lock(&_nvme_lock);
	found = q8b_pcie_scan(NVME_SLOT_PCIE2A, &dev, 1);
	if (found <= 0)
		found = q8b_pcie_scan(NVME_SLOT_PCIE3B, &dev, 1);
	if (found <= 0) {
		pthread_mutex_unlock(&_nvme_lock);
		return -1;
	}

	if (nvme_open(&info, &dev) != 0) {
		pthread_mutex_unlock(&_nvme_lock);
		return -1;
	}
	pthread_mutex_unlock(&_nvme_lock);
	return 0;
}

int32_t bsp_nvme_read(uint64_t start_lba, void *buf, uint32_t count) {
	int rc;

	if (buf == NULL || count == 0)
		return -1;
	pthread_mutex_lock(&_nvme_lock);
	rc = nvme_read(start_lba, buf, count) == (int)count ?
		(int32_t)count : -1;
	pthread_mutex_unlock(&_nvme_lock);
	return rc;
}

int32_t bsp_nvme_write(uint64_t start_lba, const void *buf, uint32_t count) {
	int rc;

	if (buf == NULL || count == 0)
		return -1;
	pthread_mutex_lock(&_nvme_lock);
	rc = nvme_write(start_lba, buf, count) == (int)count ?
		(int32_t)count : -1;
	pthread_mutex_unlock(&_nvme_lock);
	return rc;
}

uint64_t bsp_nvme_get_block_count(void) {
	nvme_info_t* info = nvme_current();
	if (info == NULL || !info->used)
		return 0;
	return info->nsze;
}

uint32_t bsp_nvme_get_block_size(void) {
	nvme_info_t* info = nvme_current();
	if (info == NULL || !info->used)
		return 0;
	return info->block_size;
}

int32_t bsp_nvme_flush(void) {
	int rc;

	pthread_mutex_lock(&_nvme_lock);
	rc = nvme_flush() == 0 ? 0 : -1;
	pthread_mutex_unlock(&_nvme_lock);
	return rc;
}
