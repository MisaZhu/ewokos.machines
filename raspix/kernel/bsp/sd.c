#include <bcm283x/sd.h>
#include <dev/sd.h>
#include <bcm283x/gpio.h>
#include "hw_arch.h"
#include <bcm283x/mailbox.h>
#include <kernel/hw_info.h>
#include <dev/timer.h>
#include <kstring.h>
#include <mm/kmalloc.h>

#define SD_CACHE_SMALL_SECTORS 8U
#define SD_CACHE_LARGE_SECTORS 32U
#define SD_CACHE_BUF_SECTORS SD_CACHE_LARGE_SECTORS
#define SD_CACHE_BUF_SIZE (SD_CACHE_BUF_SECTORS * 512U)

static uint32_t _sector = 0;
static uint32_t _cache_base_sector = 0;
static uint32_t _cache_sector_count = 0;
static uint8_t* _cache_buf = 0;
static int _cache_valid = 0;
static int32_t _last_read_sector = -1;
static uint32_t _seq_read_count = 0;

static int32_t sd_cache_ensure(void) {
        if(_cache_buf != 0)
                return 0;
        _cache_buf = (uint8_t*)kmalloc(SD_CACHE_BUF_SIZE);
        return _cache_buf == 0 ? -1 : 0;
}

static uint32_t sd_pick_cache_window(int32_t sector) {
        if(_last_read_sector >= 0 && sector == (_last_read_sector + 1)) {
                if(_seq_read_count >= 4U)
                        return SD_CACHE_LARGE_SECTORS;
        }
        return SD_CACHE_SMALL_SECTORS;
}

static int32_t sd_read_blocks_direct(int32_t sector, void* buf, uint32_t count) {
        uint8_t* out = (uint8_t*)buf;

        if(buf == 0 || count == 0)
                return -1;

        for(uint32_t i = 0; i < count; i++) {
                if(bcm283x_sd_read(sector + (int32_t)i, out + i * 512U, 1) != 0)
                        return -1;
        }
        return 0;
}

static int32_t sd_cache_fill(int32_t sector) {
        uint32_t start_sector;
        uint32_t count;

        if(sd_cache_ensure() != 0)
                return -1;

        count = sd_pick_cache_window(sector);
        if(count == SD_CACHE_LARGE_SECTORS) {
                start_sector = (uint32_t)sector;
        }
        else {
                start_sector = ((uint32_t)sector) & ~(SD_CACHE_SMALL_SECTORS - 1U);
        }

        if(sd_read_blocks_direct((int32_t)start_sector, _cache_buf, count) != 0) {
                _cache_valid = 0;
                return -1;
        }

        _cache_base_sector = start_sector;
        _cache_sector_count = count;
        _cache_valid = 1;
        return 0;
}

static int32_t sd_read_cached_sector(int32_t sector, void* buf) {
        uint32_t usector = (uint32_t)sector;

        if(buf == 0)
                return -1;

        if(!_cache_valid ||
                        usector < _cache_base_sector ||
                        usector >= (_cache_base_sector + _cache_sector_count)) {
                if(sd_cache_fill(sector) != 0)
                        return sd_read_blocks_direct(sector, buf, 1);
        }

        memcpy(buf, _cache_buf + ((usector - _cache_base_sector) * 512U), 512U);
        if(_last_read_sector >= 0 && sector == (_last_read_sector + 1)) {
                if(_seq_read_count < 0xffffffffU)
                        _seq_read_count++;
        }
        else {
                _seq_read_count = 0;
        }
        _last_read_sector = sector;
        return 0;
}

int32_t sd_init(void) {
    timer_init();
    if(_pi4){
        *(uint32_t*)(_sys_info.mmio.v_base + 0x2000d0) &= ~(0x2);
        //*(uint32_t*)(MMIO_BASE + 0x2000d0) |= 0x2;
    }
        _cache_valid = 0;
        _cache_sector_count = 0;
        _last_read_sector = -1;
        _seq_read_count = 0;
    return bcm283x_sd_init();
}

int32_t sd_dev_read(int32_t sector) {
    _sector = sector;
    return 0;
}

int32_t sd_dev_read_done(void* buf) {
        return sd_read_cached_sector((int32_t)_sector, buf);
}

int32_t sd_dev_read_blocks(int32_t sector, void* buf, uint32_t count) {
        uint8_t* out = (uint8_t*)buf;

        if(buf == 0 || count == 0)
                return -1;

        for(uint32_t i = 0; i < count; i++) {
                if(sd_read_cached_sector(sector + (int32_t)i, out + i * 512U) != 0)
                        return -1;
        }
        return 0;
}

int32_t sd_dev_write(int32_t sector, const void* buf) {
    return 0;
}

int32_t sd_dev_write_done(void) {
    return 0;
}
