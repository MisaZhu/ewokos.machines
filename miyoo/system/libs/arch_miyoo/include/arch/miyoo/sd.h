#ifndef MIYOO_SD_H
#define MIYOO_SD_H

#include <stdint.h>

int32_t miyoo_sd_init(void);
int32_t miyoo_sd_read_sector(int32_t sector, void* buf);
int32_t miyoo_sd_write_sector(int32_t sector, const void* buf);
int32_t miyoo_sd_read_blocks(int32_t sector, void* buf, uint32_t count);
int32_t miyoo_sd_write_blocks(int32_t sector, const void* buf, uint32_t count);

#endif
