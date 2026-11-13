/*
 * power_pmu.c - SPMI battery/charger access for the Robo & Kala TW220
 * (Qualcomm SC8280XP).
 *
 * All hardware access goes through the SPMI arbiter using the same core/
 * cfg/obs window pattern proven by goodixd on this board (see
 * ../goodixd/goodixd.c).  Nothing else is mapped: GENI/GCC are unvalidated
 * on the TW220 and must not be touched.
 *
 * Arbitration model (mirrors the kernel spmi-pmic-arb v5 driver):
 *   - the APID map lives at core+0x900+4*n and covers up to 512 slots; a
 *     zero word marks an unused slot, bits[8:19] hold the PPID
 *     (peripheral = bits[8:15], SID = bits[16:19]) and bit 24 marks an
 *     IRQ-owner channel (PMIC_ARB_CHAN_IS_IRQ_OWNER_MASK).
 *   - channel ownership sits at cnfg+0x700+4*n, owner = low 3 bits
 *     (SPMI_OWNERSHIP_PERIPH2OWNER).  Observer reads are allowed on any
 *     EE-owned channel; this driver only ever observer-reads.
 *   - OBS window: obsrvr + 0x10000*ee + 0x80*apid.  Status: bit0 done,
 *     bit1 transaction failed, bit2 denied, bit3 dropped.
 *
 * Battery side: the charger reports its charge state machine in
 * BATTERY_CHARGER_STATUS_1 (peripheral 0x15 register 0x06, bits[2:0];
 * qpnp-smbcharger/PM660-family table: 0 TRICKLE, 1 PRE, 2 FULL, 3 FAST,
 * 4 TERMINATE, 5 INHIBIT, 6 OVERVOLTAGE; fallback peripherals 0x46/0x10)
 * and battery presence in BATIF INT_RT_STS (peripheral 0x48 register
 * 0x10 on SID2, present = !(bit4|bit5); fallback: peripheral 0x12).
 * Reads only, no charger configuration is written.  The board exposes
 * several charger PMICs; enumerate() observer-reads each candidate's
 * status register and picks the one with a non-zero state - the
 * physically wired charger (SID10, apid 367/473 on the TW220) - instead
 * of blindly taking the last map entry.
 *
 * Capacity and voltage come from the fuel gauge (peripheral 0x40 on SID1
 * or SID3): SOC_MONOTONIC (0x09, 16-bit, LSB 0.00390625 %) and
 * VOLTAGE_NOW (0x0A, millivolts).  The FG algorithm runs autonomously in
 * the PMIC, so this driver is fully read-only: observer reads only, no
 * register writes and no ADC conversions.  The TW220 boots with the FG
 * dormant (SOC/VOLT read 0x0000), so capacity falls back to the charger:
 * FULL_CHARGE state reports 100%.
 *
 * Unplugged, every 0x15 candidate reads state 0 and no read-only
 * capacity source exists at all (register sweeps proved FG dormant,
 * BCL/ADC idle), so the daemon persists the last known truth in
 * /etc/powerd.state (persistent ext3 rootfs): enumerate() prefers the
 * remembered charger SID when no candidate is active and sample()
 * reports the remembered capacity instead of dropping to 0.  The file
 * rewrites only when a live source reports a changed value.
 * This driver is strictly READ-ONLY: it has no SPMI write path at all.
 * RW-window writes to the battery PMIC peripherals are PROVEN to wedge
 * the SPMI bus on the TW220: a write-back-same probe to FG@SID1 (apid 82)
 * and the earlier ADC5@SID8 write both froze the board (network/system
 * dead, power-cycle required), while goodixd's writes to GPIO@SID1 work -
 * the stall is specific to the FG/ADC blocks in this firmware state.
 * Real battery capacity is owned by the ADSP firmware battery manager
 * (PMIC GLINK battmgr, see battmgr_wire.h); it is not reachable from
 * these registers until the ADSP is booted.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <ewoksys/klog.h>
#include <ewoksys/syscall.h>

#include "power_pmu.h"

/* SPMI arbiter windows (addresses proven by goodixd on the TW220). */
#define SPMI_CORE_PHY      0x0c440000U
#define SPMI_CORE_VA       0x5a080000U
#define SPMI_CORE_SIZE     0x00004000U
#define SPMI_CFG_PHY       0x0c40a000U
#define SPMI_CFG_VA        0x5a0c0000U
#define SPMI_CFG_SIZE      0x00004000U
#define SPMI_OBS_PHY       0x0e600000U
#define SPMI_OBS_VA        0x5a100000U
#define SPMI_OBS_SIZE      0x00010000U
/* No RW window is mapped: this driver has no SPMI write path (writes to
 * the battery PMIC channels wedge the bus on the TW220). */

/* v5 APID map word: PPID = bits[8:19], IRQ-owner flag = bit 24. */
#define APID_PPID_SHIFT    8U
#define APID_PERIPH_MASK   0xffU
#define APID_SID_SHIFT     16U
#define APID_SID_MASK      0x0fU
#define APID_IRQ_OWNER     (1U << 24)
#define APID_MAP_SLOTS     512U
/* Channel owner: cnfg+0x700+4*n, low 3 bits. */
#define OWNER_REG(n)       (0x700U + 4U * (n))

/* Peripherals of interest.  The TW220 map (probed on device) shows:
 *   - fg@4000 (0x40) on SID1 (apid 82) and SID3 (apid 179) - fuel gauge,
 *     dormant at boot (SOC/VOLT read 0x0000);
 *   - charger@1500 (0x15) on SIDs 1/3/6/8/10/12 - smbcharger-family
 *     charger; only SID10 is live (apid 367 irq / 473 non-irq, status
 *     0x02 = FULL_CHARGE while the adapter holds the pack full);
 *   - batif@4800 (0x48) on SID2 (apid 124) - battery interface, reads OK;
 *   - SMB5-style CHGR 0x46 / BATIF 0x12 and legacy 0x10 as fallbacks;
 *   - adc@3100 (0x31) on SIDs 6/8/10/12/0 and chg@3600 (0x36) everywhere.
 * SID6 never answers on the bus (probe: every channel transaction-fails),
 * so anything on SID6 must be avoided. */
#define PERIPH_ADC5        0x31U
#define PERIPH_CHG_3600    0x36U
#define PERIPH_FG_4000     0x40U
#define PERIPH_SMB5_CHGR   0x46U
#define PERIPH_SMB5_BATIF  0x48U
#define PERIPH_CHGR_1500   0x15U
#define PERIPH_CHGR_1000   0x10U
#define PERIPH_BATIF_1200  0x12U

/* CHGR / BATIF registers (smb5-reg.h / qpnp-smbcharger). */
#define CHGR_STATUS1       0x06U  /* BATTERY_CHARGER_STATUS_1, bits[2:0] */
#define CHGR_STATE_MASK    0x07U
/* smbcharger-family charge state in CHGR_STATUS1 (see header comment).
 * The TW220's live charger reads 0x02 = FULL_CHARGE while the adapter
 * holds the pack full; the unwired charger PMICs read 0x00. */
#define CHGR_STATE_FULL    2U
#define BATIF_INT_RT_STS   0x10U
/* Battery present = !(BAT_THERM_OR_ID_MISSING | BAT_TERMINAL_MISSING). */
#define BATIF_MISSING_MASK 0x30U

/* Fuel gauge ALG registers (read-only, qpnp-fg-gen4.c).  SOC_MONOTONIC
 * is 16 bits with LSB 0.00390625 % (full = 25525); VOLTAGE_NOW is the
 * pack voltage in millivolts.  Both update autonomously while the PMIC
 * runs - no writes and no charger driver required. */
#define FG_SOC_MONOTONIC   0x09U
#define FG_VOLTAGE_NOW     0x0AU
#define FG_SOC_MAX         25525U
#define FG_VOLT_MIN_MV     2500U   /* plausibility: 1S cutoff .. 2S full */
#define FG_VOLT_MAX_MV     9800U

#define APID_ENTRY_MAX     512U
#define ADC_CANDIDATE_MAX  8U

typedef struct {
    uint32_t apid;
    uint32_t sid;
    uint32_t periph;
    uint32_t ee;
    bool irq_owner;
} apid_entry_t;

static volatile uint8_t* _core;
static volatile uint8_t* _cfg;
static volatile uint8_t* _obs;

static apid_entry_t _entries[APID_ENTRY_MAX];
static unsigned _entry_count;
static uint32_t _sid_mask;          /* bitmask of SIDs present (any EE) */
static uint32_t _sid_mask_ee0;      /* bitmask of SIDs with EE0 channels */
static unsigned _sid_ee0_count[16];
static unsigned _sid_other_count[16];
static uint8_t _sid_periphs[16][32];/* peripheral bitmap per SID */

static int _adc_apids[ADC_CANDIDATE_MAX];
static int _adc_sids[ADC_CANDIDATE_MAX];
static unsigned _adc_count;
static int _fg_apids[4];
static int _fg_sids[4];
static unsigned _fg_count;
static int _chgr_apid = -1, _chgr_sid = -1;    /* CHGR 0x15/0x46/0x10 */
static int _chgr1500_apids[8];                 /* all 0x15 candidates */
static int _chgr1500_sids[8];
static int _chgr1500_states[8];                /* status1, -1 = no read */
static unsigned _chgr1500_count;
static int _batif_apid = -1, _batif_sid = -1;  /* BATIF 0x48 or 0x12 */
static int _charger_sid = -1;       /* SID owning the charger/battery PMIC */
static bool _mapped;

/* Persisted daemon state (/etc/powerd.state, persistent ext3 rootfs):
 * the wired charger SID and the last known capacity.  Both survive an
 * unplugged reboot, where no live read-only source exists. */
#define POWERD_STATE_PATH   "/etc/powerd.state"
static int _stored_chgr_sid = -1;
static uint8_t _stored_cap;
static bool _stored_cap_valid;
static bool _state_dirty;

/* Diagnostics for --dump. */
static uint32_t _last_arb_status;   /* arbiter status of the last wait */
static bool _verbose;
static unsigned _map_slots;         /* map slots actually scanned */

void power_pmu_verbose(bool on) {
    _verbose = on;
}

#define VPRINTF(...) \
    do { \
        if (_verbose) \
            printf(__VA_ARGS__); \
    } while (0)

static inline uint32_t rd(volatile uint8_t* base, uint32_t off) {
    return *(volatile uint32_t*)(base + off);
}

static inline void wr(volatile uint8_t* base, uint32_t off, uint32_t value) {
    *(volatile uint32_t*)(base + off) = value;
}

static int map_at(uint64_t va, uint64_t pa, uint32_t size) {
    return syscall3(SYS_MEM_MAP, (ewokos_addr_t)va,
            (ewokos_addr_t)pa, (ewokos_addr_t)size) == (ewokos_addr_t)va
            ? 0 : -1;
}

/* SPMI v5 channels are always on.  Waits are bounded; error bits 1..3 of
 * the status word flag a failed, denied or dropped transaction (same
 * contract as goodixd).  The raw status is kept for --dump diagnostics. */
static int spmi_wait(volatile uint8_t* channel) {
    for (unsigned poll = 0; poll < 1000; poll++) {
        uint32_t status = rd(channel, 8);
        if (status & 1U) {
            _last_arb_status = status;
            return (status & 14U) ? -1 : 0;
        }
        usleep(10);
    }
    _last_arb_status = rd(channel, 8);
    return -1;
}

static int spmi_read(uint32_t apid, unsigned offset) {
    volatile uint8_t* obs = _obs + apid * 0x80U;

    /* Extended-read-long, one byte.  Observer reads need no ownership. */
    wr(obs, 0, (1U << 27) | (offset << 4));
    __asm__ volatile("dsb sy" ::: "memory");
    if (spmi_wait(obs) != 0)
        return -1;
    return (int)(rd(obs, 0x18) & 0xffU);
}

/* Two-byte extended-read-long (byte count 1); result is RDATA0[15:0],
 * low byte first, same as the kernel's pmic_arb_read_data. */
static int spmi_read2(uint32_t apid, unsigned offset, uint32_t* out) {
    volatile uint8_t* obs = _obs + apid * 0x80U;

    wr(obs, 0, (1U << 27) | (offset << 4) | 1U);
    __asm__ volatile("dsb sy" ::: "memory");
    if (spmi_wait(obs) != 0)
        return -1;
    *out = rd(obs, 0x18) & 0xffffU;
    return 0;
}

/* Strict decimal parser (libc strtoul is unavailable on this target). */
static int atou(const char* s, uint32_t* out) {
    uint32_t v = 0;

    if (s == NULL || *s == '\0')
        return -1;
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9')
            return -1;
        v = v * 10U + (uint32_t)(*s - '0');
    }
    *out = v;
    return 0;
}

/* Prefix-decimal parser for token streams: parses the leading digits of
 * s and stops at the first non-digit.  atou cannot be used here - it
 * rejects the trailing newline/space that ends each token. */
static int atou_prefix(const char* s, uint32_t* out) {
    uint32_t v = 0;
    bool any = false;

    while (*s >= '0' && *s <= '9') {
        any = true;
        v = v * 10U + (uint32_t)(*s - '0');
        s++;
    }
    if (!any)
        return -1;
    *out = v;
    return 0;
}

/* Load /etc/powerd.state ("chg_sid N" / "cap N" tokens) if present.
 * Missing or malformed files simply leave the remembered values at
 * their defaults - the daemon still starts and re-learns on the next
 * live observation. */
static void state_load(void) {
    char buf[128];
    FILE* f;
    size_t n;
    const char* p;

    f = fopen(POWERD_STATE_PATH, "r");
    if (f == NULL)
        return;
    n = fread(buf, 1U, sizeof(buf) - 1U, f);
    fclose(f);
    buf[n] = '\0';

    p = buf;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (strncmp(p, "chg_sid", 7U) == 0) {
            uint32_t v;

            p += 7;
            while (*p == ' ' || *p == '\t')
                p++;
            if (atou_prefix(p, &v) == 0 && v < 16U)
                _stored_chgr_sid = (int)v;
            while (*p >= '0' && *p <= '9')
                p++;
        } else if (strncmp(p, "cap", 3U) == 0) {
            uint32_t v;

            p += 3;
            while (*p == ' ' || *p == '\t')
                p++;
            if (atou_prefix(p, &v) == 0 && v <= 100U) {
                _stored_cap = (uint8_t)v;
                _stored_cap_valid = true;
            }
            while (*p >= '0' && *p <= '9')
                p++;
        } else {
            /* Unknown token: skip to the next whitespace. */
            while (*p != '\0' && *p != ' ' && *p != '\t' &&
                    *p != '\n' && *p != '\r')
                p++;
        }
    }
}

/* Rewrite /etc/powerd.state from the remembered values.  Called only
 * when _state_dirty is set - sampling runs once per second and the file
 * lives on eMMC, so writes happen on real changes only. */
static void state_save(void) {
    char buf[64];
    size_t len = 0;
    int w;
    FILE* f;

    if (_stored_chgr_sid >= 0) {
        w = snprintf(buf + len, sizeof(buf) - len, "chg_sid %d\n",
                _stored_chgr_sid);
        if (w < 0 || (size_t)w >= sizeof(buf) - len)
            return;
        len += (size_t)w;
    }
    if (_stored_cap_valid) {
        w = snprintf(buf + len, sizeof(buf) - len, "cap %u\n",
                (unsigned)_stored_cap);
        if (w < 0 || (size_t)w >= sizeof(buf) - len)
            return;
        len += (size_t)w;
    }
    if (len == 0U)
        return;
    f = fopen(POWERD_STATE_PATH, "w");
    if (f == NULL)
        return;
    if (fwrite(buf, 1U, len, f) == len)
        _state_dirty = false;
    fclose(f);
}

static const char* arb_err_name(uint32_t status) {
    if (status == 0)
        return "timeout";
    if (status & 2U)
        return "failed";
    if (status & 4U)
        return "denied";
    if (status & 8U)
        return "dropped";
    return "ok";
}

/* Walk the APID map once: record every channel with its owner and build
 * the per-SID peripheral bitmap, then locate what this driver needs.
 * The scan runs over all 512 slots; the FEATURES register (core+4) is
 * unstable on the TW220 (read 499 once, another value on a later boot),
 * so it is only used as a display hint.  Reading all 512 slots has
 * proven safe on this board. */
static int enumerate(void) {
    uint32_t version = rd(_core, 0);
    uint32_t features = rd(_core, 4) & 0x7ffU;
    int smb5_chgr_apid = -1, smb5_chgr_sid = -1;
    int chgr1000_apid = -1, chgr1000_sid = -1;
    int smb5_batif_apid = -1, smb5_batif_sid = -1;
    int batif1200_apid = -1, batif1200_sid = -1;

    if (version < 0x50000000U || version >= 0x70000000U)
        return -1;
    _map_slots = features != 0 && features <= APID_MAP_SLOTS ?
            features : APID_MAP_SLOTS;

    _entry_count = 0;
    _sid_mask = 0;
    _sid_mask_ee0 = 0;
    memset(_sid_ee0_count, 0, sizeof(_sid_ee0_count));
    memset(_sid_other_count, 0, sizeof(_sid_other_count));
    memset(_sid_periphs, 0, sizeof(_sid_periphs));
    _adc_count = 0;
    _fg_count = 0;
    _chgr1500_count = 0;

    for (uint32_t i = 0; i < _map_slots; i++) {
        uint32_t map = rd(_core, 0x900U + 4U * i);
        if (map == 0)
            continue;
        uint32_t periph = (map >> APID_PPID_SHIFT) & APID_PERIPH_MASK;
        uint32_t sid = (map >> APID_SID_SHIFT) & APID_SID_MASK;
        uint32_t ee = rd(_cfg, OWNER_REG(i)) & 7U;

        _sid_mask |= 1U << sid;
        _sid_periphs[sid][periph >> 3] |=
                (uint8_t)(1U << (periph & 7U));
        if (ee == 0) {
            _sid_mask_ee0 |= 1U << sid;
            _sid_ee0_count[sid]++;
        } else {
            _sid_other_count[sid]++;
        }
        if (_entry_count < APID_ENTRY_MAX) {
            _entries[_entry_count].apid = i;
            _entries[_entry_count].sid = sid;
            _entries[_entry_count].periph = periph;
            _entries[_entry_count].ee = ee;
            _entries[_entry_count].irq_owner =
                    (map & APID_IRQ_OWNER) != 0;
            _entry_count++;
        }
        /* ADC candidates: EE0-owned channels, recorded for the read-only
         * --dump discovery listing only (no conversion path exists). */
        if (periph == PERIPH_ADC5 && ee == 0 &&
                _adc_count < ADC_CANDIDATE_MAX) {
            _adc_apids[_adc_count] = (int)i;
            _adc_sids[_adc_count] = (int)sid;
            _adc_count++;
        }
        if (ee == 0 && periph == PERIPH_CHGR_1500 &&
                _chgr1500_count < 8U) {
            _chgr1500_apids[_chgr1500_count] = (int)i;
            _chgr1500_sids[_chgr1500_count] = (int)sid;
            _chgr1500_count++;
        }
        if (ee == 0 && periph == PERIPH_SMB5_CHGR) {
            smb5_chgr_apid = (int)i;
            smb5_chgr_sid = (int)sid;
        }
        if (ee == 0 && periph == PERIPH_CHGR_1000) {
            chgr1000_apid = (int)i;
            chgr1000_sid = (int)sid;
        }
        if (ee == 0 && periph == PERIPH_SMB5_BATIF) {
            smb5_batif_apid = (int)i;
            smb5_batif_sid = (int)sid;
        }
        if (ee == 0 && periph == PERIPH_BATIF_1200) {
            batif1200_apid = (int)i;
            batif1200_sid = (int)sid;
        }
        if (ee == 0 && periph == PERIPH_FG_4000 && _fg_count < 4) {
            _fg_apids[_fg_count] = (int)i;
            _fg_sids[_fg_count] = (int)sid;
            _fg_count++;
        }
    }
    /* Several charger PMICs are on the bus, but only one is wired to the
     * battery.  Observer-read each 0x15 candidate's status register and
     * keep the first with a non-zero charge state (the live charger:
     * SID10, apid 367/473 on the TW220); that SID is remembered in the
     * state file. */
    _chgr_apid = -1;
    _chgr_sid = -1;
    for (unsigned i = 0; i < _chgr1500_count; i++) {
        int st = spmi_read((uint32_t)_chgr1500_apids[i], CHGR_STATUS1);

        _chgr1500_states[i] = st >= 0 ? st & (int)CHGR_STATE_MASK : -1;
        if (st >= 0 && (st & CHGR_STATE_MASK) != 0U) {
            _chgr_apid = _chgr1500_apids[i];
            _chgr_sid = _chgr1500_sids[i];
            if (_chgr_sid != _stored_chgr_sid) {
                _stored_chgr_sid = _chgr_sid;
                _state_dirty = true;
            }
            break;
        }
    }
    /* Unplugged, every candidate reads state 0, so nothing distinguishes
     * the wired charger.  Prefer the SID remembered in the state file
     * (persisted whenever a live charge state was observed) over the
     * map-order fallback, which is boot-dependent on the TW220: the
     * FEATURES register is unstable and can truncate the map scan. */
    if (_chgr_apid < 0 && _stored_chgr_sid >= 0) {
        for (unsigned i = 0; i < _chgr1500_count; i++) {
            if (_chgr1500_sids[i] == _stored_chgr_sid) {
                _chgr_apid = _chgr1500_apids[i];
                _chgr_sid = _chgr1500_sids[i];
                break;
            }
        }
    }
    if (_chgr_apid < 0 && _chgr1500_count > 0U) {
        _chgr_apid = _chgr1500_apids[_chgr1500_count - 1];
        _chgr_sid = _chgr1500_sids[_chgr1500_count - 1];
    }
    if (_chgr_apid < 0) {
        _chgr_apid = smb5_chgr_apid >= 0 ? smb5_chgr_apid : chgr1000_apid;
        _chgr_sid = smb5_chgr_apid >= 0 ? smb5_chgr_sid : chgr1000_sid;
    }
    _batif_apid = smb5_batif_apid >= 0 ? smb5_batif_apid : batif1200_apid;
    _batif_sid = smb5_batif_apid >= 0 ? smb5_batif_sid : batif1200_sid;
    /* The charger SID hosts the battery interface (CHGR/BATIF). */
    _charger_sid = _chgr_sid >= 0 ? _chgr_sid : _batif_sid;
    return 0;
}

int power_pmu_init(void) {
    if (_mapped)
        return 0;
    state_load();
    VPRINTF("powerd: mapping core window\n");
    if (map_at(SPMI_CORE_VA, SPMI_CORE_PHY, SPMI_CORE_SIZE) != 0 ||
            map_at(SPMI_CFG_VA, SPMI_CFG_PHY, SPMI_CFG_SIZE) != 0 ||
            map_at(SPMI_OBS_VA, SPMI_OBS_PHY, SPMI_OBS_SIZE) != 0) {
        slog("q8b powerd: SPMI window mapping failed\n");
        VPRINTF("powerd: SPMI window mapping failed\n");
        return -1;
    }
    _core = (volatile uint8_t*)(uintptr_t)SPMI_CORE_VA;
    _cfg = (volatile uint8_t*)(uintptr_t)SPMI_CFG_VA;
    _obs = (volatile uint8_t*)(uintptr_t)SPMI_OBS_VA;
    VPRINTF("powerd: reading APID map\n");
    if (enumerate() != 0) {
        slog("q8b powerd: SPMI arbiter version check failed\n");
        VPRINTF("powerd: SPMI arbiter version check failed\n");
        return -1;
    }
    /* A live charge state observed during enumeration updates the
     * remembered charger SID; persist it right away so diagnostic modes
     * (--dump/--sample) leave the state file current too. */
    if (_state_dirty)
        state_save();
    VPRINTF("powerd: map done, %u channels in %u slots\n",
            _entry_count, _map_slots);
    _mapped = true;
    slog("q8b powerd: SPMI discovery chgr=%s batif=%s adc=%u "
            "charger_sid=%d sids=%03x\n",
            _chgr_apid >= 0 ? "found" : "none",
            _batif_apid >= 0 ? "found" : "none",
            _adc_count, _charger_sid, (unsigned)_sid_mask);
    return 0;
}

/* Bootstrap /etc/powerd.state without any SPMI access.  Run once at
 * deployment ("powerd --seed <chg_sid> <cap>"); the daemon picks the
 * values up on its next start. */
int power_pmu_seed(int sid, int cap) {
    if (sid < 0 || sid > 15 || cap < 0 || cap > 100)
        return -1;
    _stored_chgr_sid = sid;
    _stored_cap = (uint8_t)cap;
    _stored_cap_valid = true;
    _state_dirty = true;
    state_save();
    return _state_dirty ? -1 : 0;
}

/* "powerd --seed <chg_sid> <cap>": strict-decimal argv form. */
int power_pmu_seed_args(int n, const char* const* args) {
    uint32_t sid, cap;

    if (n != 2 || atou(args[0], &sid) != 0 || atou(args[1], &cap) != 0) {
        printf("seed: usage: powerd --seed <chg_sid> <cap> "
                "(decimal)\n");
        return -1;
    }
    if (sid > 15U || cap > 100U) {
        printf("seed: bad values chg_sid=%u cap=%u\n", sid, cap);
        return -1;
    }
    if (power_pmu_seed((int)sid, (int)cap) != 0) {
        printf("seed: state write failed\n");
        return -1;
    }
    printf("seed: wrote chg_sid=%u cap=%u%% to %s\n",
            sid, cap, POWERD_STATE_PATH);
    return 0;
}

/* Fully read-only sample: charger status + BATIF presence are single
 * observer reads, capacity and voltage come from the fuel gauge's ALG
 * registers.  The driver has no SPMI write path at all - writes to the
 * battery PMIC channels froze the board during bring-up (see README). */
int power_pmu_sample(power_sample_t* out) {
    memset(out, 0, sizeof(*out));
    out->charger_sid = -1;
    out->vbat_sid = -1;
    out->voltage_mv = -1;
    if (!_mapped)
        return -1;

    out->charger_sid = _charger_sid;

    if (_chgr_apid >= 0) {
        int st = spmi_read((uint32_t)_chgr_apid, CHGR_STATUS1);
        if (st >= 0) {
            out->chgr_state = (uint32_t)st & CHGR_STATE_MASK;
            /* smbcharger table: PRE/FULL/FAST mean current is flowing
             * in; the other states are idle/terminal/error. */
            out->charging = out->chgr_state >= 1U && out->chgr_state <= 3U;
            /* FULL_CHARGE: the charger holds the pack at full - the only
             * read-only capacity source while the FG is dormant. */
            if (out->chgr_state == CHGR_STATE_FULL) {
                out->capacity = 100U;
                out->cap_src = CAP_SRC_CHARGER;
            }
        }
    }
    if (_batif_apid >= 0) {
        int sts = spmi_read((uint32_t)_batif_apid, BATIF_INT_RT_STS);
        if (sts >= 0) {
            out->batif_sts = (uint32_t)sts;
            out->battery = ((uint32_t)sts & BATIF_MISSING_MASK) == 0;
        }
    }
    for (unsigned i = 0; i < _fg_count; i++) {
        uint32_t soc, volt;

        if (spmi_read2((uint32_t)_fg_apids[i], FG_SOC_MONOTONIC, &soc) != 0 ||
                soc == 0 || soc > FG_SOC_MAX)
            continue;
        if (spmi_read2((uint32_t)_fg_apids[i], FG_VOLTAGE_NOW, &volt) != 0)
            continue;
        if (volt < FG_VOLT_MIN_MV || volt > FG_VOLT_MAX_MV)
            continue;
        /* SOC_MONOTONIC: LSB 0.00390625 %, full = 25525. */
        out->capacity = (uint8_t)(soc * 100U / FG_SOC_MAX);
        if (out->capacity > 100U)
            out->capacity = 100U;
        out->cap_src = CAP_SRC_FG;
        out->voltage_mv = (int)volt;
        out->vbat_code = (uint32_t)soc;
        out->vbat_sid = _fg_sids[i];
        out->vbat_valid = true;
        break;
    }
    /* Persist live truth when it changed - sampling runs once per
     * second and the state file lives on eMMC, so only real changes
     * rewrite it. */
    if ((out->cap_src == CAP_SRC_CHARGER || out->cap_src == CAP_SRC_FG) &&
            (!_stored_cap_valid || _stored_cap != out->capacity)) {
        _stored_cap = out->capacity;
        _stored_cap_valid = true;
        _state_dirty = true;
    }
    /* Charger idle + FG dormant: no live capacity source exists on the
     * TW220 (proven by register sweeps), so report the last known
     * capacity instead of dropping to 0. */
    if (out->capacity == 0U && _stored_cap_valid) {
        out->capacity = _stored_cap;
        out->cap_src = CAP_SRC_REMEMBERED;
    }
    if (_state_dirty)
        state_save();
    return 0;
}

/* Stage 1: map summary - pure core-register reads and in-memory state,
 * proven safe on the TW220. */
int power_pmu_dump_map(void) {
    if (!_mapped) {
        printf("powerd: SPMI not mapped\n");
        return -1;
    }
    printf("SPMI version=%08x apids=%u map_slots=%u "
            "sids(all)=%03x sids(ee0)=%03x\n",
            (unsigned)rd(_core, 0), _entry_count, _map_slots,
            (unsigned)_sid_mask, (unsigned)_sid_mask_ee0);
    printf("discovery: chgr(0x15/0x46/0x10) apid=%d sid=%d | "
            "batif(0x48/0x12) apid=%d sid=%d | fg apids:",
            _chgr_apid, _chgr_sid, _batif_apid, _batif_sid);
    for (unsigned i = 0; i < _fg_count; i++)
        printf(" %d(sid%d)", _fg_apids[i], _fg_sids[i]);
    printf(" | adc apids:");
    for (unsigned i = 0; i < _adc_count; i++)
        printf(" %d(sid%d)", _adc_apids[i], _adc_sids[i]);
    printf("\n");
    printf("chgr1500 candidates:");
    for (unsigned i = 0; i < _chgr1500_count; i++)
        printf(" %d(sid%d,st=%d)", _chgr1500_apids[i],
                _chgr1500_sids[i], _chgr1500_states[i]);
    printf("\n");
    if (_stored_chgr_sid >= 0 || _stored_cap_valid) {
        printf("state: chg_sid=%d cap=%u%%%s\n",
                _stored_chgr_sid, (unsigned)_stored_cap,
                _stored_cap_valid ? "" : " (no cap)");
    } else {
        printf("state: none\n");
    }

    for (uint32_t sid = 0; sid < 16; sid++) {
        if (!(_sid_mask & (1U << sid)))
            continue;
        printf("  sid=%u: ee0=%u other=%u periphs:",
                (unsigned)sid, _sid_ee0_count[sid], _sid_other_count[sid]);
        for (unsigned g = 0; g < 8; g++)
            printf(" %02x%02x%02x%02x",
                    _sid_periphs[sid][g * 4 + 0],
                    _sid_periphs[sid][g * 4 + 1],
                    _sid_periphs[sid][g * 4 + 2],
                    _sid_periphs[sid][g * 4 + 3]);
        printf("\n");
    }
    for (unsigned i = 0; i < _entry_count; i++) {
        apid_entry_t* e = &_entries[i];
        bool interesting =
                e->periph == PERIPH_ADC5 ||
                e->periph == PERIPH_CHG_3600 ||
                e->periph == PERIPH_FG_4000 ||
                e->periph == PERIPH_SMB5_CHGR ||
                e->periph == PERIPH_SMB5_BATIF ||
                e->periph == PERIPH_CHGR_1000 ||
                e->periph == PERIPH_BATIF_1200;
        if (!interesting)
            continue;
        printf("  periph=0x%02x apid=%u sid=%u ee=%u%s\n",
                (unsigned)e->periph, e->apid, e->sid, e->ee,
                e->irq_owner ? " irq" : "");
    }
    return 0;
}

/* Stage 2: one observer read per representative channel.  Every line is
 * printed before the next read so a bus-stalling channel is the one with
 * no result line after it. */
int power_pmu_dump_probe(void) {
    bool sid_seen[16] = { false };

    if (!_mapped) {
        printf("powerd: SPMI not mapped\n");
        return -1;
    }
    for (unsigned i = 0; i < _entry_count; i++) {
        apid_entry_t* e = &_entries[i];
        bool interesting =
                e->periph == PERIPH_ADC5 ||
                e->periph == PERIPH_CHG_3600 ||
                e->periph == PERIPH_FG_4000 ||
                e->periph == PERIPH_SMB5_CHGR ||
                e->periph == PERIPH_SMB5_BATIF ||
                e->periph == PERIPH_CHGR_1000 ||
                e->periph == PERIPH_BATIF_1200;
        if (!interesting && sid_seen[e->sid])
            continue;
        sid_seen[e->sid] = true;
        printf("probe apid=%u sid=%u periph=0x%02x ee=%u%s: reading\n",
                e->apid, e->sid, e->periph, e->ee,
                e->irq_owner ? " irq" : "");
        _last_arb_status = 0;
        {
            int v = spmi_read(e->apid, 0);
            printf("probe apid=%u: val=%02x st=%08x %s\n",
                    e->apid, v < 0 ? 0 : (unsigned)v & 0xff,
                    (unsigned)_last_arb_status,
                    arb_err_name(_last_arb_status));
        }
    }
    return 0;
}

/* Stage 3: one normal sample through the daemon path. */
int power_pmu_dump_sample(void) {
    power_sample_t s;

    if (!_mapped) {
        printf("powerd: SPMI not mapped\n");
        return -1;
    }
    printf("sample: reading\n");
    if (power_pmu_sample(&s) == 0) {
        printf("sample: battery=%d charging=%d capacity=%u%% src=%u "
                "voltage=%dmV vbat_code=0x%04x chgr_state=%u batif=%02x\n",
                s.battery ? 1 : 0, s.charging ? 1 : 0, (unsigned)s.capacity,
                (unsigned)s.cap_src, s.voltage_mv, (unsigned)s.vbat_code,
                (unsigned)s.chgr_state, (unsigned)s.batif_sts);
    } else {
        printf("sample: pmu not mapped\n");
    }
    return 0;
}

/* Full APID map listing (pure in-memory state, no hardware access). */
int power_pmu_dump_mapall(void) {
    if (!_mapped) {
        printf("powerd: SPMI not mapped\n");
        return -1;
    }
    for (unsigned i = 0; i < _entry_count; i++) {
        apid_entry_t* e = &_entries[i];
        printf("apid=%u sid=%u periph=0x%02x ee=%u%s\n",
                e->apid, e->sid, e->periph, e->ee,
                e->irq_owner ? " irq" : "");
    }
    return 0;
}

/* Raw observer reads: "powerd --read <apid> <reg> <len> ..." (decimal,
 * len 1 or 2, any number of triples).  Read-only and streamed - the
 * same safety properties as --probe. */
int power_pmu_read(int n, const char* const* args) {
    if (!_mapped) {
        printf("powerd: SPMI not mapped\n");
        return -1;
    }
    for (int i = 0; i + 3 <= n; i += 3) {
        uint32_t apid, reg, len;

        if (atou(args[i], &apid) != 0 || atou(args[i + 1], &reg) != 0 ||
                atou(args[i + 2], &len) != 0 || apid > 511U ||
                reg > 255U || (len != 1U && len != 2U)) {
            printf("read: bad triple '%s %s %s'\n",
                    args[i], args[i + 1], args[i + 2]);
            continue;
        }
        if (len == 1U) {
            int v;

            printf("read apid=%u reg=0x%02x len=1: reading\n",
                    apid, reg);
            v = spmi_read(apid, reg);
            printf("read apid=%u reg=0x%02x: val=%02x st=%08x %s\n",
                    apid, reg, v < 0 ? 0 : (unsigned)v & 0xff,
                    (unsigned)_last_arb_status,
                    arb_err_name(_last_arb_status));
        } else {
            uint32_t v = 0;

            printf("read apid=%u reg=0x%02x len=2: reading\n",
                    apid, reg);
            if (spmi_read2(apid, reg, &v) == 0)
                printf("read apid=%u reg=0x%02x: val=%04x (%u) "
                        "st=%08x %s\n",
                        apid, reg, (unsigned)v, (unsigned)v,
                        (unsigned)_last_arb_status,
                        arb_err_name(_last_arb_status));
            else
                printf("read apid=%u reg=0x%02x: val=-- st=%08x %s\n",
                        apid, reg, (unsigned)_last_arb_status,
                        arb_err_name(_last_arb_status));
        }
    }
    return 0;
}
