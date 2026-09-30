#ifndef __EV3_LED_DEV_H__
#define __EV3_LED_DEV_H__

#include <stdint.h>

/*
 * Client protocol for ledd (/dev/led).
 *
 *   read(fd, &led_state_t, sizeof)   -> current state
 *   write(fd, &led_state_t, sizeof)  -> apply state (all fields)
 *   dev_cntl(fd, LED_CNTL_SET)  in : led_state_t
 *   dev_cntl(fd, LED_CNTL_GET)  out: led_state_t
 *
 * When pattern != LED_PATTERN_NONE the four channel fields are ignored
 * and the daemon animates the LEDs; blink_ms > 0 toggles the current
 * channel state with that half period.
 */

#define LED_CNTL_SET  1
#define LED_CNTL_GET  2

#define LED_PATTERN_NONE     0
#define LED_PATTERN_GREEN_PULSE 1   /* both green, breathing         */
#define LED_PATTERN_RED_PULSE   2
#define LED_PATTERN_ORANGE      3   /* red + green on                 */
#define LED_PATTERN_ALTERNATE   4   /* left/right alternate green    */
#define LED_PATTERN_CYCLE       5   /* green -> orange -> red cycle   */

typedef struct {
    int32_t left_green;   /* 0 / 1 */
    int32_t left_red;
    int32_t right_green;
    int32_t right_red;
    int32_t blink_ms;     /* 0 = steady                              */
    int32_t pattern;      /* LED_PATTERN_*                            */
} led_state_t;

#endif
