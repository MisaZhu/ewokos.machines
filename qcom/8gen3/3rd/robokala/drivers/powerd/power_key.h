/*
 * power_key.h - PMIC PON power key for the Robo & Kala TW220
 * (Qualcomm SC8280XP), read-only over the SPMI arbiter.
 *
 * The key state is the debounced KPDPWR real-time status bit of the PON
 * peripheral's INT_RT_STS register - the same signal mainline Linux's
 * pm8941-pwrkey driver reports as KEY_POWER (bit 0 on PON gen1/gen2,
 * bit 7 on gen3 / pmk8350).  The PON peripheral is discovered from the
 * APID map at init (pon@1300 HLOS / pon@800 legacy-PBS, peripheral type
 * 0x01); a candidate that reads "pressed" before any press is treated as
 * unwired/floating and skipped.  No PON interrupt, debounce or reset
 * configuration is written - observer reads only, like the battery path
 * this code was extracted from (8gen3 powerd/power_pmu.c).
 */
#ifndef POWER_KEY_H
#define POWER_KEY_H

#include <stdbool.h>

/* Map the SPMI arbiter windows and discover the PON power key.
 * Returns 0 when a usable key was selected, -1 otherwise (mapping
 * failure or no PON answers).  Safe to call more than once. */
int power_key_init(void);

/* Enable progress prints on stdout while initializing (diagnostics). */
void power_key_verbose(bool on);

/* Live key state: 1 = key down, 0 = key up, -1 = no usable PON or the
 * read failed (callers must keep their previous state on -1). */
int power_key_state(void);

/* Discovery result for diagnostics: selected PON apid/sid/subtype.
 * Returns 0 when a power key was selected, -1 otherwise (outs untouched). */
int power_key_info(int* apid, int* sid, int* subtype);

/* Print the PON candidate list and the selection (diagnostics). */
void power_key_dump(void);

#endif