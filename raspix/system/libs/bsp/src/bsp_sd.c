#include <bsp/bsp_sd.h>
#include <stdlib.h>
#include <stdint.h>
#include <sysinfo.h>
#include <string.h>
#include <ewoksys/syscall.h>
#include <sd/sd.h>
#include <ewoksys/mmio.h>
#include <arch/bcm283x/sd.h>

static void* cache_entry[4096] = {0};
static uint8_t* prefetch_buf = 0;
static uint32_t prefetch_buf_sectors = 0;
static uint32_t last_prefetch_end_page = UINT32_MAX;
static uint32_t next_prefetch_pages = 1;

#define SD_CACHE_PAGE_SECTORS 8U
#define SD_CACHE_PAGE_SIZE (SD_CACHE_PAGE_SECTORS * 512U)
#define SD_CACHE_MIN_PREFETCH_PAGES 1U
#define SD_CACHE_PREFETCH_PAGES 16U
#define SD_CACHE_PREFETCH_SECTORS (SD_CACHE_PAGE_SECTORS * SD_CACHE_PREFETCH_PAGES)

static void** bsp_sd_get_l3(uint32_t sector, int create) {
	uint32_t l1 = (sector >> 21) & 0x1FF;
	if(cache_entry[l1] == 0 && create)
		cache_entry[l1] = calloc(4096, 1);
	if(cache_entry[l1] == 0)
		return 0;

	void **l2_entry = cache_entry[l1];
	uint32_t l2 = (sector >> 12) & 0x1FF;
	if(l2_entry[l2] == 0 && create)
		l2_entry[l2] = calloc(4096, 1);
	if(l2_entry[l2] == 0)
		return 0;

	return l2_entry[l2];
}

static int bsp_sd_ensure_prefetch_buf(uint32_t sectors) {
	if(sectors <= prefetch_buf_sectors)
		return 0;
	uint8_t* new_buf = realloc(prefetch_buf, sectors * 512U);
	if(new_buf == 0)
		return -1;
	prefetch_buf = new_buf;
	prefetch_buf_sectors = sectors;
	return 0;
}

static int32_t bsp_sd_read_cache(uint32_t sector, void *buf){
	void **l3_entry = bsp_sd_get_l3(sector, 1);
	uint32_t l3 = (sector >> 3) & 0x1FF;
	if(l3_entry == 0)
		return -1;

	if(l3_entry[l3] == 0){
		uint32_t page = sector >> 3;
		uint32_t prefetch_pages = next_prefetch_pages;
		uint32_t prefetch_l3 = l3;

		if(page == last_prefetch_end_page)
			prefetch_pages <<= 1;
		else
			prefetch_pages = SD_CACHE_MIN_PREFETCH_PAGES;

		if(prefetch_pages > SD_CACHE_PREFETCH_PAGES)
			prefetch_pages = SD_CACHE_PREFETCH_PAGES;

		if((prefetch_l3 + prefetch_pages) > 0x200)
			prefetch_pages = 0x200 - prefetch_l3;
		if(prefetch_pages == 0)
			prefetch_pages = 1;

		uint32_t prefetch_sectors = prefetch_pages * SD_CACHE_PAGE_SECTORS;
		uint32_t prefetch_start = page * SD_CACHE_PAGE_SECTORS;
		if(bsp_sd_ensure_prefetch_buf(prefetch_sectors) != 0)
			return -1;
		if(mmc_read_blocks(prefetch_buf, prefetch_start, prefetch_sectors) != (int32_t)prefetch_sectors)
			return -1;

		for(uint32_t i = 0; i < prefetch_pages; i++) {
			uint32_t idx = prefetch_l3 + i;
			if(l3_entry[idx] == 0) {
				l3_entry[idx] = malloc(SD_CACHE_PAGE_SIZE);
				if(l3_entry[idx] == 0)
					continue;
			}
			memcpy(l3_entry[idx], prefetch_buf + i * SD_CACHE_PAGE_SIZE, SD_CACHE_PAGE_SIZE);
		}
		next_prefetch_pages = prefetch_pages;
		last_prefetch_end_page = page + prefetch_pages;
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

	if(mmc_write_blocks(sector, 1, buf) != 1)
		return -1;

	void **l3_entry = bsp_sd_get_l3(sector, 0);
	if(l3_entry != 0) {
		uint8_t *page = l3_entry[(sector >> 3) & 0x1FF];
		if(page != 0)
			memcpy(page + (sector & (SD_CACHE_PAGE_SECTORS - 1)) * 512, buf, 512);
	}
	return 0;
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
    last_prefetch_end_page = UINT32_MAX;
    next_prefetch_pages = SD_CACHE_MIN_PREFETCH_PAGES;
    int ret = sd_init(bsp_sd_init_by_info, bsp_sd_read_cache, bsp_sd_write_cache);
    if(ret == 0)
        sd_enable_sector_buffer(0);
    return ret;
}
