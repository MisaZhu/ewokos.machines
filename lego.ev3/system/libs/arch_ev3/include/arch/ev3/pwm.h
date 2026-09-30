#ifndef __EV3_PWM_H__
#define __EV3_PWM_H__

#include <stdint.h>

/*
 * EV3 motor PWM (TI AM1808 / DA850 eHRPWM + eCAP).
 *
 * The four output ports are driven by four independent PWM channels
 * (da850-lego-ev3.dts ev3-ports/outA..outD):
 *   Motor A -> eHRPWM1 channel B  (EPWM1B)
 *   Motor B -> eHRPWM1 channel A  (EPWM1A)
 *   Motor C -> eCAP0 APWM
 *   Motor D -> eCAP1 APWM
 *
 * All four are declared PWM_POLARITY_INVERTED with a 75758 ns period
 * (~13.2 kHz): the H-bridge input is active LOW, so "duty" is the time
 * the pin is driven low. The helpers below take care of that, callers
 * just pass the on-time.
 *
 * eHRPWM0 channel B is the speaker (see sound.h) and is not touched here.
 */

#define EV3_MOTOR_A  0
#define EV3_MOTOR_B  1
#define EV3_MOTOR_C  2
#define EV3_MOTOR_D  3
#define EV3_MOTOR_COUNT 4

/*
 * eHRPWM/eCAP functional clock is PLL0_SYSCLK2 (CPU/2 = 150 MHz on the
 * EV3, same source the UART divisor in uart.c is derived from).
 */
#define EV3_PWM_TBCLK_HZ  150000000

/* 75758 ns at 150 MHz. */
#define EV3_PWM_DEFAULT_PERIOD  11364

/* Enable PWMSS power domains, TBCLK sync and pinmux. Safe to call once. */
void ev3_pwm_init(void);

/* Set the time-base period (in TBCLK cycles) for one motor channel. */
void ev3_pwm_set_period(int motor, uint32_t period_cycles);

/* Set the on-time (in TBCLK cycles) for one motor channel. */
void ev3_pwm_set_duty(int motor, uint32_t duty_cycles);

/* Convenience: set duty as a percentage (0..100) of the current period. */
void ev3_pwm_set_percent(int motor, int percent);

/* Enable (1) / disable (0) the PWM output for one motor channel. When
 * disabled the output is held at its inactive level (0 % duty). */
void ev3_pwm_enable(int motor, int en);

#endif
