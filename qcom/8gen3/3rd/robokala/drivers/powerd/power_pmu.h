/*
 * power_pmu.h - SPMI battery/charger interface for the Robo & Kala TW220
 * (Qualcomm SC8280XP).
 *
 * The TW220's battery hardware is reached over the SPMI arbiter, the same
 * path goodixd uses on this board.  Power data is read-only:
 *  - the smbcharger-family charger PMIC (peripheral 0x15): charge state
 *    (BATTERY_CHARGER_STATUS_1) and battery presence status registers;
 *    enumerate() picks the live charger by a non-zero state read (SID10,
 *    apid 367/473 on the TW220);
 *  - the fuel gauge (peripheral 0x40): SOC_MONOTONIC / VOLTAGE_NOW when
 *    the PMIC's FG algorithm is running.
 *
 * IMPORTANT: the TW220 boots with the FG measurement engine HALTED (status
 * registers non-zero, every measurement register zero - verified on device).
 * No live SOC/voltage is therefore readable.  This is a Windows-on-ARM
 * laptop: the battery is owned by the ADSP firmware battery manager
 * (PMIC GLINK battmgr), which EwokOS does not boot, so the FG is never
 * enabled.  Capacity sources, in order: ADSP battmgr BATT_CAPACITY (the
 * real SOC, preferred once the ADSP/GLINK bring-up lands); live FG SOC
 * (when enabled); FULL_CHARGE state -> 100%; otherwise the last known
 * capacity remembered in /etc/powerd.state (lines "chg_sid N" / "cap N") -
 * a placeholder, NOT a real measurement.  Real capacity needs the firmware
 * battery manager.
 *
 * All peripherals are discovered at runtime from the SPMI APID map; if the
 * board has no usable charger PMIC every field simply reports "no battery".
 * The driver has NO SPMI write path: RW-window writes to battery PMIC
 * channels (FG/ADC) are PROVEN to wedge the TW220 SPMI bus (three frozen
 * boards during bring-up - see README), even for a no-op write-back.
 */
#ifndef POWER_PMU_H
#define POWER_PMU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Where the reported capacity came from (0 = no source at all). */
#define CAP_SRC_UNKNOWN    0U
#define CAP_SRC_CHARGER    1U  /* FULL_CHARGE -> 100% */
#define CAP_SRC_FG         2U  /* fuel-gauge SOC register */
#define CAP_SRC_REMEMBERED 3U  /* last known capacity from the state file */
#define CAP_SRC_BATTMGR    4U  /* ADSP firmware battery manager (PMIC GLINK):
                                * the real SOC; preferred once the ADSP/GLINK
                                * bring-up lands (see battmgr_wire.h) */

typedef struct {
    bool battery;        /* battery present (charger PMIC BATIF status) */
    bool charging;       /* charge state is PRE/FULL/FAST (smbcharger) */
    bool vbat_valid;     /* voltage sample is plausible */
    uint8_t capacity;    /* 0..100 percent; 0 also means "unknown" */
    uint8_t cap_src;     /* CAP_SRC_*: where capacity came from */
    int voltage_mv;      /* last VBAT_SNS voltage in millivolts, -1 unknown */
    int charger_sid;     /* SPMI SID of the charger PMIC, -1 if none */
    int vbat_sid;        /* SID the VBAT_SNS sample came from, -1 if none */
    uint32_t chgr_state; /* raw BATTERY_CHARGER_STATUS_1 bits[2:0] */
    uint32_t batif_sts;  /* raw BATIF INT_RT_STS register */
    uint32_t vbat_code;  /* raw ADC code of the last sample */
} power_sample_t;

/* Map the SPMI arbiter windows and discover the charger / fuel-gauge
 * peripherals.  Returns 0 on success, -1 when the arbiter itself cannot
 * be reached (peripheral absence is NOT an error - sampling then reports
 * "no battery"). */
int power_pmu_init(void);

/* Bootstrap the persisted daemon state without any SPMI access: remember
 * the wired charger SID and a starting capacity in /etc/powerd.state.
 * Run once at deployment: "powerd --seed <chg_sid> <cap>". */
int power_pmu_seed(int sid, int cap);

/* argv form of power_pmu_seed for the --seed diagnostic mode. */
int power_pmu_seed_args(int n, const char* const* args);

/* Enable progress prints on stdout while initializing (used by the
 * --dump bring-up mode to localize hangs). */
void power_pmu_verbose(bool on);

/* Take one sample into out.  Returns 0 when the PMU interface is up,
 * -1 otherwise (out is still fully initialized in that case). */
int power_pmu_sample(power_sample_t* out);

/* Staged diagnostics for --dump bring-up (each prints directly to stdout
 * so a hang leaves the last completed stage visible on the wire):
 *  dump_map    - version, channel counts, per-SID peripheral bitmaps;
 *  dump_probe  - one observer read per representative channel, streamed;
 *  dump_sample - one normal sample via power_pmu_sample();
 *  dump_mapall - full APID map listing (no hardware access);
 *  read        - raw observer reads of given (apid, reg, len) triples. */
int power_pmu_dump_map(void);
int power_pmu_dump_probe(void);
int power_pmu_dump_sample(void);
int power_pmu_dump_mapall(void);
int power_pmu_read(int n, const char* const* args);

#endif