/*
 * picocalc kernel-side SD driver.
 *
 * Follows the miyoo speed-up scheme:
 *   - real CMD18 multi-block reads (mmc_read_multi_blocks) replace the
 *     per-sector CMD17 loop;
 *   - adaptive read-ahead window: random reads use a small window (4
 *     sectors); once a sequential read is detected (sector == last completed
 *     sector + 1) it grows to 128 sectors, filled by one CMD18;
 *   - the sd_dev_read_blocks bulk interface reads bounce-sized chunks with
 *     CMD18 and copies them out, for whole-block reads on the ext2 load path.
 * dwmmc uses FIFO PIO: the CPU reads data out of the FIFO, so there is no DMA
 * coherency issue and the bounce buffer can be ordinary static memory.
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
 * Read-ahead window management
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

/* Fill the read-ahead window with one CMD18; on failure fall back to
 * single-block reads to stay usable */
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
 * Bulk read: CMD18 straight into the target when it is 4-byte aligned,
 * otherwise through the bounce buffer (the FIFO is read as 32-bit words and
 * needs a word-aligned destination).
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
