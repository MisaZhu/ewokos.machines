/*
 * BSP NVMe glue — maps PCIe windows, initialises the NVMe controller,
 * and provides a simple sector-cache wrapper around the raw block I/O.
 *
 * Layout follows bsp_sd.c: the init function is called from system
 * startup and registers read/write callbacks with the generic storage
 * layer (sd / bsd equivalent).
 */

#include <bsp/bsp_nvme.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <sysinfo.h>
#include <ewoksys/syscall.h>
#include <ewoksys/mmio.h>
#include <ewoksys/dma.h>
#include <ewoksys/klog.h>
#include <sd/sd.h>
#include <arch/bcm2712/nvme.h>
#include <arch/bcm2712/mmio.h>

/* ------------------------------------------------------------------ */
/*  Global controller state                                             */
/* ------------------------------------------------------------------ */
static nvme_ctrl_t _nvme_ctrl;
static bool _nvme_ready = false;

/* ------------------------------------------------------------------ */
/*  Sector cache (simple linear read-buffer — NVMe is fast enough)      */
/* ------------------------------------------------------------------ */

#define NVME_CACHE_PAGE_SECTORS 16U
#define NVME_CACHE_PAGE_SIZE    (NVME_CACHE_PAGE_SECTORS * 512U)
#define NVME_CACHE_PAGES        128U
#define NVME_CACHE_BUF_SIZE     (NVME_CACHE_PAGES * NVME_CACHE_PAGE_SIZE)

typedef struct {
    uint64_t	start_lba;	/* first LBA covered by this page  */
    bool		valid;
    uint8_t		data[NVME_CACHE_PAGE_SIZE];
} nvme_cache_page_t;

static nvme_cache_page_t *_cache = NULL;
static uint32_t _cache_pages = 0;

/* ------------------------------------------------------------------ */
/*  Cache helpers                                                        */
/* ------------------------------------------------------------------ */

static nvme_cache_page_t *nvme_cache_find(uint64_t lba) {
    if (_cache == NULL)
        return NULL;
    uint64_t page_lba = lba & ~(uint64_t)(NVME_CACHE_PAGE_SECTORS - 1);
    for (uint32_t i = 0; i < _cache_pages; i++) {
        if (_cache[i].valid && _cache[i].start_lba == page_lba)
            return &_cache[i];
    }
    return NULL;
}

static nvme_cache_page_t *nvme_cache_alloc(void) {
    if (_cache == NULL)
        return NULL;

    /* Simple round-robin eviction */
    static uint32_t next_victim = 0;
    nvme_cache_page_t *p = &_cache[next_victim];
    next_victim = (next_victim + 1) % _cache_pages;
    p->valid = false;
    return p;
}

static int nvme_cache_fill(nvme_cache_page_t *p, uint64_t start_lba) {
    if (!_nvme_ready)
        return -1;

    uint64_t page_lba = start_lba & ~(uint64_t)(NVME_CACHE_PAGE_SECTORS - 1);
    uint32_t to_read = NVME_CACHE_PAGE_SECTORS;

    /* Clamp to device capacity */
    if (page_lba + to_read > _nvme_ctrl.nsze)
        to_read = (uint32_t)(_nvme_ctrl.nsze - page_lba);
    if (to_read == 0)
        return -1;

    int ret = nvme_read_blocks(&_nvme_ctrl, p->data, page_lba, to_read);
    if (ret < 0)
        return -1;

    p->start_lba = page_lba;
    p->valid = true;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Init sequence                                                       */
/* ------------------------------------------------------------------ */

static int bsp_nvme_controller_init(void) {
    sys_info_t sysinfo;
    int ret;

    if (_nvme_ready)
        return 0;

    syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&sysinfo);
    _mmio_base = sysinfo.mmio.v_base;

    /* Map the standard peripheral windows (required by other drivers) */
    syscall3(SYS_MEM_MAP,
         (ewokos_addr_t)sysinfo.mmio.v_base,
         (ewokos_addr_t)sysinfo.mmio.phy_base,
         (ewokos_addr_t)sysinfo.mmio.size);
    syscall3(SYS_MEM_MAP,
         _mmio_base + PI5_EMMC_WIN_OFF,
         PI5_EMMC_PHY_WIN,
         PI5_EMMC_WIN_SIZE);
    syscall3(SYS_MEM_MAP,
         _mmio_base + PI5_RP1_WIN_OFF,
         PI5_RP1_PHY,
         PI5_RP1_WIN_SIZE);

    /* Probe and initialise NVMe */
    ret = nvme_probe_and_init(&_nvme_ctrl);
    if (ret != 0)
        return ret;
    if ((1U << _nvme_ctrl.lba_shift) != SECTOR_SIZE) {
        klog("nvmefsd: unsupported NVMe block size %u (expected %u)\n",
             1U << _nvme_ctrl.lba_shift, SECTOR_SIZE);
        nvme_shutdown(&_nvme_ctrl);
        return -1;
    }

    /* Allocate sector cache */
    _cache_pages = NVME_CACHE_PAGES;
    _cache = (nvme_cache_page_t *)calloc(_cache_pages,
                         sizeof(nvme_cache_page_t));
    if (_cache == NULL) {
        nvme_shutdown(&_nvme_ctrl);
        return -1;
    }

    _nvme_ready = true;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Block read / write                                                  */
/* ------------------------------------------------------------------ */

int32_t bsp_nvme_read(uint64_t start_lba, void *buf, uint32_t count) {
    if (!_nvme_ready || buf == NULL || count == 0)
        return -1;

    uint8_t *out = (uint8_t *)buf;
    uint64_t current_lba = start_lba;
    uint32_t remaining = count;

    while (remaining > 0) {
        /* Try cache first for single-page-aligned reads */
        nvme_cache_page_t *c = nvme_cache_find(current_lba);

        if (c == NULL) {
            c = nvme_cache_alloc();
            if (c != NULL)
                nvme_cache_fill(c, current_lba);
        }

        if (c != NULL && c->valid &&
            current_lba >= c->start_lba &&
            current_lba < c->start_lba + NVME_CACHE_PAGE_SECTORS) {

            uint32_t offset = (uint32_t)(current_lba - c->start_lba);
            uint32_t available = NVME_CACHE_PAGE_SECTORS - offset;
            uint32_t chunk = remaining < available ? remaining : available;

            memcpy(out, c->data + offset * 512U, chunk * 512U);
            out += chunk * 512U;
            current_lba += chunk;
            remaining -= chunk;
        } else {
            /* Direct I/O for bulk reads */
            uint32_t chunk = remaining;
            uint32_t max_chunk = _nvme_ctrl.max_transfer_blocks;
            if (chunk > max_chunk)
                chunk = max_chunk;

            int ret = nvme_read_blocks(&_nvme_ctrl, out,
                           current_lba, chunk);
            if (ret < 0)
                return -1;
            if ((uint32_t)ret != chunk)
                return -1;

            out += chunk * 512U;
            current_lba += chunk;
            remaining -= chunk;
        }
    }

    return (int32_t)count;
}

int32_t bsp_nvme_write(uint64_t start_lba, const void *buf, uint32_t count) {
    if (!_nvme_ready || buf == NULL || count == 0)
        return -1;

    /* Invalidate cache lines that overlap the write range */
    if (_cache != NULL) {
        for (uint32_t i = 0; i < _cache_pages; i++) {
            if (!_cache[i].valid)
                continue;
            uint64_t page_end = _cache[i].start_lba
                      + NVME_CACHE_PAGE_SECTORS;
            if (start_lba < page_end &&
                start_lba + count > _cache[i].start_lba) {
                _cache[i].valid = false;
            }
        }
    }

    const uint8_t *in = (const uint8_t *)buf;
    uint64_t current_lba = start_lba;
    uint32_t remaining = count;

    while (remaining > 0) {
        uint32_t chunk = remaining;
        uint32_t max_chunk = _nvme_ctrl.max_transfer_blocks;
        if (chunk > max_chunk)
            chunk = max_chunk;

        int ret = nvme_write_blocks(&_nvme_ctrl, current_lba,
                        chunk, in);
        if (ret < 0)
            return -1;
        if ((uint32_t)ret != chunk)
            return -1;

        in += chunk * 512U;
        current_lba += chunk;
        remaining -= chunk;
    }

    return (int32_t)count;
}

uint64_t bsp_nvme_get_block_count(void) {
    if (!_nvme_ready)
        return 0;
    return _nvme_ctrl.nsze;
}

uint32_t bsp_nvme_get_block_size(void) {
    if (!_nvme_ready)
        return 512;
    return 1U << _nvme_ctrl.lba_shift;
}

static int32_t bsp_nvme_sd_read_sector(int32_t sector, void *buf) {
    if (sector < 0)
        return -1;
    return bsp_nvme_read((uint64_t)(uint32_t)sector, buf, 1) == 1 ? 0 : -1;
}

static int32_t bsp_nvme_sd_read_sectors(int32_t sector, void *buf,
                    uint32_t count) {
    if (sector < 0)
        return -1;
    return bsp_nvme_read((uint64_t)(uint32_t)sector, buf, count)
        == (int32_t)count ? 0 : -1;
}

static int32_t bsp_nvme_sd_write_sector(int32_t sector, const void *buf) {
    if (sector < 0)
        return -1;
    return bsp_nvme_write((uint64_t)(uint32_t)sector, buf, 1) == 1 ? 0 : -1;
}

int bsp_nvme_init(void) {
    int ret = sd_init_ex(bsp_nvme_controller_init,
                 bsp_nvme_sd_read_sector,
                 bsp_nvme_sd_read_sectors,
                 bsp_nvme_sd_write_sector);
    if (ret == 0)
        sd_enable_sector_buffer(0);
    return ret;
}
