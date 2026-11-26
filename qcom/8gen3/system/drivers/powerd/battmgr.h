/*
 * battmgr.h - ADSP firmware battery-manager client (PMIC GLINK battmgr).
 *
 * power_pmu.c can only see the SPMI charger/fuel-gauge registers, and on
 * this board the fuel gauge boots halted, so SOC/health/energy are NOT
 * available from the PMIC.  The real battery data is owned by the ADSP
 * firmware battery manager ("msm/adsp/charger_pd"), reached over the
 * PMIC_RTR_ADSP_APPS GLINK channel.  The kernel owns the channel; this
 * module speaks the wire protocol from battmgr_wire.h through the narrow
 * SYS_PLATFORM_CTL battery door (q8b_batt.h).
 *
 * Behaviour: probe once at startup.  When the kernel door reports
 * Q8B_BATT_UNSUPPORTED (no ADSP channel in this build) the client stays
 * quiet and power_pmu keeps its SPMI-only sources, so the daemon behaves
 * exactly as before.  When the channel exists, one property request is
 * issued per second on a round-robin and the replies fill the snapshot;
 * power_pmu then prefers these values (CAP_SRC_BATTMGR).
 */
#ifndef BATTMGR_H
#define BATTMGR_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool up;              /* channel open and the service is answering */
    bool adsp_booted;
    bool channel_open;
    bool pdr_ready;
    bool notif_armed;

    bool status_valid;    /* firmware charge status (power_supply enum) */
    uint32_t status;      /* 0 unknown, 1 charging, 2 discharging,
                           * 3 not-charging, 4 full */
    bool capacity_valid;  /* real state of charge */
    uint8_t capacity;     /* percent */
    bool present_valid;
    bool present;
    bool health_valid;
    uint8_t health;       /* firmware health code (POWER_SUPPLY_HEALTH) */
    bool soh_valid;
    uint8_t soh;          /* state of health, percent */
    bool volt_valid;
    int voltage_mv;
    bool curr_valid;
    int current_ua;
    bool temp_valid;
    int temp_dc;          /* deci-degrees Celsius */
    bool cycle_valid;
    uint32_t cycle_count;
    bool power_valid;
    int power_now_uw;     /* micro-watts, negative when charging? see fw */
    bool full_valid;      /* last full charge capacity (mWh on the wire) */
    uint32_t full_mwh;
    bool design_valid;    /* design capacity (mWh on the wire) */
    uint32_t design_mwh;

    uint32_t requests;    /* property requests issued */
    uint32_t replies;     /* matched property replies */
    uint32_t timeouts;    /* request deadlines missed */
} battmgr_snapshot_t;

/* Probe the kernel battery door and allocate the DMA window.  Returns 0
 * when the ADSP battmgr channel is usable, -1 otherwise (the daemon then
 * keeps the SPMI-only path; no retry is attempted later). */
int battmgr_init(void);

/* One time slice: drain replies and issue the next property request at most
 * once per second.  Safe to call unconditionally from the 1 s sampler. */
void battmgr_poll(void);

/* Current snapshot (never NULL).  Capacity is in percent; the other fields
 * are valid only when their *_valid flag is set. */
const battmgr_snapshot_t* battmgr_state(void);

/* 1 when the channel is up and the last capacity reply is fresh. */
bool battmgr_available(void);

#endif
