/*
 * firmware.c - load the RTL8723CS WLAN firmware blob from the rootfs.
 *
 * The MCU firmware is a proprietary Realtek blob (rtl8723bs_fw.bin from
 * linux-firmware/rtlwifi); ship it to /etc/wifi/ on the target.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <types.h>
#include <utils/log.h>

#include "firmware.h"

int fw_load(const char *path, uint8_t **buf, uint32_t *size) {
    int fd, rd, off;
    struct stat st;
    uint8_t *p;

    *buf = NULL;
    *size = 0;

    if (stat(path, &st) != 0 || st.st_size <= 0) {
        wifi_log("fw: %s not found\n", path);
        return -1;
    }

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        wifi_log("fw: open %s failed\n", path);
        return -1;
    }

    p = malloc(st.st_size);
    if (p == NULL) {
        close(fd);
        return -1;
    }

    off = 0;
    while (off < st.st_size) {
        rd = read(fd, p + off, st.st_size - off);
        if (rd <= 0)
            break;
        off += rd;
    }
    close(fd);

    if (off != st.st_size) {
        wifi_log("fw: short read %d/%d\n", off, (int)st.st_size);
        free(p);
        return -1;
    }

    *buf = p;
    *size = (uint32_t)st.st_size;
    wifi_log("fw: loaded %s (%d bytes)\n", path, *size);
    return 0;
}
