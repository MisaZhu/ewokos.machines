#ifndef __GPIO_H__
#define __GPIO_H__

#include <stdint.h>

/*
 * AM1808 / DA850 GPIO helpers for the EV3.
 *
 * Pin numbering is bank*16 + pin_in_bank, identical to the Linux
 * da850-lego-ev3.dts numbers (e.g. GP6[7] -> 103).
 *
 * All SYSCFG registers (PINMUX, CFGCHIPx, PUPD_*) are only writable in
 * privileged mode on this SoC, so every access to them goes through the
 * SYS_MMIO_RW syscall via ev3_syscfg_write(). Plain MMIO stores to those
 * registers from user space are silently ignored by the hardware.
 */

#define MODE_GPIO       0

/* ev3_gpio_config() modes */
#define GPIO_INPUT      0x00
#define GPIO_OUTPUT     0x01
/* Route the pin to an alternate function: pass EV3_GPIO_ALT(<pinmux value>) */
#define EV3_GPIO_ALT(m) (0x10 | ((m) & 0xF))

/* ev3_gpio_pull() */
#define GPIO_PULL_NONE  0x00
#define GPIO_PULL_DOWN  0x01
#define GPIO_PULL_UP    0x02

/* SYSCFG0 physical register addresses (for ev3_syscfg_write). */
#define EV3_SYSCFG0_BASE     0x01C14000
#define EV3_SYSCFG_CFGCHIP0  (EV3_SYSCFG0_BASE + 0x17C)
#define EV3_SYSCFG_CFGCHIP1  (EV3_SYSCFG0_BASE + 0x180)
#define EV3_SYSCFG_CFGCHIP2  (EV3_SYSCFG0_BASE + 0x184)
#define EV3_SYSCFG_CFGCHIP3  (EV3_SYSCFG0_BASE + 0x188)
#define EV3_SYSCFG_PINMUX(n) (EV3_SYSCFG0_BASE + 0x120 + (n)*4)

/* First DaVinci GPIO bank interrupt (bank n -> irq EV3_GPIO_IRQ_BASE + n). */
#define EV3_GPIO_IRQ_BASE    42
#define EV3_GPIO_BANK_COUNT  9

void     ev3_gpio_init(void);
void     ev3_gpio_config(int32_t pin, int32_t mode);
void     ev3_gpio_pull(int32_t pin, int32_t updown);
void     ev3_gpio_write(int32_t pin, int32_t value);
uint8_t  ev3_gpio_read(int32_t pin);

/* Change only the direction register (1 = output, 0 = input) of a pin that
 * is already muxed as GPIO. Cheaper than ev3_gpio_config (no syscall). */
void     ev3_gpio_dir(int32_t pin, int32_t output);

/*
 * Masked write to a SYSCFG register: reg = (reg & ~mask) | (val & mask).
 * phys is the physical register address (EV3_SYSCFG_*). Returns the
 * syscall result (0 on success).
 */
uint32_t ev3_syscfg_write(uint32_t phys, uint32_t val, uint32_t mask);

/*
 * GPIO edge interrupts. Each 16-pin bank has its own AINTC line
 * (ev3_gpio_irq_num). Enable rising and/or falling edge detection on a
 * pin and the bank interrupt; the caller registers the handler with
 * sys_interrupt_setup(ev3_gpio_irq_num(pin), ...).
 */
uint32_t ev3_gpio_irq_num(int32_t pin);
void     ev3_gpio_irq_enable(int32_t pin, int32_t rising, int32_t falling);
void     ev3_gpio_irq_disable(int32_t pin);

/*
 * Read-and-clear the pending edge status of the 32-pin register group
 * that contains pin. Bit (pin % 32) is set when that pin fired.
 */
uint32_t ev3_gpio_irq_ack(int32_t pin);

#endif
