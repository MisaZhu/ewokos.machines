/*
 * picocalc kernel-side SD driver.
 *
 * 参考 miyoo 的加速方案：
 *   - CMD18 真正的多块读（mmc_read_multi_blocks）替代 CMD17 逐扇区循环；
 *   - 自适应预读窗口：随机读用小窗口（4 扇区），检测到顺序读
 *     （sector == 上次完成扇区 + 1）时放大到 128 扇区，一次 CMD18 填满；
 *   - sd_dev_read_blocks 批量接口按 bounce 容量分块 CMD18 直读后拷出，
 *     供 ext2 加载路径整块读取。
 * dwmmc 走 FIFO PIO，数据由 CPU 从 FIFO 读出，无 DMA 一致性问题，
 * bounce buffer 用普通静态内存即可。
 */
#include <dev/sd.h>
#include <mm/mmu.h>

#define SD_BOUNCE_SECTORS   128U
#define SD_READAHEAD_SMALL  4U
#define SD_READAHEAD_LARGE  128U
#define SD_RETRY_COUNT      5

static uint8_t _sector_buf[SD_BOUNCE_SECTORS * 512] __attribute__((aligned(8)));

static int32_t _ra_start_sector = -1;
static uint32_t _ra_sector_count = 0;
static int32_t _pending_sector = -1;
static int32_t _last_done_sector = -1;

extern int dwmci_init(void);
extern int mmc_read_blocks(void *dst, uint32_t sector);
extern int mmc_read_multi_blocks(void *dst, uint32_t sector, uint32_t count);

/* ------------------------------------------------------------------
 * 预读窗口管理
 * ------------------------------------------------------------------ */
static inline int sd_ra_hit(int32_t sector) {
	return _ra_start_sector >= 0 &&
		sector >= _ra_start_sector &&
		(uint32_t)(sector - _ra_start_sector) < _ra_sector_count;
}

static inline void sd_ra_invalidate(void) {
	_ra_start_sector = -1;
	_ra_sector_count = 0;
	_pending_sector = -1;
}

static inline uint32_t sd_pick_ra_window(int32_t sector) {
	if(_last_done_sector >= 0 && sector == (_last_done_sector + 1))
		return SD_READAHEAD_LARGE;
	return SD_READAHEAD_SMALL;
}

static int sd_read_multi_retry(void* dst, uint32_t sector, uint32_t count) {
	int ret = -1, retry = SD_RETRY_COUNT;
	do {
		ret = mmc_read_multi_blocks(dst, sector, count);
		if(ret == 0)
			break;
	} while(retry--);
	return ret;
}

/* 一次 CMD18 填满预读窗口；失败退回单块读保证可用性 */
static int32_t sd_fill_ra_window(int32_t sector) {
	uint32_t window = sd_pick_ra_window(sector);

	if(sd_read_multi_retry(_sector_buf, (uint32_t)sector, window) == 0) {
		_ra_start_sector = sector;
		_ra_sector_count = window;
		return 0;
	}

	if(sd_read_multi_retry(_sector_buf, (uint32_t)sector, 1) == 0) {
		_ra_start_sector = sector;
		_ra_sector_count = 1;
		return 0;
	}

	sd_ra_invalidate();
	return -1;
}

int32_t sd_init(void) {
	sd_ra_invalidate();
	_last_done_sector = -1;
	dwmci_init();
	return 0;
}

int32_t sd_dev_read(int32_t sector) {
	if(sector < 0)
		return -1;

	if(!sd_ra_hit(sector)) {
		if(sd_fill_ra_window(sector) != 0)
			return -1;
	}

	_pending_sector = sector;
	return 0;
}

int32_t sd_dev_read_done(void* buf) {
	uint32_t offset;

	if(buf == 0 || !sd_ra_hit(_pending_sector))
		return -1;

	offset = (uint32_t)(_pending_sector - _ra_start_sector) * 512U;
	memcpy(buf, _sector_buf + offset, 512U);
	_last_done_sector = _pending_sector;
	return 0;
}

/*
 * 批量读：目标缓冲 4 字节对齐时 CMD18 直读，否则经 bounce 中转
 * （FIFO 按 32bit 字读出，要求目的地址字对齐）。
 */
int32_t sd_dev_read_blocks(int32_t sector, void* buf, uint32_t count) {
	uint8_t* out = (uint8_t*)buf;
	int direct = (((uint32_t)(uintptr_t)buf) & 3U) == 0;

	if(buf == 0 || count == 0 || sector < 0)
		return -1;

	sd_ra_invalidate();

	while(count > 0) {
		uint32_t chunk = (count > SD_BOUNCE_SECTORS) ?
				SD_BOUNCE_SECTORS : count;

		if(direct) {
			if(sd_read_multi_retry(out, (uint32_t)sector, chunk) != 0)
				return -1;
		} else {
			if(sd_read_multi_retry(_sector_buf, (uint32_t)sector, chunk) != 0)
				return -1;
			memcpy(out, _sector_buf, chunk * 512U);
		}

		sector += (int32_t)chunk;
		out += chunk * 512U;
		count -= chunk;
	}

	_last_done_sector = sector - 1;
	return 0;
}

int32_t sd_dev_write(int32_t sector, const void* buf) {
	(void)sector;
	(void)buf;
	return -1;
}

int32_t sd_dev_write_done(void) {
	return -1;
}
