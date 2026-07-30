#ifndef __RTL_FIRMWARE_H__
#define __RTL_FIRMWARE_H__

#include <types.h>

/* runtime firmware blob location on the rootfs */
#define RTL_FW_PATH     "/etc/wifi/rtl8723bs_fw.bin"

/*
 * Load the firmware file into a malloc'ed buffer.
 * Returns 0 and fills buf/size on success (caller frees), -1 if the
 * file is missing or unreadable.
 */
int fw_load(const char *path, uint8_t **buf, uint32_t *size);

#endif /* __RTL_FIRMWARE_H__ */
