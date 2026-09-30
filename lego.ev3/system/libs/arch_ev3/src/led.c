#include <stdint.h>
#include <ewoksys/mmio.h>

#include "../include/arch/ev3/led.h"
#include "../include/arch/ev3/gpio.h"

/*
 * Pin assignment from the mainline da850-lego-ev3.dts "leds" node,
 * all GPIO_ACTIVE_HIGH:
 *   led0:green (left)  = GP6[7]  = 103
 *   led0:red   (left)  = GP6[13] = 109
 *   led1:green (right) = GP6[14] = 110
 *   led1:red   (right) = GP6[12] = 108
 */
static const int _pins[EV3_LED_SIDE_COUNT][EV3_LED_COLOR_COUNT] = {
    /* left  */ { 103 /* green */, 109 /* red */ },
    /* right */ { 110 /* green */, 108 /* red */ },
};

static void write_raw(int side, int color, int on) {
    ev3_gpio_write(_pins[side][color], on ? 1 : 0);
}

void ev3_led_init(void) {
    for (int s = 0; s < EV3_LED_SIDE_COUNT; s++) {
        for (int c = 0; c < EV3_LED_COLOR_COUNT; c++) {
            ev3_gpio_config(_pins[s][c], GPIO_OUTPUT);
            write_raw(s, c, 0);   /* start dark */
        }
    }
}

void ev3_led_set(int side, int color, int on) {
    if (side < 0 || side >= EV3_LED_SIDE_COUNT) return;
    if (color < 0 || color >= EV3_LED_COLOR_COUNT) return;
    write_raw(side, color, on);
}

void ev3_led_all_off(void) {
    for (int s = 0; s < EV3_LED_SIDE_COUNT; s++)
        for (int c = 0; c < EV3_LED_COLOR_COUNT; c++)
            write_raw(s, c, 0);
}
