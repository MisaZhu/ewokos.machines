/*
 * AM1808 / DA850 hardware I2C master (I2C0 / I2C1), polled.
 *
 * Register map and bit definitions follow Linux drivers/i2c/busses/
 * i2c-davinci.c. The controllers are not connected to the EV3 sensor
 * ports (see i2c_gpio.c for those); I2C0 is exposed on the board only.
 */
#include <stdint.h>
#include <ewoksys/mmio.h>

#include "../include/arch/ev3/i2c.h"
#include "../include/arch/ev3/gpio.h"

/* register offsets (32-bit access, low 16 bits meaningful) */
#define ICOAR    0x00
#define ICIMR    0x04
#define ICSTR    0x08
#define ICCLKL   0x0C
#define ICCLKH   0x10
#define ICCNT    0x14
#define ICDRR    0x18
#define ICSAR    0x1C
#define ICDXR    0x20
#define ICMDR    0x24
#define ICIVR    0x28
#define ICEMDR   0x2C
#define ICPSC    0x30

/* ICMDR (DAVINCI_I2C_MDR_*) */
#define MDR_NACK   (1u << 15)
#define MDR_STT    (1u << 13)
#define MDR_STP    (1u << 11)
#define MDR_MST    (1u << 10)
#define MDR_TRX    (1u << 9)
#define MDR_XA     (1u << 8)
#define MDR_RM     (1u << 7)
#define MDR_IRS    (1u << 5)

/* ICSTR (DAVINCI_I2C_STR_*) */
#define STR_BB     (1u << 12)
#define STR_RSFULL (1u << 11)
#define STR_SCD    (1u << 5)
#define STR_XRDY   (1u << 4)
#define STR_RRDY   (1u << 3)
#define STR_ARDY   (1u << 2)
#define STR_NACK   (1u << 1)
#define STR_AL     (1u << 0)
#define STR_ERR    (STR_NACK | STR_AL)

#define REG32(base, off)  (*(volatile uint32_t*)(_mmio_base + (base) + (off)))

/*
 * I2C0 runs from PLL0_AUXCLK (the 24 MHz oscillator) and is always on;
 * I2C1 is LPSC 11 in PSC1 and clocked by PLL0_SYSCLK4 (75 MHz).
 */
#define I2C0_CLK_HZ   24000000u
#define I2C1_CLK_HZ   75000000u
#define PSC1_BASE     0x01E27000
#define PSC1_LPSC_I2C1 11

/* da850.dtsi: i2c0_pins PINMUX4[15:8]=0x22, i2c1_pins PINMUX4[23:16]=0x44 */
#define I2C0_PINMUX_VAL  0x00002200u
#define I2C0_PINMUX_MASK 0x0000ff00u
#define I2C1_PINMUX_VAL  0x00440000u
#define I2C1_PINMUX_MASK 0x00ff0000u

#define I2C_TIMEOUT   200000

static void psc_enable(uint32_t psc_base, int32_t module) {
    volatile uint32_t* mdctl  = (volatile uint32_t*)(_mmio_base + psc_base + 0xA00 + 4*module);
    volatile uint32_t* mdstat = (volatile uint32_t*)(_mmio_base + psc_base + 0x800 + 4*module);
    volatile uint32_t* ptcmd  = (volatile uint32_t*)(_mmio_base + psc_base + 0x120);
    volatile uint32_t* ptstat = (volatile uint32_t*)(_mmio_base + psc_base + 0x128);
    int32_t t;
    if ((*mdstat & 0x1f) == 0x3)
        return;
    t = 100000; while (*ptstat && --t > 0) ;
    *mdctl = (*mdctl & ~0x1fu) | 0x3;   /* NEXT = ENABLE */
    *ptcmd = 0x1;
    t = 100000; while (*ptstat && --t > 0) ;
    t = 100000; while (((*mdstat & 0x1f) != 0x3) && --t > 0) ;
}

static inline uint32_t status(uint32_t base) {
    return REG32(base, ICSTR) & 0xFFFF;
}

static inline void clear_status(uint32_t base, uint32_t bits) {
    REG32(base, ICSTR) = bits;
}

static int32_t wait_bit(uint32_t base, uint32_t bit) {
    int32_t t = I2C_TIMEOUT;
    while (t-- > 0) {
        uint32_t s = status(base);
        if (s & STR_ERR)
            return -1;
        if (s & bit)
            return 0;
    }
    return -2;
}

static int32_t wait_bus_free(uint32_t base) {
    int32_t t = I2C_TIMEOUT;
    while (t-- > 0) {
        if (!(status(base) & STR_BB))
            return 0;
    }
    return -3;
}

/* abort the current transfer and put the controller back to idle */
static void recover(uint32_t base) {
    REG32(base, ICMDR) = MDR_IRS | MDR_MST | MDR_STP;
    for (int32_t t = 0; t < I2C_TIMEOUT; t++) {
        if (status(base) & STR_SCD)
            break;
    }
    clear_status(base, 0xFFFF);
}

int32_t ev3_i2c_init(uint32_t base, int32_t khz) {
    uint32_t input_clk;
    if (base == EV3_I2C0_BASE) {
        input_clk = I2C0_CLK_HZ;
        ev3_syscfg_write(EV3_SYSCFG_PINMUX(4), I2C0_PINMUX_VAL, I2C0_PINMUX_MASK);
    } else if (base == EV3_I2C1_BASE) {
        input_clk = I2C1_CLK_HZ;
        psc_enable(PSC1_BASE, PSC1_LPSC_I2C1);
        ev3_syscfg_write(EV3_SYSCFG_PINMUX(4), I2C1_PINMUX_VAL, I2C1_PINMUX_MASK);
    } else {
        return -1;
    }
    if (khz <= 0) khz = EV3_I2C_SPEED_100K;
    if (khz > EV3_I2C_SPEED_400K) khz = EV3_I2C_SPEED_400K;

    /* controller in reset while configuring */
    REG32(base, ICMDR) = 0;

    /*
     * i2c_davinci_calc_clk_dividers():
     *   module clk = input / (PSC + 1), keep it in 7..12 MHz
     *   SCL = module clk / ((ICCL + d) + (ICCH + d)),  d = 7 / 6 / 5
     *         for PSC = 0 / 1 / >1
     */
    uint32_t psc = input_clk / 7000000u - 1;
    if (input_clk / (psc + 1) > 12000000u)
        psc++;
    uint32_t d = (psc >= 2) ? 5 : 7 - psc;
    uint32_t clk = (input_clk / (psc + 1)) / ((uint32_t)khz * 1000u);
    if (clk > 2 * d) clk -= 2 * d; else clk = 2;
    uint32_t clkh = clk >> 1;
    uint32_t clkl = clk - clkh;

    REG32(base, ICPSC)  = psc;
    REG32(base, ICCLKH) = clkh;
    REG32(base, ICCLKL) = clkl;
    REG32(base, ICOAR)  = 0x08;      /* own address, unused as slave */
    REG32(base, ICIMR)  = 0;         /* polled */
    clear_status(base, 0xFFFF);

    REG32(base, ICMDR) = MDR_IRS;    /* out of reset */
    return 0;
}

int32_t ev3_i2c_write(uint32_t base, uint8_t addr, const uint8_t* buf, int32_t len) {
    if (buf == NULL || len <= 0 || len > 0xFFFF) return -1;
    if (wait_bus_free(base) != 0) return -3;

    clear_status(base, 0xFFFF);
    REG32(base, ICSAR) = addr & 0x7F;
    REG32(base, ICCNT) = (uint32_t)len;

    /* first byte goes into ICDXR before START to avoid an underrun */
    REG32(base, ICDXR) = buf[0];
    REG32(base, ICMDR) = MDR_IRS | MDR_MST | MDR_TRX | MDR_STT | MDR_STP;

    for (int32_t i = 1; i < len; i++) {
        if (wait_bit(base, STR_XRDY) != 0) {
            recover(base);
            return -4;
        }
        REG32(base, ICDXR) = buf[i];
    }

    if (wait_bit(base, STR_SCD) != 0) {
        int32_t r = (status(base) & STR_NACK) ? -6 : -5;
        recover(base);
        return r;
    }
    clear_status(base, STR_SCD);
    return len;
}

int32_t ev3_i2c_read(uint32_t base, uint8_t addr, uint8_t* buf, int32_t len) {
    if (buf == NULL || len <= 0 || len > 0xFFFF) return -1;
    if (wait_bus_free(base) != 0) return -3;

    clear_status(base, 0xFFFF);
    REG32(base, ICSAR) = addr & 0x7F;
    REG32(base, ICCNT) = (uint32_t)len;
    /* master receiver; the controller NACKs the last byte by itself */
    REG32(base, ICMDR) = MDR_IRS | MDR_MST | MDR_STT | MDR_STP;

    for (int32_t i = 0; i < len; i++) {
        if (wait_bit(base, STR_RRDY) != 0) {
            recover(base);
            return (status(base) & STR_NACK) ? -6 : -4;
        }
        buf[i] = (uint8_t)(REG32(base, ICDRR) & 0xFF);
    }

    if (wait_bit(base, STR_SCD) != 0) {
        recover(base);
        return -5;
    }
    clear_status(base, STR_SCD);
    return len;
}

int32_t ev3_i2c_read_reg(uint32_t base, uint8_t addr, uint8_t reg,
                         uint8_t* buf, int32_t len) {
    int32_t r = ev3_i2c_write(base, addr, &reg, 1);
    if (r != 1)
        return r;
    return ev3_i2c_read(base, addr, buf, len);
}
