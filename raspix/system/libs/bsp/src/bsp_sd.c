#include <bsp/bsp_sd.h>
#include <stdint.h>
#include <sysinfo.h>
#include <string.h>
#include <ewoksys/syscall.h>
#include <sd/sd.h>
#include <arch/bcm283x/sd.h>


void* cache_entry[4096];

static int32_t bsp_sd_read_cache(uint32_t sector, void *buf){
	uint32_t l1 = (sector >> 21) & 0x1FF;
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
		bcm283x_sd_read_sector(sector&(~0x7), l3_entry[l3], 8);
	}
	uint8_t *page = l3_entry[l3];
	memcpy(buf, page + (sector & 0x7) * 512, 512);
	return 0;
}

static int32_t bsp_sd_write_cache(uint32_t sector, void *buf){
	uint32_t l1 = (sector >> 21) & 0x1FF;
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
		bcm283x_sd_read_sector(sector&(~0x7), l3_entry[l3], 8);
	}
	uint8_t *page = l3_entry[l3];
	memcpy(page + (sector & 0x7) * 512, buf, 512);
	return 0;
}

int bsp_sd_init(void) {
  sys_info_t sysinfo;
  syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&sysinfo);
  if(strstr(sysinfo.machine, "pi4") || strstr(sysinfo.machine, "cm4"))
      return sd_init(emmc2_init, emmc2_read_sector, emmc2_write_sector);
  else
    return sd_init(bcm283x_sd_init, bsp_sd_read_cache, bsp_sd_write_cache);
}

