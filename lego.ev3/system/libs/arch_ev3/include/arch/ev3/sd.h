#ifndef EV3_SD_H
#define EV3_SD_H

#include <stdint.h>

int32_t ev3_sd_init(void);
int32_t ev3_sd_read_sector(int32_t sector, void* buf);
int32_t ev3_sd_read_sectors(int32_t sector, void* buf, uint32_t count);
int32_t ev3_sd_write_sector(int32_t sector, const void* buf);
int32_t ev3_sd_write_sectors(int32_t sector, const void* buf, uint32_t count);

#endif
