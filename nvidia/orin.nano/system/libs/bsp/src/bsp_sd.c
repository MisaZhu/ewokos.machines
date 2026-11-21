#include <stdint.h>
#include <string.h>
#include <bsp/bsp_sd.h>
#include <bsp/bsp_sdmmc.h>
#include <sd/sd.h>
#include <ewoksys/syscall.h>
#include <sysinfo.h>
#include <q8b_platform.h>

/*
 * Orin Nano user-space storage backend: the firmware boot RAM disk only.
 * This bring-up port has no persistent-disk driver, so every libsd consumer
 * (init's readfs, sdfsd) is served from the INITRAMFS.IMG the UEFI loader
 * handed over through bootinfo (published by the kernel as pd->rd).
 */

#define ORIN_RD_VA 0x60000000ULL

static uint8_t* _rd_va = NULL;
static uint32_t _rd_size = 0;

/* libsd init callback.  May be called once per process (init calls it again
 * in each forked child): SYS_MEM_MAP windows belong to one address space and
 * are not inherited across fork, so the mapping is rebuilt every time. */
static int32_t storage_init(void) {
    sys_info_t sysinfo;
    const q8b_platform_data_t* pd;

    if (sys_get_sys_info(&sysinfo) != 0)
        return -1;
    pd = (const q8b_platform_data_t*)sysinfo.platform_data;
    if (pd->rd.phy_base == 0 || pd->rd.size == 0)
        return -1;

    ewokos_addr_t ret = syscall3(SYS_MEM_MAP, (ewokos_addr_t)ORIN_RD_VA,
            (ewokos_addr_t)pd->rd.phy_base, (ewokos_addr_t)pd->rd.size);
    if (ret != (ewokos_addr_t)ORIN_RD_VA)
        return -1;

    _rd_va = (uint8_t*)(uintptr_t)ORIN_RD_VA;
    _rd_size = pd->rd.size;
    return 0;
}

static int32_t storage_read_sectors(int32_t sector, void* buf,
        uint32_t count) {
    if (sector < 0 || buf == NULL || count == 0)
        return -1;
    uint64_t off = (uint64_t)(uint32_t)sector * 512ULL;
    uint64_t bytes = (uint64_t)count * 512ULL;
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
    if (sector < 0 || buf == NULL || count == 0)
        return -1;
    uint64_t off = (uint64_t)(uint32_t)sector * 512ULL;
    uint64_t bytes = (uint64_t)count * 512ULL;
    if (_rd_va == NULL || off + bytes < off || off + bytes > _rd_size)
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

/* Physical microSD backend (SDHCI) for a persistent rootfs.  Selected by
 * sdfsd when booting from SD rather than the self-root boot RAMFS. */
static int32_t sdmmc_read_sectors(int32_t sector, void* buf, uint32_t count) {
    if (sector < 0 || buf == NULL || count == 0)
        return -1;
    return bsp_sdmmc_read_blocks((uint32_t)sector, buf, count);
}
static int32_t sdmmc_read_sector(int32_t sector, void* buf) {
    return sdmmc_read_sectors(sector, buf, 1U);
}
static int32_t sdmmc_write_sectors(int32_t sector, const void* buf,
        uint32_t count) {
    if (sector < 0 || buf == NULL || count == 0)
        return -1;
    return bsp_sdmmc_write_blocks((uint32_t)sector, buf, count);
}
static int32_t sdmmc_write_sector(int32_t sector, const void* buf) {
    return sdmmc_write_sectors(sector, buf, 1U);
}

int bsp_sd_init_physical(void) {
    return sd_init_ex(bsp_sdmmc_init, sdmmc_read_sector,
            sdmmc_read_sectors, sdmmc_write_sector,
            sdmmc_write_sectors, NULL);
}