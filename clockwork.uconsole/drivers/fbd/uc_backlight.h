#ifndef UC_BACKLIGHT_H
#define UC_BACKLIGHT_H

#include <stdint.h>

/*
 * OCP8178 single-wire backlight controller on GPIO 9. The chip enters a
 * 1-wire configuration mode after a defined level sequence and then
 * accepts an address byte (0x72) followed by a 5-bit brightness code.
 * The ClockworkPi Linux driver (drivers/video/backlight/ocp8178_bl.c)
 * writes each level twice for reliability; we do the same.
 */
#define UC_BACKLIGHT_GPIO        9
#define UC_BACKLIGHT_MAX_LEVEL   9   /* index into raw-value table */
#define UC_BACKLIGHT_DEFAULT     5   /* matches devterm-panel-uc-overlay */

void uc_backlight_init(void);
void uc_backlight_set(uint8_t level); /* 0..UC_BACKLIGHT_MAX_LEVEL */

#endif
