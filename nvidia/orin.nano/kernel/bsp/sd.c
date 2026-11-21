#include <dev/sd.h>
#include <kernel/hw_info.h>
#include <mm/mmu.h>
#include <stdint.h>
#include <string.h>
#include "hw_arch.h"

/*
 * Ramdisk-only block backend for the Orin Nano bring-up port.
 *
 * There is no physical SD/eMMC driver yet: the UEFI loader places an ext3
 * initramfs in DRAM and the kernel serves every "disk" access from it.
 * Writes land in the same DRAM copy (volatile rootfs, lost on reset).
 *
 * The boot reader hook keeps the shared kernel's weak symbol name
 * (q8b_ramdisk_read_block in kernel/lib/src/readfs.c) so the common
 * read_fs() path works without any shared-code change.
 */

static int32_t _pending_sector = -1;
static uint8_t _write_buf[512];

static void* rd_ptr(int32_t sector, uint32_t count) {
    uint64_t off = (uint64_t)(uint32_t)sector * 512ULL;
    uint64_t end = off + (uint64_t)count * 512ULL;
    if (orin_ramdisk_size == 0 || end > orin_ramdisk_size)
        return NULL;
    return (void*)P2V(orin_ramdisk_base + off);
}

/* Kernel boot reader hook used by kernel/lib/src/readfs.c.  The loader
 * image is a standalone ext3 filesystem, so its first filesystem block is
 * also the first block of the image (no partition offset). */
int32_t q8b_ramdisk_read_block(int32_t block, void* buf) {
    static uint32_t block_sectors = 2U;
    uint8_t* src;
    if (block < 0 || buf == NULL || orin_ramdisk_size == 0)
        return -1;
    if (block == 1 && block_sectors == 2U)
        src = (uint8_t*)rd_ptr(2, 8U);
    else
        src = (uint8_t*)rd_ptr(block * (int32_t)block_sectors, block_sectors);
    if (src == NULL)
        return -1;
    /* The reader's buffers are EXT3_MAX_BLOCK_SIZE bytes.  Copy a full
     * maximum-sized block so the initial superblock probe can reveal the
     * real block size; then use that size for subsequent calls. */
    memcpy(buf, src, (block == 1 && block_sectors == 2U) ? 4096U :
            block_sectors * 512U);
    if (block == 1 && ((uint32_t)((uint8_t*)buf)[24] <= 2U))
        block_sectors = 2U << ((uint32_t)((uint8_t*)buf)[24]);
    return 0;
}

int32_t sd_init(void) {
    if (orin_ramdisk_size == 0) {
        orin_dbg_puts("sd: no ramdisk published by the loader\n");
        return -1;
    }
    return 0;
}

int32_t sd_dev_read(int32_t sector) {
    _pending_sector = sector;
    return 0;
}

int32_t sd_dev_read_done(void* buf) {
    if (_pending_sector < 0)
        return -1;
    int32_t sector = _pending_sector;
    _pending_sector = -1;
    return sd_dev_read_blocks(sector, buf, 1U);
}

int32_t sd_dev_read_blocks(int32_t sector, void* buf, uint32_t count) {
    if (sector < 0 || buf == NULL || count == 0)
        return -1;
    void* src = rd_ptr(sector, count);
    if (src == NULL)
        return -1;
    memcpy(buf, src, (size_t)count * 512);
    return 0;
}

int32_t sd_dev_write(int32_t sector, const void* buf) {
    if (orin_ramdisk_size == 0 || sector < 0 || buf == NULL)
        return -1;
    _pending_sector = sector;
    memcpy(_write_buf, buf, sizeof(_write_buf));
    return 0;
}

int32_t sd_dev_write_done(void) {
    if (_pending_sector < 0)
        return -1;
    void* dst = rd_ptr(_pending_sector, 1);
    _pending_sector = -1;
    if (dst == NULL)
        return -1;
    memcpy(dst, _write_buf, sizeof(_write_buf));
    return 0;
}

void sd_dev_handle(void) {
}