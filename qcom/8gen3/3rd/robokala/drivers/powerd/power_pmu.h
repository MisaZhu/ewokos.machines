/*
 * power_pmu.h - SPMI battery/charger interface for the Robo & Kala TW220
 * (Qualcomm SC8280XP).
 *
 * The TW220's battery hardware is reached over the SPMI arbiter, the same
 * path goodixd uses on this board.  Power data comes from two places:
 *  - the SMB5-class charger PMIC (if the ODM fitted one): charge state and
 *    battery presence status registers (read-only access);
 *  - the PMIC ADC7's VBAT_SNS channel: a raw battery voltage sample that is
 *    converted to a capacity estimate with the uConsole voltage curve.
 *
 * Both peripherals are discovered at runtime from the SPMI APID map; if the
 * board has no charger PMIC (e.g. the battery is EC-managed) every field
 * simply reports "no battery".
 */
#ifndef POWER_PMU_H
#define POWER_PMU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool battery;        /* battery present (charger PMIC BATIF status) */
    bool charging;       /* SMB5 charge state machine is in a charge state */
    bool vbat_valid;     /* voltage sample is plausible */
    uint8_t capacity;    /* 0..100 percent; 0 also means "unknown" */
    int voltage_mv;      /* last VBAT_SNS voltage in millivolts, -1 unknown */
    int charger_sid;     /* SPMI SID of the charger PMIC, -1 if none */
    int vbat_sid;        /* SID the VBAT_SNS sample came from, -1 if none */
    uint32_t chgr_state; /* raw BATTERY_CHARGER_STATUS_1 bits[2:0] */
    uint32_t batif_sts;  /* raw BATIF INT_RT_STS register */
    uint32_t vbat_code;  /* raw ADC code of the last sample */
} power_sample_t;

/* Map the SPMI arbiter windows and discover the ADC7 / charger
 * peripherals.  Returns 0 on success, -1 when the arbiter itself cannot
 * be reached (peripheral absence is NOT an error - sampling then reports
 * "no battery"). */
int power_pmu_init(void);

/* Enable progress prints on stdout while initializing (used by the
 * --dump bring-up mode to localize hangs). */
void power_pmu_verbose(bool on);

/* Take one sample into out.  Returns 0 when the PMU interface is up,
 * -1 otherwise (out is still fully initialized in that case). */
int power_pmu_sample(power_sample_t* out);

/* Staged diagnostics for --dump bring-up (each prints directly to stdout
 * so a hang leaves the last completed stage visible on the wire):
 *  dump_map    - version, channel counts, per-SID peripheral bitmaps
 *                (pure core-register reads, proven safe);
 *  dump_probe  - one observer read per representative channel, streamed;
 *  dump_adc    - raw VBAT_SNS conversion attempts per ADC channel
 *                (includes RW-window register writes);
 *  dump_sample - one normal sample via power_pmu_sample();
 *  dump_mapall - full APID map listing (no hardware access);
 *  read        - raw observer reads of given (apid, reg, len) triples;
 *  write_args  - one gated RW-window write with pre/post reads. */
int power_pmu_dump_map(void);
int power_pmu_dump_probe(void);
int power_pmu_dump_adc(void);
int power_pmu_dump_sample(void);
int power_pmu_dump_mapall(void);
int power_pmu_read(int n, const char* const* args);
int power_pmu_write_args(int n, const char* const* args);

#endif
