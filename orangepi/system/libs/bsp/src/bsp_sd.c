#include <bsp/bsp_sd.h>
#include <stdint.h>
#include <sysinfo.h>
#include <string.h>
#include <ewoksys/syscall.h>
#include <sd/sd.h>
#include <arch/orangepi/sd.h>

int bsp_sd_init(void) {
  sys_info_t sysinfo;
  syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&sysinfo);
  int res = sd_init(orangepi_sd_init, orangepi_sd_read_sector, orangepi_sd_write_sector);
	return res;
}

