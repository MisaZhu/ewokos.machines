/* bsp_bootfs.c: lego.ev3 has no firmware boot initramfs hand-off to sdfsd
   - no-op stub so the shared sdfsd links (bsp_bootfs contract).
   bsp_bootfs_map() always fails, so "sdfsd -b" exits with "boot initramfs
   unavailable". */
#include <bsp/bsp_bootfs.h>

int bsp_bootfs_map(void) {
    return -1;
}

void bsp_bootfs_unmap(void) {
}

int32_t bsp_bootfs_read_block(int32_t block, void* buf) {
    (void)block;
    (void)buf;
    return -1;
}

int32_t bsp_bootfs_read_blocks(int32_t block, void* buf, uint32_t count) {
    (void)block;
    (void)buf;
    (void)count;
    return -1;
}

int32_t bsp_bootfs_write_block(int32_t block, const void* buf) {
    (void)block;
    (void)buf;
    return -1;
}

int32_t bsp_bootfs_write_blocks(int32_t block, const void* buf, uint32_t count) {
    (void)block;
    (void)buf;
    (void)count;
    return -1;
}
