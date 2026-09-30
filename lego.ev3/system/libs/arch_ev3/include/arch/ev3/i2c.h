#ifndef __EV3_I2C_H__
#define __EV3_I2C_H__

#include <stdint.h>

/*
 * Two I2C paths exist on the EV3:
 *
 * 1. Sensor ports (ev3_i2c_gpio_*): the four input ports have no hardware
 *    I2C controller. pin 5 (SCL) and pin 6 (SDA) are plain GPIOs that the
 *    LEGO firmware, ev3dev (legoev3-fiq.c) and this driver bit-bang at the
 *    NXT rate of ~9.6 kHz. The sequencing is deliberately non-standard for
 *    NXT ultrasonic compatibility: a register read is
 *        START addr+W reg STOP <extra SCL pulse> START addr+R data... STOP
 *    instead of a repeated START. SDA is left floating (input) between
 *    transfers so the port can still detect (dis)connection via pin 6.
 *
 * 2. SoC controllers I2C0 / I2C1 (ev3_i2c_*): AM1808 / DA850 master
 *    driver, register-level, following Linux i2c-davinci.c. Neither
 *    controller is routed to the sensor ports on the EV3 (I2C0 pins are
 *    free on the board connector); they are kept for expansion boards.
 */

/* ------------------------------------------------------------------ */
/* GPIO bit-bang I2C on an input port                                  */
/* ------------------------------------------------------------------ */

#define EV3_I2C_GPIO_HZ_NXT   9600     /* LEGO NXT/EV3 sensor bus rate */
#define EV3_I2C_GPIO_HZ_MAX   100000

typedef struct {
    int32_t port;        /* EV3_IN_PORT_x, -1 = closed */
    int32_t scl;         /* pin 5 GPIO */
    int32_t sda;         /* pin 6 GPIO */
    int32_t half_us;     /* half clock period in microseconds */
    int32_t nacks;       /* NACK counter (diagnostics) */
} ev3_i2c_gpio_t;

/* Power the port, route pin5/pin6 to GPIO and idle the bus (SCL high,
 * SDA floating). hz <= 0 selects EV3_I2C_GPIO_HZ_NXT. Returns 0 / -1. */
int32_t ev3_i2c_gpio_open(ev3_i2c_gpio_t* bus, int32_t port, int32_t hz);
void    ev3_i2c_gpio_close(ev3_i2c_gpio_t* bus);

/*
 * Generic transfer: write wlen bytes (if wlen > 0) then read rlen bytes
 * (if rlen > 0) using the NXT STOP + extra clock + START sequence between
 * the two phases. Either phase may be empty.
 * Returns 0 on success, -1 bad args, -2 address NACK, -3 data NACK.
 * A full 1-byte register read at 9.6 kHz takes roughly 4 ms and blocks
 * the caller for that time.
 */
int32_t ev3_i2c_gpio_xfer(ev3_i2c_gpio_t* bus, uint8_t addr,
                          const uint8_t* wbuf, int32_t wlen,
                          uint8_t* rbuf, int32_t rlen);

int32_t ev3_i2c_gpio_write(ev3_i2c_gpio_t* bus, uint8_t addr,
                           const uint8_t* buf, int32_t len);
int32_t ev3_i2c_gpio_read(ev3_i2c_gpio_t* bus, uint8_t addr,
                          uint8_t* buf, int32_t len);
int32_t ev3_i2c_gpio_read_reg(ev3_i2c_gpio_t* bus, uint8_t addr, uint8_t reg,
                              uint8_t* buf, int32_t len);

/* ------------------------------------------------------------------ */
/* SoC hardware I2C controllers                                        */
/* ------------------------------------------------------------------ */

#define EV3_I2C0_BASE   0x01C22000
#define EV3_I2C1_BASE   0x01E28000

/* Common I2C speeds in kHz. */
#define EV3_I2C_SPEED_100K   100
#define EV3_I2C_SPEED_400K   400

/* Power the module, mux its pins, reset the controller and configure the
 * bit rate (100 or 400 kHz). Returns 0 / -1. */
int32_t ev3_i2c_init(uint32_t base, int32_t khz);

/*
 * Master transmit: write len bytes to addr (7-bit).
 * Returns len on success, negative on error (bus busy, NACK, timeout).
 */
int32_t ev3_i2c_write(uint32_t base, uint8_t addr, const uint8_t* buf, int32_t len);

/* Master receive: read len bytes from addr (7-bit). */
int32_t ev3_i2c_read(uint32_t base, uint8_t addr, uint8_t* buf, int32_t len);

/*
 * Register read: START addr+W <reg> STOP, START addr+R <len bytes> STOP.
 * Two separate transactions (no repeated START), which is what most
 * simple I2C sensors expect.
 */
int32_t ev3_i2c_read_reg(uint32_t base, uint8_t addr, uint8_t reg,
                         uint8_t* buf, int32_t len);

#endif
