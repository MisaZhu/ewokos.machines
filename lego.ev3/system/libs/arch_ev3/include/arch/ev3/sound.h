#ifndef __EV3_SOUND_H__
#define __EV3_SOUND_H__

#include <stdint.h>

/*
 * EV3 beeper (speaker) driver.
 *
 * The speaker is driven by eHRPWM0 channel B (TBCLK = PLL0_SYSCLK2 =
 * 150 MHz). A GPIO enables the speaker amplifier; keeping the amp off
 * between tones avoids idle hiss and saves power.
 *
 * Pin assignment comes from the mainline da850-lego-ev3.dts "sound" node:
 *   eHRPWM0B pin     = PINMUX3[7:4] = 2 (da850.dtsi ehrpwm0b_pins)
 *   amp enable GPIO  = GP6[15] = pin 111, active high
 *
 * eHRPWM0 shares PSC1 module 17 with eHRPWM1, so calling ev3_pwm_init()
 * (motord does this at boot) already powers the block; ev3_sound_init()
 * is safe to call independently too — the PSC enable is idempotent.
 */

/* TBCLK for the eHRPWM time base (post CFGCHIP1.TBCLKSYNC gate). */
#define EV3_SOUND_TBCLK_HZ   150000000

/* Practical tone range: 100 Hz to 12 kHz. */
#define EV3_SOUND_MIN_FREQ   100
#define EV3_SOUND_MAX_FREQ   12000

/* Power on eHRPWM0 chB, mux the pin, configure the amp GPIO (off). */
void ev3_sound_init(void);

/* Enable or disable the speaker amplifier. When disabled the PWM still
 * runs but the speaker is muted. */
void ev3_sound_enable_amp(int on);

/* Start a continuous tone at freq_hz (0 = silence). The tone plays until
 * the next call to ev3_sound_tone() or ev3_sound_stop(). */
void ev3_sound_tone(int freq_hz);

/* Stop the current tone and mute the amp. */
void ev3_sound_stop(void);

/* Loudness 0..100 (PWM duty, 100 = 50 % duty). Applies to the next
 * ev3_sound_tone() call. */
void ev3_sound_set_volume(int percent);
int  ev3_sound_get_volume(void);

#endif
