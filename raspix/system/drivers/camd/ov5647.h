#ifndef OV5647_H
#define OV5647_H

#include <stdint.h>
#include <stdio.h>

/* OV5647 I2C 7-bit slave address */
#define OV5647_I2C_ADDR 0x36

/* Chip ID registers */
#define OV5647_REG_CHIPID_HI 0x300A
#define OV5647_REG_CHIPID_LO 0x300B
#define OV5647_CHIPID_HI     0x56
#define OV5647_CHIPID_LO     0x47

/* Key control registers */
#define OV5647_REG_SW_STANDBY    0x0100  /* 0x01 = active, 0x00 = standby */
#define OV5647_REG_SW_RESET      0x0103
#define OV5647_REG_PAD_OUT       0x300D  /* pad output enable */
#define OV5647_REG_FRAME_OFF_NUM 0x4202  /* frame off number */
#define OV5647_REG_MIPI_CTRL00   0x4800  /* MIPI control */
#define OV5647_REG_MIPI_CTRL14   0x4814  /* MIPI virtual channel */

/* Supported modes.
 * OV5647 is a RAW Bayer sensor: only RAW8/RAW10 output is possible. */
#define OV5647_MODE_640x480 0  /* RAW8 (SBGGR8), 2x2 binned */

/**
 * Initialize I2C bus and verify sensor presence.
 * @param sda_gpio  GPIO number for I2C SDA
 * @param scl_gpio  GPIO number for I2C SCL
 * @return 0 on success, -1 on failure
 */
int ov5647_init(int32_t sda_gpio, int32_t scl_gpio);

/**
 * Configure sensor output mode (resolution + format).
 * Leaves the sensor in stream-off (MIPI LP-11) state.
 * @param mode  OV5647_MODE_640x480
 * @return 0 on success, -1 on failure
 */
int ov5647_set_mode(int mode);

/**
 * Start streaming (sensor outputs frames on CSI-2 bus).
 */
int ov5647_stream_on(void);

/**
 * Stop streaming (MIPI clock lane gated, LP-11).
 */
int ov5647_stream_off(void);

/**
 * Get the bytes-per-pixel for the current mode (RAW8 = 1).
 */
int ov5647_bytes_per_pixel(int mode);

#endif /* OV5647_H */
