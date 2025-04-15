#ifndef __RK_I2C_H__
#define __RK_I2C_H__

#include <stdlib.h>

int rk_i2c_read(uint16_t addr, uint32_t reg, uint8_t* buf, int size, int flag);

int rk_i2c_write(uint16_t addr, uint32_t reg, uint8_t* buf, int size, int flag);

int rk_i2c_init(int bus);

#endif
