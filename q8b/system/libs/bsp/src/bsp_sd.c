#include <stdint.h>
#include <string.h>
#include <bsp/bsp_sd.h>
#include <bsp/sdcc2.h>
#include <sd/sd.h>
#include <ewoksys/sys.h>
#include <ewoksys/syscall.h>
#include <sysinfo.h>
#include <q8b_platform.h>

/*
 * Q8B user-space storage backend.
 *
 * The firmware RAM disk is preferred when present.  Otherwise the physical
 * card is driven directly by the user-space SDCC2 driver (sdcc2.c), which maps
 * the GCC/SDCC2/TLMM/RPMh windows itself.  The kernel no longer exposes a
 * block-read/write syscall: its only remaining SD use is loading init from the
 * card before user space starts.
 */

#define Q8B_RD_VA 0x60000000ULL

static uint8_t* _rd_va = NULL;
static uint32_t _rd_size = 0;
static int32_t _physical_sd = 0;

static int32_t physical_init_from_kernel(void) {
    sys_info_t sysinfo;
    const q8b_platform_data_t* pd;
    uint32_t stage;
    uint32_t multi;
    uint64_t sectors;

    if (sys_get_sys_info(&sysinfo) != 0)
        return -1;
    pd = (const q8b_platform_data_t*)sysinfo.platform_data;

    /* The kernel's loadinit path already identified the card before user
     * space started.  Reuse that result; every runtime read/write is still
     * issued by this process through sdcc2.c. */
    stage = pd->sd.state & 0xffffU;
    if ((pd->sd.state & 0xffff0000U) != 0x53440000U ||
            (stage & 0x8000U) != 0 || stage < 20U)
        return -1;
    if (sdcc2_remap() != 0)
        return -1;

    if (stage >= 24U) {
        sectors = pd->sd.detail;
        multi = 1;
    }
    else {
        /* stage 20-23 only proves LBA0/LBA1 are readable; the kernel's
         * detail field is not a capacity there. */
        sectors = 0xffffffffULL;
        multi = 0;
    }

    sdcc2_inherit(1, multi, sectors);
    _rd_va = NULL;
    _rd_size = 0;
    _physical_sd = 1;
    return 0;
}

/* Explicit maintenance path (sdprobe/sdupdate/sdinstall): run the full
 * user-space probe when a fresh identification is actually required. */
static int32_t physical_init_probe(void) {
    if (sdcc2_init() != 0)
        return -1;
    _rd_va = NULL;
    _rd_size = 0;
    _physical_sd = 1;
    return 0;
}

/*
 * libsd init callback.  bsp_sd_init() may be called once per process (init
 * calls it again in each forked child): SYS_MEM_MAP windows belong to one
 * address space and are not inherited across fork, so both branches below
 * must build the mapping again rather than assume the parent already did.
 * Neither branch re-runs card identification: the physical path adopts the
 * kernel's published SDCC2 state (sdcc2_inherit) and only re-maps registers.
 */
static int32_t storage_init(void) {
    sys_info_t sysinfo;
    const q8b_platform_data_t* pd;

    if (sys_get_sys_info(&sysinfo) != 0)
        return -1;
    pd = (const q8b_platform_data_t*)sysinfo.platform_data;

    if (pd->rd.phy_base == 0 || pd->rd.size == 0)
        return physical_init_from_kernel();

    ewokos_addr_t ret = syscall3(SYS_MEM_MAP, (ewokos_addr_t)Q8B_RD_VA,
            (ewokos_addr_t)pd->rd.phy_base, (ewokos_addr_t)pd->rd.size);
    if (ret != (ewokos_addr_t)Q8B_RD_VA)
        return -1;

    _rd_va = (uint8_t*)(uintptr_t)Q8B_RD_VA;
    _rd_size = pd->rd.size;
    _physical_sd = 0;
    return 0;
}

static int32_t storage_read_sectors(int32_t sector, void* buf,
        uint32_t count) {
    uint64_t off;
    uint64_t bytes;

    if (sector < 0 || buf == NULL || count == 0)
        return -1;
    if (_physical_sd)
        return sdcc2_read_blocks(sector, buf, count);

    off = (uint64_t)(uint32_t)sector * 512ULL;
    bytes = (uint64_t)count * 512ULL;
    if (_rd_va == NULL || off + bytes < off || off + bytes > _rd_size)
        return -1;
    memcpy(buf, _rd_va + off, (size_t)bytes);
    return 0;
}

static int32_t storage_read_sector(int32_t sector, void* buf) {
    return storage_read_sectors(sector, buf, 1U);
}

static int32_t storage_write_sectors(int32_t sector, const void* buf,
        uint32_t count) {
    uint64_t off;
    uint64_t bytes;

    if (sector < 0 || buf == NULL || count == 0)
        return -1;
    if (_physical_sd)
        return sdcc2_write_blocks(sector, buf, count);

    if (_rd_va == NULL)
        return -1;
    off = (uint64_t)(uint32_t)sector * 512ULL;
    bytes = (uint64_t)count * 512ULL;
    if (off + bytes < off || off + bytes > _rd_size)
        return -1;
    memcpy(_rd_va + off, buf, (size_t)bytes);
    return 0;
}

static int32_t storage_write_sector(int32_t sector, const void* buf) {
    return storage_write_sectors(sector, buf, 1U);
}

int bsp_sd_init(void) {
    return sd_init_ex(storage_init, storage_read_sector,
            storage_read_sectors, storage_write_sector,
            storage_write_sectors, NULL);
}

int bsp_sd_init_physical(void) {
    return sd_init_ex(physical_init_probe, storage_read_sector,
            storage_read_sectors, storage_write_sector,
            storage_write_sectors, NULL);
}
