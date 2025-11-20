#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sysinfo.h>
#include <ewoksys/syscall.h>
#include <ewoksys/mmio.h>
#include <arch/bcm283x/mmc.h>

int32_t bcm283x_sd_init(void) {
   sys_info_t sysinfo;
    _mmio_base = mmio_map();
   syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&sysinfo);
   if(strstr(sysinfo.machine, "pi4") || strstr(sysinfo.machine, "cm4"))
		return mmc_init(1);
   else
		return mmc_init(0);
}

int32_t bcm283x_sd_read_sector(int32_t sector, void* buf, int cnt) {
	mmc_read_blocks(buf, sector, cnt);
	return 0;
}

int32_t bcm283x_sd_write_sector(int32_t sector, const void* buf, int cnt) {
	mmc_write_blocks(sector, cnt, buf);
	return 0;
}

