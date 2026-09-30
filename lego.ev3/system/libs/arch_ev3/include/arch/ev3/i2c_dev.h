#ifndef __EV3_I2C_DEV_H__
#define __EV3_I2C_DEV_H__

#include <stdint.h>

/*
 * Client protocol for i2cd (/dev/i2c<port>), one daemon per input port,
 * bit-banging pin5/pin6 (see arch/ev3/i2c.h).
 *
 *   dev_cntl(fd, I2C_CNTL_XFER)   in/out: i2c_xfer_t
 *   dev_cntl(fd, I2C_CNTL_INFO)   out   : i2c_bus_info_t
 *   dev_cntl(fd, I2C_CNTL_SPEED)  in    : i2c_bus_info_t (hz used)
 *   write(fd, &i2c_xfer_t, sizeof)  -> perform the transfer, result kept
 *   read(fd, &i2c_xfer_t, sizeof)   -> result of the last transfer
 *
 * A transfer writes wlen bytes then reads rlen bytes (either may be 0)
 * using the NXT-compatible STOP / extra clock / START sequence.
 */

#define I2C_CNTL_XFER   1
#define I2C_CNTL_INFO   2
#define I2C_CNTL_SPEED  3

#define I2C_DEV_MAX_DATA 32

typedef struct {
    int32_t addr;          /* 7-bit slave address                     */
    int32_t wlen;          /* bytes of wdata to send                  */
    int32_t rlen;          /* bytes to read into rdata                */
    int32_t result;        /* out: 0 ok, -2 addr NACK, -3 data NACK   */
    uint8_t wdata[I2C_DEV_MAX_DATA];
    uint8_t rdata[I2C_DEV_MAX_DATA];
} i2c_xfer_t;

typedef struct {
    int32_t port;          /* input port 0..3                         */
    int32_t hz;            /* bus clock                               */
    int32_t xfers;         /* transfers since start                   */
    int32_t nacks;         /* NACKs since start                       */
} i2c_bus_info_t;

#endif
