#include <bsp/bsp_sd.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sd/sd.h>
#include <arch/ev3/sd.h>

/*
 * Page-cache acceleration modeled after the miyoo/raspix bsp layer:
 *
 * 1) A three-level radix page cache (4KB pages, malloc on demand) keeps
 *    the metadata ext2/ext3 re-reads constantly (superblock, group
 *    descriptors, bitmaps, inode tables, indirect blocks) in memory,
 *    so hits cost zero card transactions.
 * 2) Batched miss-fill: up to SD_CACHE_MAX_BATCH_PAGES pages are merged
 *    into one multi-block read (ev3_sd_read_sectors runs CMD18 chunks
 *    of up to 32 sectors internally), amortizing command overhead.
 * 3) Streaming bypass: large reads covering >=4 consecutive uncached full
 *    pages go straight into the caller's buffer, skipping the
 *    prefetch->page->out double copy.
 * 4) Write-through: writes go straight to the card; on success the
 *    cached pages covering the range are updated, on failure they are
 *    invalidated so stale data is never served afterwards.
 */

static void* cache_entry[4096] = {0};
static uint8_t* prefetch_buf = 0;
static uint32_t prefetch_buf_sectors = 0;

#define SD_CACHE_PAGE_SECTORS 8U
#define SD_CACHE_PAGE_SIZE (SD_CACHE_PAGE_SECTORS * 512U)
#define SD_CACHE_MAX_BATCH_PAGES 16U
#define SD_STREAM_MAX_BATCH_PAGES 32U

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

static int32_t bsp_sd_fill_page_cache(uint32_t start_page, uint32_t page_count) {
    uint32_t start_sector = start_page * SD_CACHE_PAGE_SECTORS;
    uint32_t sectors = page_count * SD_CACHE_PAGE_SECTORS;

    if(page_count == 0)
        return 0;
    if(bsp_sd_ensure_prefetch_buf(sectors) != 0)
        return -1;
    if(ev3_sd_read_sectors((int32_t)start_sector, prefetch_buf, sectors) != 0)
        return -1;

    for(uint32_t i = 0; i < page_count; i++) {
        uint32_t page = start_page + i;
        void **l3_entry = bsp_sd_get_l3(page * SD_CACHE_PAGE_SECTORS, 1);
        uint32_t l3 = page & 0x1FF;
        if(l3_entry == 0)
            return -1;
        if(l3_entry[l3] == 0) {
            l3_entry[l3] = malloc(SD_CACHE_PAGE_SIZE);
            if(l3_entry[l3] == 0)
                return -1;
        }
        memcpy(l3_entry[l3], prefetch_buf + i * SD_CACHE_PAGE_SIZE, SD_CACHE_PAGE_SIZE);
    }
    return 0;
}

static int32_t bsp_sd_read_cache_sectors(int32_t sector, void *buf, uint32_t count) {
    uint8_t *out = (uint8_t*)buf;
    uint32_t current_sector = (uint32_t)sector;
    uint32_t remaining = count;

    while(remaining > 0) {
        uint32_t page = current_sector >> 3;
        uint32_t sector_offset = current_sector & (SD_CACHE_PAGE_SECTORS - 1);
        void **l3_entry = bsp_sd_get_l3(current_sector, 1);
        uint32_t l3 = page & 0x1FF;
        if(l3_entry == 0)
            return -1;

        if(l3_entry[l3] == 0) {
            /* Streaming path: consecutive uncached full pages go direct */
            if(sector_offset == 0) {
                uint32_t max_run = 0x200 - l3;
                uint32_t run = remaining / SD_CACHE_PAGE_SECTORS;
                if(run > max_run)
                    run = max_run;
                if(run > SD_STREAM_MAX_BATCH_PAGES)
                    run = SD_STREAM_MAX_BATCH_PAGES;
                uint32_t uncached = 0;
                while(uncached < run && l3_entry[l3 + uncached] == 0)
                    uncached++;
                if(uncached >= 4) {
                    uint32_t run_sectors = uncached * SD_CACHE_PAGE_SECTORS;
                    if(ev3_sd_read_sectors((int32_t)current_sector, out, run_sectors) != 0)
                        return -1;
                    out += uncached * SD_CACHE_PAGE_SIZE;
                    current_sector += run_sectors;
                    remaining -= run_sectors;
                    continue;
                }
            }
            uint32_t needed_pages = (sector_offset + remaining + SD_CACHE_PAGE_SECTORS - 1) / SD_CACHE_PAGE_SECTORS;
            uint32_t max_pages = 0x200 - l3;
            if(needed_pages > max_pages)
                needed_pages = max_pages;
            if(needed_pages > SD_CACHE_MAX_BATCH_PAGES)
                needed_pages = SD_CACHE_MAX_BATCH_PAGES;
            if(bsp_sd_fill_page_cache(page, needed_pages) != 0)
                return -1;
            if(l3_entry[l3] == 0)
                return -1;
        }

        uint8_t *page_buf = l3_entry[l3];
        uint32_t sectors_to_copy = SD_CACHE_PAGE_SECTORS - sector_offset;
        if(sectors_to_copy > remaining)
            sectors_to_copy = remaining;
        memcpy(out, page_buf + sector_offset * 512U, sectors_to_copy * 512U);
        out += sectors_to_copy * 512U;
        current_sector += sectors_to_copy;
        remaining -= sectors_to_copy;
    }
    return 0;
}

static int32_t bsp_sd_read_cache(int32_t sector, void *buf){
    return bsp_sd_read_cache_sectors(sector, buf, 1);
}

static void bsp_sd_invalidate_sectors(uint32_t sector, uint32_t count) {
    for(uint32_t i = 0; i < count; i++) {
        void **l3_entry = bsp_sd_get_l3(sector + i, 0);
        if(l3_entry == 0)
            continue;
        uint32_t l3 = ((sector + i) >> 3) & 0x1FF;
        if(l3_entry[l3] != 0) {
            free(l3_entry[l3]);
            l3_entry[l3] = 0;
        }
    }
}

static int32_t bsp_sd_write_cache(int32_t sector, const void *buf){
    if(ev3_sd_write_sector(sector, buf) != 0) {
        bsp_sd_invalidate_sectors((uint32_t)sector, 1);
        return -1;
    }

    void **l3_entry = bsp_sd_get_l3(sector, 0);
    if(l3_entry != 0) {
        uint8_t *page = l3_entry[((uint32_t)sector >> 3) & 0x1FF];
        if(page != 0)
            memcpy(page + ((uint32_t)sector & (SD_CACHE_PAGE_SECTORS - 1)) * 512, buf, 512);
    }
    return 0;
}

static int32_t bsp_sd_write_cache_sectors(int32_t sector, const void *buf, uint32_t count) {
    const uint8_t *src = (const uint8_t*)buf;

    if(count == 0)
        return 0;
    if(ev3_sd_write_sectors(sector, buf, count) != 0) {
        bsp_sd_invalidate_sectors((uint32_t)sector, count);
        return -1;
    }

    for(uint32_t i = 0; i < count; i++) {
        uint32_t cur = (uint32_t)sector + i;
        void **l3_entry = bsp_sd_get_l3(cur, 0);
        if(l3_entry != 0) {
            uint8_t *page = l3_entry[(cur >> 3) & 0x1FF];
            if(page != 0)
                memcpy(page + (cur & (SD_CACHE_PAGE_SECTORS - 1)) * 512U,
                                src + i * 512U, 512U);
        }
    }
    return 0;
}

int bsp_sd_init(void) {
    int ret = sd_init_ex(ev3_sd_init,
        bsp_sd_read_cache,
        bsp_sd_read_cache_sectors,
        bsp_sd_write_cache,
        bsp_sd_write_cache_sectors,
        bsp_sd_flush);
    if(ret == 0)
        sd_enable_sector_buffer(0);
    return ret;
}
