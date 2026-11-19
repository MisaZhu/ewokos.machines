/*
 * power_lid.h - keyboard/lid hall sensor for the Robo & Kala TW220
 * (Qualcomm SC8280XP).
 *
 * The TW220's lid switch is a hall sensor on TLMM GPIO 107, active-low:
 * the keyboard folded onto the panel pulls the line low (closed), open
 * reads high.  This is exactly the signal the mainline Ntmer TW220
 * device tree wires to gpio-keys as SW_LID (GPIO_ACTIVE_LOW, bias
 * disabled), so the pad is read through the TLMM window goodixd already
 * proved safe on this board - no SPMI, no interrupt configuration.
 */
#ifndef POWER_LID_H
#define POWER_LID_H

/* TLMM pad wired to the hall sensor (mainline TW220 switch-lid). */
#define POWER_LID_GPIO 107U

/* Map the TLMM window and configure the hall pad as a GPIO input.
 * Returns 0 on success, -1 when the mapping failed (the daemon then
 * runs without lid detection).  Safe to call more than once. */
int power_lid_init(void);

/* Live lid state: 1 = keyboard closed, 0 = open, -1 = unavailable
 * (init failed; callers must keep their previous state on -1). */
int power_lid_state(void);

#endif