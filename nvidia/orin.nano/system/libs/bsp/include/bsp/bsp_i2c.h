#ifndef ORIN_BSP_I2C_H
#define ORIN_BSP_I2C_H

#include <stdint.h>

/*
 * Tegra234 I2C master (tegra194-i2c, packet mode + MST FIFO, polled PIO).
 * bus selects the controller; see include/orin_net.h for bases/devices.
 * Transfers are limited to the FIFO depth (<=32 bytes per message).
 */
#define ORIN_I2C_BUS_GEN1  0   /* 0x3160000: module EEPROM 0x50, carrier 0x57 */
#define ORIN_I2C_BUS_GEN2  1   /* 0x0c240000: INA3221 0x40 */

int bsp_i2c_init(int bus);

/* Raw read/write (no register address). */
int bsp_i2c_read(int bus, uint8_t addr, void* buf, uint32_t len);
int bsp_i2c_write(int bus, uint8_t addr, const void* buf, uint32_t len);

/* Register access: write the 1-byte reg, repeated start, then read/write. */
int bsp_i2c_read_reg(int bus, uint8_t addr, uint8_t reg, void* buf, uint32_t len);
int bsp_i2c_write_reg(int bus, uint8_t addr, uint8_t reg,
        const void* buf, uint32_t len);

/* i2cd service (/dev/i2cN) dev_cntl commands.  in = proto {addr, reg, len,
 * data...}; the bus is fixed by the service instance (mnt point). */
#define ORIN_I2C_CMD_READ_REG   0x49320001
#define ORIN_I2C_CMD_WRITE_REG  0x49320002
#define ORIN_I2C_CMD_READ       0x49320003
#define ORIN_I2C_CMD_WRITE      0x49320004

#endif