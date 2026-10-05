#ifndef __EV3_LED_H__
#define __EV3_LED_H__

/*
 * EV3 status LEDs.
 *
 * The EV3 has two dual-color (red/green) LEDs on the front panel, one on
 * each side of the LCD. All four are driven by GPIOs that are active high
 * (write 1 = LED lit, write 0 = LED off).
 *
 * Pin assignment comes from the mainline da850-lego-ev3.dts "leds" node:
 *   led0:green (left)  = GP6[7]  = pin 103
 *   led0:red   (left)  = GP6[13] = pin 109
 *   led1:green (right) = GP6[14] = pin 110
 *   led1:red   (right) = GP6[12] = pin 108
 *
 * ev3_gpio_config() transparently routes these to GPIO mode via the
 * SYS_MMIO_RW pinmux syscall, so no extra pinmux code is needed here.
 */

#define EV3_LED_LEFT   0
#define EV3_LED_RIGHT  1
#define EV3_LED_SIDE_COUNT 2

#define EV3_LED_GREEN  0
#define EV3_LED_RED    1
#define EV3_LED_COLOR_COUNT 2

/* Configure all four LED pins as outputs and turn them off. */
void ev3_led_init(void);

/* Set one LED. on = 1 lights it, on = 0 turns it off. */
void ev3_led_set(int side, int color, int on);

/* Convenience: turn off all LEDs. */
void ev3_led_all_off(void);

#endif
