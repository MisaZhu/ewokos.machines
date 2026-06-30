#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ewoksys/syscall.h>
#include <ewoksys/mmio.h>
#include <sysinfo.h>
#include <ewoksys/mmio.h>

#include "dwmmc.h"

extern int mmc_read_blocks(struct dwmci_host *host, void *dst, uint32_t sector);
extern int mmc_read_sectors(struct dwmci_host *host, void *dst, uint32_t sector,
		uint32_t count);

static struct dwmci_host dwc_host = {
    .fifoth_val = 0,
    .fifo_mode = true,
};

#define RK3506_SD_RETRY_COUNT 5U
#define RK3506_SD_MULTI_RETRY_COUNT 2U
#define RK3506_SD_MULTI_CHUNK_SECTORS 32U

static void rk3506_sd_setup_host(void) {
	uint32_t fifo_size;

	fifo_size = dwmci_readl(&dwc_host, DWMCI_FIFOTH);
	fifo_size = ((fifo_size & RX_WMARK_MASK) >> RX_WMARK_SHIFT) + 1;
	dwc_host.fifoth_val = MSIZE(0x2) | RX_WMARK(fifo_size / 2 - 1) |
		TX_WMARK(fifo_size / 2);
}

/**
 * initialize EMMC to read SDHC card
 */
int32_t rk3506_sd_init(void) {
	_mmio_base = mmio_map();
	if(_mmio_base == 0)
		return -1;
	dwc_host.ioaddr = (void*)(_mmio_base + 0x480000);
	rk3506_sd_setup_host();
	return 0;
}


int32_t rk3506_sd_read_sector(int32_t sector, void* buf) {
	int ret;
	uint32_t retry = RK3506_SD_RETRY_COUNT;

	if(buf == NULL)
		return -1;
	do{
		ret = mmc_read_blocks(&dwc_host, buf, sector);
		if(ret == 0)
			break;
	}while(retry--);

	return ret;
}

int32_t rk3506_sd_read_blocks(int32_t sector, void* buf, uint32_t count) {
	uint8_t *dst = (uint8_t*)buf;

	if(buf == NULL)
		return -1;
	while(count > 0) {
		uint32_t chunk = (count > RK3506_SD_MULTI_CHUNK_SECTORS) ?
				RK3506_SD_MULTI_CHUNK_SECTORS : count;
		int ret = -1;
		uint32_t retry = RK3506_SD_MULTI_RETRY_COUNT;

		if(chunk > 1) {
			do {
				ret = mmc_read_sectors(&dwc_host, dst, sector, chunk);
				if(ret == 0)
					break;
			} while(retry--);

			if(ret == 0) {
				dst += chunk * 512U;
				sector += chunk;
				count -= chunk;
				continue;
			}
		}

		ret = rk3506_sd_read_sector(sector, dst);
		if(ret != 0)
			return ret;
		dst += 512U;
		sector++;
		count--;
	}

	return 0;
}

int32_t rk3506_sd_write_sector(int32_t sector, const void* buf) {
	(void)sector;
	(void)buf;
	return 0;
}
