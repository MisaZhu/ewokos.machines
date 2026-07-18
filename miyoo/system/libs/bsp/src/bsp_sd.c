#include <bsp/bsp_sd.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <sysinfo.h>
#include <ewoksys/syscall.h>
#include <sd/sd.h>
#include <arch/miyoo/sd.h>

/*
 * 顺序预读窗口。
 *
 * ext2 以 1KB(2 扇区)为单位读文件，直达驱动的做法会让每个 1KB
 * 都产生一次完整的卡交易(CMD18 + ADMA + CMD12)，交易开销占主导。
 * 本层在 bsp 维护一个只读顺序窗口：小块读未命中时一次性预读整个
 * 窗口，后续顺序小块读直接命中内存；落入窗口范围的写会使窗口失效。
 *
 * 多块拆分、ADMA、错误恢复与高速模式维护全部在 arch_miyoo 驱动内部
 * (miyoo_sd_read_blocks)，本层只做缓存，不改变任何底层时序。
 */
#define SD_RA_WINDOW_SECTORS 128U	/* 64KB，内部按 32 扇区分块发 CMD18 */
#define SD_RA_WINDOW_SIZE (SD_RA_WINDOW_SECTORS * 512U)

static uint8_t _ra_buf[SD_RA_WINDOW_SIZE];
static int32_t _ra_start = -1;
static uint32_t _ra_count = 0;

static inline bool sd_ra_hit(int32_t sector, uint32_t count) {
	return _ra_start >= 0 &&
		sector >= _ra_start &&
		(uint32_t)(sector - _ra_start) + count <= _ra_count;
}

static inline void sd_ra_invalidate(void) {
	_ra_start = -1;
	_ra_count = 0;
}

static int32_t bsp_sd_read_sectors(int32_t sector, void* buf, uint32_t count) {
	/* 大读旁路窗口，避免冲刷顺序流 */
	if(count >= SD_RA_WINDOW_SECTORS)
		return miyoo_sd_read_blocks(sector, buf, count);

	if(sd_ra_hit(sector, count)) {
		memcpy(buf, _ra_buf + (uint32_t)(sector - _ra_start) * 512U, count * 512U);
		return 0;
	}

	/* 未命中：整窗预读；失败(如卡尾越界)退回精确读 */
	if(miyoo_sd_read_blocks(sector, _ra_buf, SD_RA_WINDOW_SECTORS) == 0) {
		_ra_start = sector;
		_ra_count = SD_RA_WINDOW_SECTORS;
		memcpy(buf, _ra_buf, count * 512U);
		return 0;
	}
	sd_ra_invalidate();
	return miyoo_sd_read_blocks(sector, buf, count);
}

static int32_t bsp_sd_read_sector(int32_t sector, void* buf) {
	return bsp_sd_read_sectors(sector, buf, 1);
}

static int32_t bsp_sd_write_sector(int32_t sector, const void* buf) {
	if(sd_ra_hit(sector, 1))
		sd_ra_invalidate();
	return miyoo_sd_write_sector(sector, buf);
}

int bsp_sd_init(void) {
	sys_info_t sysinfo;

	syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&sysinfo);
	return sd_init_ex(miyoo_sd_init,
			bsp_sd_read_sector,
			bsp_sd_read_sectors,
			bsp_sd_write_sector);
}
