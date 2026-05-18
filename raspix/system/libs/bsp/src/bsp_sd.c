#include <bsp/bsp_sd.h>
#include <stdlib.h>
#include <stdint.h>
#include <sysinfo.h>
#include <string.h>
#include <ewoksys/syscall.h>
#include <sd/sd.h>
#include <ewoksys/mmio.h>
#include <arch/bcm283x/sd.h>


void* cache_entry[4096] = {0};

#define SD_CACHE_PAGE_SECTORS 8U
#define SD_CACHE_PAGE_SIZE (SD_CACHE_PAGE_SECTORS * 512U)
#define SD_CACHE_PREFETCH_PAGES 16U
#define SD_CACHE_PREFETCH_SECTORS (SD_CACHE_PAGE_SECTORS * SD_CACHE_PREFETCH_PAGES)

static int32_t bsp_sd_read_cache(uint32_t sector, void *buf){
	uint32_t l1 = (sector >> 21) & 0x1FF;
	if(cache_entry[l1] == 0)
		cache_entry[l1] = calloc(4096, 1);

	void **l2_entry = cache_entry[l1];
	uint32_t l2 = (sector >> 12) & 0x1FF;
	if(l2_entry[l2] == 0)
		l2_entry[l2] = calloc(4096, 1);

	void **l3_entry = l2_entry[l2];
	uint32_t l3 = (sector >> 3) & 0x1FF;
	if(l3_entry[l3] == 0){
		uint32_t prefetch_start = sector & (~(SD_CACHE_PREFETCH_SECTORS - 1));
		uint32_t prefetch_l3 = (prefetch_start >> 3) & 0x1FF;
		uint8_t *prefetch_buf = malloc(SD_CACHE_PREFETCH_SECTORS * 512U);
		if(prefetch_buf == 0)
			return -1;

		if(mmc_read_blocks(prefetch_buf, prefetch_start, SD_CACHE_PREFETCH_SECTORS) != SD_CACHE_PREFETCH_SECTORS) {
			free(prefetch_buf);
			return -1;
		}

		for(uint32_t i = 0; i < SD_CACHE_PREFETCH_PAGES; i++) {
			uint32_t idx = prefetch_l3 + i;
			if(idx >= 0x200)
				break;
			if(l3_entry[idx] == 0) {
				l3_entry[idx] = malloc(SD_CACHE_PAGE_SIZE);
				if(l3_entry[idx] == 0)
					continue;
			}
			memcpy(l3_entry[idx], prefetch_buf + i * SD_CACHE_PAGE_SIZE, SD_CACHE_PAGE_SIZE);
		}
		free(prefetch_buf);
		if(l3_entry[l3] == 0)
			return -1;
	}
	uint8_t *page = l3_entry[l3];
	memcpy(buf, page + (sector & (SD_CACHE_PAGE_SECTORS - 1)) * 512, 512);
	return 0;
}

static int32_t bsp_sd_write_cache(uint32_t sector, void *buf){
	/*uint32_t l1 = (sector >> 21) & 0x1FF;
	if(cache_entry[l1] == 0)
		cache_entry[l1] = calloc(4096, 1);

	uint32_t *l2_entry = cache_entry[l1];
	uint32_t l2 = (sector >> 12) & 0x1FF;
	if(l2_entry[l2] == 0)
		l2_entry[l2] = calloc(4096, 1);

	uint32_t *l3_entry = l2_entry[l2];
	uint32_t l3 = (sector >> 3) & 0x1FF;
	if(l3_entry[l3] == 0){
		l3_entry[l3] = malloc(4096);
		mmc_read_blocks(l3_entry[l3], sector&(~0x7),1);
	}
	uint8_t *page = l3_entry[l3];
	memcpy(page + (sector & 0x7) * 512, buf, 512);
	// Write back the modified page to SD card
	mmc_write_blocks(sector&(~0x7), 1, page);
	*/

	return mmc_write_blocks(sector, 1, buf) == 1 ? 0 : -1;
}

int32_t bsp_sd_init_by_info(void) {
   sys_info_t sysinfo;
    _mmio_base = mmio_map();
   syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&sysinfo);
   if(strstr(sysinfo.machine, "pi4") || strstr(sysinfo.machine, "cm4"))
        return mmc_init(1);
   else
        return mmc_init(0);
}

int bsp_sd_init(void) {
    return sd_init(bsp_sd_init_by_info, bsp_sd_read_cache, bsp_sd_write_cache);
}
