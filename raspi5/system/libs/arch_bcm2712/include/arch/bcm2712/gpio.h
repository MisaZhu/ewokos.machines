#ifndef BCM2712_GPIO_H
#define BCM2712_GPIO_H

#include <stdint.h>
#include <stdbool.h>
#include <arch/bcm2712/mmio.h>

/*
 * RP1 GPIO for BCM2712 (Raspberry Pi 5).
 *
 * The register layout below comes from the official device tree
 * (arch/arm64/boot/dts/broadcom/rp1.dtsi) and from the linux driver
 * drivers/pinctrl/pinctrl-rp1.c. It has nothing in common with the BCM283x
 * GPIO block: there is no FSEL/SET/CLR/LEV register file, but three separate
 * regions, and the pins are described one struct per pin instead of one bit
 * per pin.
 *
 *   rp1_gpio: gpio@d0000 {
 *       reg = <0xc0 0x400d0000  0x0 0xc000>,   // IO_BANK: per pin status+ctrl
 *             <0xc0 0x400e0000  0x0 0xc000>,   // SYS_RIO: out/oe/in bitmaps
 *             <0xc0 0x400f0000  0x0 0xc000>;   // PADS_BANK: pull, drive, ...
 *       compatible = "raspberrypi,rp1-gpio";
 *
 * Each region holds three banks at a 0x4000 stride. The 54 pins are split
 * 28 / 6 / 20 over those banks (rp1_iobanks[] in pinctrl-rp1.c), *not*
 * 28 / 14 / 12.
 */

#define RP1_IO_BANK_OFF     (PI5_RP1_WIN_OFF + 0x000d0000)
#define RP1_SYS_RIO_OFF     (PI5_RP1_WIN_OFF + 0x000e0000)
#define RP1_PADS_BANK_OFF   (PI5_RP1_WIN_OFF + 0x000f0000)

#define RP1_BANK_STRIDE     0x4000
#define RP1_NUM_BANKS       3
#define RP1_NUM_GPIOS       54

/*
 * Every register of every region is aliased four times: the plain read/write
 * view plus three atomic read-modify-write views. Using SET/CLR avoids the
 * read-modify-write races the BCM283x driver had to live with.
 */
#define RP1_RW_OFFSET       0x0000
#define RP1_XOR_OFFSET      0x1000
#define RP1_SET_OFFSET      0x2000
#define RP1_CLR_OFFSET      0x3000

/* IO_BANK: two words per pin */
#define RP1_GPIO_STATUS     0x00
#define RP1_GPIO_CTRL       0x04
#define RP1_GPIO_PIN_STRIDE 0x08

#define RP1_GPIO_CTRL_FUNCSEL_LSB   0
#define RP1_GPIO_CTRL_FUNCSEL_MASK  0x0000001f
#define RP1_GPIO_CTRL_OUTOVER_LSB   12
#define RP1_GPIO_CTRL_OUTOVER_MASK  0x00003000
#define RP1_GPIO_CTRL_OEOVER_LSB    14
#define RP1_GPIO_CTRL_OEOVER_MASK   0x0000c000

#define RP1_OUTOVER_PERI    0
#define RP1_OEOVER_PERI     0
#define RP1_OEOVER_DISABLE  2

/* SYS_RIO: one bit per pin within the bank */
#define RP1_RIO_OUT         0x00
#define RP1_RIO_OE          0x04
#define RP1_RIO_IN          0x08

/* PADS_BANK: one word per pin, the first word is VOLTAGE_SELECT */
#define RP1_PADS_PIN0       0x04
#define RP1_PAD_PIN_STRIDE  0x04
#define RP1_PAD_PULL_LSB    2
#define RP1_PAD_PULL_MASK   0x0000000c
#define RP1_PAD_IN_ENABLE_MASK   0x00000040
#define RP1_PAD_OUT_DISABLE_MASK 0x00000080

/*
 * Function select values are raw RP1 funcsel codes. funcsel 5 is the
 * software controlled function (SYS_RIO), so it is what both directions of a
 * plain GPIO use; the direction itself lives in RIO_OE, not in funcsel.
 * GPIO_FUNC_INPUT / GPIO_FUNC_OUTPUT are funcsel 5 plus the matching OE.
 */
#define GPIO_FUNC_ALTF0  0
#define GPIO_FUNC_ALTF1  1
#define GPIO_FUNC_ALTF2  2
#define GPIO_FUNC_ALTF3  3
#define GPIO_FUNC_ALTF4  4
#define GPIO_FUNC_RIO    5
#define GPIO_FUNC_ALTF6  6
#define GPIO_FUNC_ALTF7  7
#define GPIO_FUNC_ALTF8  8
#define GPIO_FUNC_NONE   9

#define GPIO_FUNC_DIR_FLAG 0x10
#define GPIO_FUNC_INPUT  (GPIO_FUNC_DIR_FLAG | GPIO_FUNC_RIO)
#define GPIO_FUNC_OUTPUT (GPIO_FUNC_DIR_FLAG | GPIO_FUNC_RIO | 0x20)

/* Pull resistor values, same encoding as PADS_BANK bits 3:2 */
#define GPIO_PULL_NONE   0
#define GPIO_PULL_DOWN   1
#define GPIO_PULL_UP     2

void bcm2712_gpio_init(void);
void bcm2712_gpio_config(uint32_t pin, uint32_t func);
void bcm2712_gpio_pull(uint32_t pin, uint32_t pull);
void bcm2712_gpio_write(uint32_t pin, bool high);
bool bcm2712_gpio_read(uint32_t pin);

#endif
