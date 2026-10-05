#ifndef __EV3_BATTERY_DEV_H__
#define __EV3_BATTERY_DEV_H__

#include <stdint.h>

/*
 * Client protocol for batteryd (/dev/battery).
 *
 *   read(fd, &battery_info_t, sizeof)    -> latest measurement
 *   write(fd, &battery_cmd_t, sizeof)    -> set low threshold
 *   dev_cntl(fd, BATTERY_CNTL_GET)        out: battery_info_t
 *   dev_cntl(fd, BATTERY_CNTL_SET_THRESH) in : battery_cmd_t
 *
 * Measurement follows Linux drivers/power/supply/lego_ev3_battery.c:
 *   voltage = adc4 * 2000 + 50000 + adc3 * 1000 / 15   [uV]
 *   current = adc3 * 20000 / 15                        [uA]
 * (adc values in mV; 6xAA: 7.5 V full, 5.5 V empty; Li-ion: 8.4 / 7.1 V)
 */

#define BATTERY_CNTL_GET         1
#define BATTERY_CNTL_SET_THRESH  2

#define BATTERY_CMD_SET_THRESH   1   /* value = mV                      */

typedef struct {
    int32_t voltage_mv;
    int32_t current_ma;
    int32_t percent;          /* 0..100 estimate                       */
    int32_t low;              /* voltage_mv < low_threshold_mv         */
    int32_t rechargeable;     /* 1 = Li-ion pack detected (GPIO 136)   */
    int32_t low_threshold_mv;
    int32_t adc_voltage_mv;   /* raw ADC channel 4                     */
    int32_t adc_current_mv;   /* raw ADC channel 3                     */
} battery_info_t;

typedef struct {
    int32_t cmd;              /* BATTERY_CMD_*                         */
    int32_t value;
} battery_cmd_t;

#endif
