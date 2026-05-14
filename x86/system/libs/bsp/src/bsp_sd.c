#include <bsp/bsp_sd.h>
#include <ewoksys/syscall.h>
#include <sd/sd.h>
#include <syscalls.h>

static int32_t x86_sd_init(void) {
	return 0;
}

static int32_t x86_sd_read_sector(int32_t sector, void* buf) {
	return syscall2(SYS_SD_READ_SECTOR, (ewokos_addr_t)sector, (ewokos_addr_t)buf);
}

static int32_t x86_sd_write_sector(int32_t sector, const void* buf) {
	return syscall2(SYS_SD_WRITE_SECTOR, (ewokos_addr_t)sector, (ewokos_addr_t)buf);
}

int bsp_sd_init(void) {
	return sd_init(x86_sd_init, x86_sd_read_sector, x86_sd_write_sector);
}
