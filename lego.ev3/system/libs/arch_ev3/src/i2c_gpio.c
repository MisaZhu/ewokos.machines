/*
 * GPIO bit-bang I2C for the EV3 input ports.
 *
 * Mirrors the timer-driven state machine in ev3dev's legoev3-fiq.c (which
 * in turn is the LEGO lms2012 d_iic.c sequence):
 *   - SCL is always driven (no clock stretching support, like LEGO),
 *   - SDA is open-drain emulated: 0 = drive low, 1 = float (pin pull-up),
 *   - data bits change while SCL is low, are sampled while SCL is high,
 *   - after a byte is ACKed the bus idles a few half-periods before the
 *     next byte (4 after a write, 2 after a read),
 *   - between the write and read phase of a transfer LEGO issues
 *     STOP, one extra SCL pulse, START instead of a repeated START; the
 *     NXT ultrasonic sensor needs exactly this,
 *   - SDA is left floating after STOP so pin 6 can be used for detection.
 *
 * Each half-period is a busy wait (usleep <= 200 us spins), so a
 * transfer blocks the caller: ~1 ms per byte at 9.6 kHz.
 */
#include <stdint.h>
#include <string.h>
#include <ewoksys/proc.h>

#include "../include/arch/ev3/i2c.h"
#include "../include/arch/ev3/gpio.h"
#include "../include/arch/ev3/port.h"
#include "../include/arch/ev3/sensor_detect.h"

#define WAIT_AFTER_WRITE 4    /* half-periods (legoev3-fiq TRANSFER_WAIT) */
#define WAIT_AFTER_READ  2

static inline void tick(ev3_i2c_gpio_t* bus) {
    usleep((uint32_t)bus->half_us);
}

static inline void scl(ev3_i2c_gpio_t* bus, int32_t v) {
    ev3_gpio_write(bus->scl, v);
}

/* open-drain SDA: low = drive 0, high = release to the pull-up */
static inline void sda(ev3_i2c_gpio_t* bus, int32_t v) {
    if (v) {
        ev3_gpio_dir(bus->sda, 0);
    } else {
        ev3_gpio_write(bus->sda, 0);
        ev3_gpio_dir(bus->sda, 1);
    }
}

static inline int32_t sda_in(ev3_i2c_gpio_t* bus) {
    return ev3_gpio_read(bus->sda);
}

/* SDA high->low while SCL high, then SCL low (TRANSFER_START2) */
static void start(ev3_i2c_gpio_t* bus) {
    sda(bus, 1);
    scl(bus, 1);
    tick(bus);
    sda(bus, 0);
    tick(bus);
    scl(bus, 0);
    tick(bus);
}

/* SDA low->high while SCL high; SDA stays floating (TRANSFER_STOP..3) */
static void stop(ev3_i2c_gpio_t* bus) {
    sda(bus, 0);
    tick(bus);
    scl(bus, 1);
    tick(bus);
    sda(bus, 1);
    tick(bus);
}

/* NXT quirk: after STOP, one extra SCL low/high before the next START */
static void extra_clock(ev3_i2c_gpio_t* bus) {
    scl(bus, 0);
    tick(bus);
    scl(bus, 1);
    tick(bus);
}

/* shift one byte out, return 1 if the slave ACKed (TRANSFER_WBIT/RACK) */
static int32_t write_byte(ev3_i2c_gpio_t* bus, uint8_t b) {
    for (int32_t i = 0; i < 8; i++) {
        sda(bus, (b & 0x80) ? 1 : 0);
        b <<= 1;
        tick(bus);
        scl(bus, 1);
        tick(bus);
        scl(bus, 0);
    }
    /* ACK bit: release SDA, sample while SCL high */
    sda(bus, 1);
    tick(bus);
    scl(bus, 1);
    tick(bus);
    int32_t ack = !sda_in(bus);
    scl(bus, 0);
    return ack;
}

/* shift one byte in and ACK it (or NACK if last) (TRANSFER_RBIT/WACK) */
static uint8_t read_byte(ev3_i2c_gpio_t* bus, int32_t last) {
    uint8_t b = 0;
    sda(bus, 1);
    for (int32_t i = 0; i < 8; i++) {
        tick(bus);
        scl(bus, 1);
        tick(bus);
        b = (uint8_t)((b << 1) | (sda_in(bus) ? 1 : 0));
        scl(bus, 0);
    }
    sda(bus, last ? 1 : 0);
    tick(bus);
    scl(bus, 1);
    tick(bus);
    scl(bus, 0);
    return b;
}

static void idle(ev3_i2c_gpio_t* bus, int32_t half_periods) {
    for (int32_t i = 0; i < half_periods; i++)
        tick(bus);
}

/* ---------------- public API ---------------- */

int32_t ev3_i2c_gpio_open(ev3_i2c_gpio_t* bus, int32_t port, int32_t hz) {
    if (bus == NULL || port < 0 || port >= EV3_IN_PORT_COUNT)
        return -1;
    memset(bus, 0, sizeof(*bus));
    bus->port = -1;

    if (hz <= 0) hz = EV3_I2C_GPIO_HZ_NXT;
    if (hz > EV3_I2C_GPIO_HZ_MAX) hz = EV3_I2C_GPIO_HZ_MAX;
    bus->half_us = 500000 / hz;
    if (bus->half_us < 5) bus->half_us = 5;

    /* This is the NXT path: NXT I2C sensors are fed from the ~9 V battery
     * rail, so pin 1 (I_ON) is driven HIGH here - the deliberate exception to
     * the EV3 sensing rule (see sensor_detect.h). */
    ev3_input_port_power(port, 1);
    if (ev3_input_port_i2c_enable(port) != 0)
        return -1;
    bus->port = port;
    bus->scl = ev3_input_port_gpio(port, EV3_PIN5);
    bus->sda = ev3_input_port_gpio(port, EV3_PIN6);
    if (bus->scl < 0 || bus->sda < 0) {
        bus->port = -1;
        return -1;
    }

    /* idle bus: SCL driven high, SDA floating */
    ev3_gpio_write(bus->scl, 1);
    ev3_gpio_config(bus->scl, GPIO_OUTPUT);
    ev3_gpio_config(bus->sda, GPIO_INPUT);
    return 0;
}

void ev3_i2c_gpio_close(ev3_i2c_gpio_t* bus) {
    if (bus == NULL || bus->port < 0)
        return;
    ev3_gpio_config(bus->scl, GPIO_INPUT);
    ev3_gpio_config(bus->sda, GPIO_INPUT);
    /* Release the 9 V NXT supply: drive pin 1 back LOW (float). */
    ev3_input_port_power(bus->port, 0);
    bus->port = -1;
}

int32_t ev3_i2c_gpio_xfer(ev3_i2c_gpio_t* bus, uint8_t addr,
                          const uint8_t* wbuf, int32_t wlen,
                          uint8_t* rbuf, int32_t rlen) {
    if (bus == NULL || bus->port < 0)
        return -1;
    if (wlen < 0 || rlen < 0 || (wlen == 0 && rlen == 0))
        return -1;
    if ((wlen > 0 && wbuf == NULL) || (rlen > 0 && rbuf == NULL))
        return -1;
    addr &= 0x7F;

    if (wlen > 0) {
        start(bus);
        if (!write_byte(bus, (uint8_t)(addr << 1))) {
            bus->nacks++;
            stop(bus);
            return -2;
        }
        for (int32_t i = 0; i < wlen; i++) {
            idle(bus, WAIT_AFTER_WRITE);
            if (!write_byte(bus, wbuf[i])) {
                bus->nacks++;
                stop(bus);
                return -3;
            }
        }
        stop(bus);
        if (rlen > 0)
            extra_clock(bus);
    }

    if (rlen > 0) {
        start(bus);
        if (!write_byte(bus, (uint8_t)((addr << 1) | 1))) {
            bus->nacks++;
            stop(bus);
            return -2;
        }
        for (int32_t i = 0; i < rlen; i++) {
            idle(bus, (i == 0) ? WAIT_AFTER_WRITE : WAIT_AFTER_READ);
            rbuf[i] = read_byte(bus, i == rlen - 1);
        }
        stop(bus);
    }
    return 0;
}

int32_t ev3_i2c_gpio_write(ev3_i2c_gpio_t* bus, uint8_t addr,
                           const uint8_t* buf, int32_t len) {
    int32_t r = ev3_i2c_gpio_xfer(bus, addr, buf, len, NULL, 0);
    return (r == 0) ? len : r;
}

int32_t ev3_i2c_gpio_read(ev3_i2c_gpio_t* bus, uint8_t addr,
                          uint8_t* buf, int32_t len) {
    int32_t r = ev3_i2c_gpio_xfer(bus, addr, NULL, 0, buf, len);
    return (r == 0) ? len : r;
}

int32_t ev3_i2c_gpio_read_reg(ev3_i2c_gpio_t* bus, uint8_t addr, uint8_t reg,
                              uint8_t* buf, int32_t len) {
    int32_t r = ev3_i2c_gpio_xfer(bus, addr, &reg, 1, buf, len);
    return (r == 0) ? len : r;
}
