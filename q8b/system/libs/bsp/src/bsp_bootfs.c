#include <stdint.h>
#include <string.h>
#include <bsp/bsp_bootfs.h>
#include <ewoksys/syscall.h>
#include <sysinfo.h>
#include <q8b_platform.h>

/*
 * Q8B boot-initramfs backend for sdfsd -b.
 *
 * The firmware hands over a complete boot-only filesystem image
 * (Q8B_RD_FLAG_BOOT_ONLY ramdisk) that has no persistent block device
 * behind it.  bsp_bootfs_map() maps that image into this process and the
 * read callbacks serve it straight from memory, read-only; the ext3
 * block size starts at 1024 and is re-derived from the superblock once
 * block 1 has been read.
 */

#define Q8B_BOOTFS_VA 0x60000000ULL
static uint8_t* _bootfs_va;
static uint32_t _bootfs_size;
static uint32_t _bootfs_block_size = 1024;

int bsp_bootfs_map(void) {
    sys_info_t info;
    const q8b_platform_data_t* pd;
    if (sys_get_sys_info(&info) != 0)
        return -1;
    pd = (const q8b_platform_data_t*)info.platform_data;
    if (pd == NULL || pd->rd.phy_base == 0 || pd->rd.size == 0 ||
            (pd->rd.flags & Q8B_RD_FLAG_BOOT_ONLY) == 0)
        return -1;
    ewokos_addr_t ret = syscall3(SYS_MEM_MAP, (ewokos_addr_t)Q8B_BOOTFS_VA,
            (ewokos_addr_t)pd->rd.phy_base, (ewokos_addr_t)pd->rd.size);
    if (ret != (ewokos_addr_t)Q8B_BOOTFS_VA)
        return -1;
    _bootfs_va = (uint8_t*)(uintptr_t)Q8B_BOOTFS_VA;
    _bootfs_size = pd->rd.size;
    _bootfs_block_size = 1024;
    return 0;
}

void bsp_bootfs_unmap(void) {
    if (_bootfs_va != NULL && _bootfs_size != 0)
        (void)syscall3(SYS_MEM_UNMAP, (ewokos_addr_t)Q8B_BOOTFS_VA,
                (ewokos_addr_t)_bootfs_size, 0);
    _bootfs_va = NULL;
    _bootfs_size = 0;
    _bootfs_block_size = 1024;
}

int32_t bsp_bootfs_read_block(int32_t block, void* buf) {
    uint64_t off;
    uint32_t bytes = _bootfs_block_size;
    if (block < 0 || buf == NULL || bytes == 0 || _bootfs_va == NULL)
        return -1;
    off = (uint64_t)(uint32_t)block * bytes;
    if (off + bytes < off || off + bytes > _bootfs_size)
        return -1;
    memcpy(buf, _bootfs_va + off, bytes);
    if (block == 1 && bytes == 1024) {
        const uint8_t* sb = (const uint8_t*)buf;
        uint32_t log = (uint32_t)sb[24];
        if (log <= 2)
            _bootfs_block_size = 1024U << log;
    }
    return 0;
}

int32_t bsp_bootfs_read_blocks(int32_t block, void* buf, uint32_t count) {
    uint32_t bytes = _bootfs_block_size;
    if (count == 0 || bytes == 0)
        return 0;
    for (uint32_t i = 0; i < count; i++) {
        if (bsp_bootfs_read_block(block + (int32_t)i,
                (uint8_t*)buf + i * bytes) != 0)
            return -1;
    }
    return 0;
}

int32_t bsp_bootfs_write_block(int32_t block, const void* buf) {
    (void)block;
    (void)buf;
    return -1;
}

int32_t bsp_bootfs_write_blocks(int32_t block, const void* buf,
        uint32_t count) {
    (void)block;
    (void)buf;
    (void)count;
    return -1;
}
