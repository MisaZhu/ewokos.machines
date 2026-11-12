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
 *     EE-owned channel; only register writes need EE0 ownership.
 *   - OBS window: obsrvr + 0x10000*ee + 0x80*apid; RW window:
 *     0x10000*apid.  Status: bit0 done, bit1 transaction failed,
 *     bit2 denied, bit3 dropped.
 *
 * Battery side: the PM8350B-class charger reports its charge state machine
 * in BATTERY_CHARGER_STATUS_1 (peripheral 0x15 register 0x06, bits[2:0];
 * standalone-SMB5 layout: peripheral 0x46; legacy fallback 0x10) and
 * battery presence in BATIF INT_RT_STS (peripheral 0x48 register 0x10 on
 * SID2, present = !(bit4|bit5); fallback: peripheral 0x12).  Reads only,
 * no charger configuration is written.
 *
 * Capacity and voltage come from the fuel gauge (peripheral 0x40 on SID1
 * or SID3): SOC_MONOTONIC (0x09, 16-bit, LSB 0.00390625 %) and
 * VOLTAGE_NOW (0x0A, millivolts).  The FG algorithm runs autonomously in
 * the PMIC, so this driver is fully read-only: observer reads only, no
 * register writes and no ADC conversions.  RW-window writes to the
 * battery PMIC peripherals are PROVEN to wedge the SPMI bus on the TW220:
 * a write-back-same probe to FG@SID1 (apid 82) and the earlier ADC5@SID8
 * write both froze the board (network/system dead, reboot required),
 * while goodixd's writes to GPIO@SID1 work - so the stall is specific to
 * the FG/ADC blocks in this firmware state.  Never issue RW-window
 * writes to battery PMIC channels (see powerd README).
 *
 * Register map references follow the downstream qcom drivers
 * (qpnp-smb5-reg.h, qpnp-fg-gen4.c); ADC5 conversion code is retained
 * for the opt-in --adc diagnostic only, which must not be run.
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
/* One RW window per APID: physical base + apid * 64K, 16K each.  A handful
 * of slots is enough - writes only ever target the ADC5 peripheral. */
#define SPMI_RW_PHY_BASE   0x0c600000U
#define SPMI_RW_VA_BASE    0x5a180000U
#define SPMI_RW_STRIDE     0x00010000U
#define SPMI_RW_SIZE       0x00004000U
#define SPMI_RW_SLOTS      8U

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
 *   - fg@4000 (0x40) on SID1 (apid 82) and SID3 (apid 179) - fuel gauge;
 *   - charger@1500 (0x15) on SID1 and SID3 - PM8350B-class charger;
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

/* SMB5 CHGR / BATIF registers (smb5-reg.h). */
#define CHGR_STATUS1       0x06U  /* BATTERY_CHARGER_STATUS_1, bits[2:0] */
#define CHGR_STATE_MASK    0x07U
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

/* ADC5 (PMIC7 VADC) registers (qcom-spmi-adc5.c). */
#define ADC_STATUS1        0x08U
#define ADC_STATUS1_EOC    0x01U
#define ADC_STATUS1_MASK   0x03U
#define ADC_STATUS1_FAULT  0x80U
#define ADC_APP_SID        0x40U
#define ADC_DIG_PARAM      0x42U
#define ADC_FAST_AVG       0x43U
#define ADC_CH_SEL         0x44U
#define ADC_DELAY_CTL      0x45U
#define ADC_CONV_REQ       0x47U
#define ADC_DATA0          0x50U
#define ADC_DATA1          0x51U

/* VBAT_SNS conversion setup, mirroring the downstream pm8350b DT:
 * cal-type absolute (2), decimation 340 (index 1), avg-samples 1,
 * hw-settle 200us. */
#define ADC_DIG_CAL_SHIFT  4U
#define ADC_DIG_DEC_SHIFT  2U
#define ADC_DIG_ABS_CAL    (2U << ADC_DIG_CAL_SHIFT)
#define ADC_DIG_DEC_340    (1U << ADC_DIG_DEC_SHIFT)
#define ADC_DIG_MASK       0x3cU
#define ADC_FAST_AVG_SAMPLES 0x01U
#define ADC_FAST_AVG_MASK  0x07U
#define ADC_CH_VBAT_SNS    0x8fU
#define ADC_DELAY_200US    0x02U
#define ADC_DELAY_MASK     0x0fU
#define ADC_CONV_REQ_GO    0x80U

#define ADC_DATA_INVALID   0x8000U
#define ADC_FULL_SCALE_CODE 28900U  /* 0x70e4, adc5_data_pmic */
#define ADC_VREF_UV        1875000U
#define VBAT_PRESCALE_DEN  6U       /* VBAT_SNS pre-scaling 1/6 */

/* EOC poll budget: decimation-340 conversions finish in well under 100ms. */
#define ADC_POLL_MS        100U

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
static struct {
    uint32_t apid;
    volatile uint8_t* va;
} _rw_slots[SPMI_RW_SLOTS];
static unsigned _rw_slot_count;

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
static int _batif_apid = -1, _batif_sid = -1;  /* BATIF 0x48 or 0x12 */
static int _charger_sid = -1;       /* SID owning the charger/battery PMIC */
static bool _mapped;

/* Diagnostics for --dump. */
static uint32_t _last_arb_status;   /* arbiter status of the last wait */
static const char* _adc_stage = "idle";
static int _adc_status1 = -1;
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

static volatile uint8_t* rw_window(uint32_t apid) {
    for (unsigned i = 0; i < _rw_slot_count; i++) {
        if (_rw_slots[i].apid == apid)
            return _rw_slots[i].va;
    }
    if (_rw_slot_count == SPMI_RW_SLOTS)
        return NULL;
    uint64_t va = SPMI_RW_VA_BASE +
            (uint64_t)_rw_slot_count * SPMI_RW_STRIDE;
    uint64_t pa = SPMI_RW_PHY_BASE + (uint64_t)apid * SPMI_RW_STRIDE;
    if (map_at(va, pa, SPMI_RW_SIZE) != 0)
        return NULL;
    _rw_slots[_rw_slot_count].apid = apid;
    _rw_slots[_rw_slot_count].va = (volatile uint8_t*)(uintptr_t)va;
    return _rw_slots[_rw_slot_count++].va;
}

/* Register writes require EE0 ownership of the channel.  The kernel's
 * pmic_arb_offset_v5 refuses with -EPERM before touching hardware when
 * the channel's write owner is another EE, so mirror that gate here
 * instead of relying on the arbiter's undefined behavior. */
static int spmi_write(uint32_t apid, unsigned offset, unsigned value) {
    volatile uint8_t* rw;

    if (apid >= APID_MAP_SLOTS ||
            (rd(_cfg, OWNER_REG(apid)) & 7U) != 0)
        return -1;
    rw = rw_window(apid);
    if (rw == NULL)
        return -1;
    /* Latch data first, then issue the command (goodixd order). */
    wr(rw, 0x10, value);
    __asm__ volatile("dsb sy" ::: "memory");
    wr(rw, 0, offset << 4);
    __asm__ volatile("dsb sy" ::: "memory");
    return spmi_wait(rw);
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
    int chgr1500_apid = -1, chgr1500_sid = -1;
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
        /* ADC candidates: EE0-owned only (conversions need writes). */
        if (periph == PERIPH_ADC5 && ee == 0 &&
                _adc_count < ADC_CANDIDATE_MAX) {
            _adc_apids[_adc_count] = (int)i;
            _adc_sids[_adc_count] = (int)sid;
            _adc_count++;
        }
        if (ee == 0 && periph == PERIPH_CHGR_1500) {
            chgr1500_apid = (int)i;
            chgr1500_sid = (int)sid;
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
    _chgr_apid = chgr1500_apid >= 0 ? chgr1500_apid :
            (smb5_chgr_apid >= 0 ? smb5_chgr_apid : chgr1000_apid);
    _chgr_sid = chgr1500_apid >= 0 ? chgr1500_sid :
            (smb5_chgr_apid >= 0 ? smb5_chgr_sid : chgr1000_sid);
    _batif_apid = smb5_batif_apid >= 0 ? smb5_batif_apid : batif1200_apid;
    _batif_sid = smb5_batif_apid >= 0 ? smb5_batif_sid : batif1200_sid;
    /* The charger SID hosts the battery interface (CHGR/BATIF). */
    _charger_sid = _chgr_sid >= 0 ? _chgr_sid : _batif_sid;
    return 0;
}

/* One VBAT_SNS conversion on the given ADC channel and APP_SID.  Read-
 * modify-writes the config registers like the downstream driver so fields
 * owned by whoever else may use the ADC are preserved, and restores
 * APP_SID afterwards.  Progress is recorded for --dump. */
static int adc7_convert(uint32_t apid, uint32_t sid, uint32_t* code_out) {
    int old_sid;
    uint8_t cfg[4];
    uint32_t code = ADC_DATA_INVALID;

    _adc_status1 = -1;
    _adc_stage = "read-app-sid";
    old_sid = spmi_read(apid, ADC_APP_SID);
    _adc_stage = "read-cfg";
    for (unsigned i = 0; i < 4; i++) {
        int v = spmi_read(apid, ADC_DIG_PARAM + i);
        if (v < 0)
            return -1;
        cfg[i] = (uint8_t)v;
    }
    cfg[0] = (uint8_t)((cfg[0] & ~ADC_DIG_MASK) |
            ADC_DIG_ABS_CAL | ADC_DIG_DEC_340);
    cfg[1] = (uint8_t)((cfg[1] & ~ADC_FAST_AVG_MASK) | ADC_FAST_AVG_SAMPLES);
    cfg[2] = ADC_CH_VBAT_SNS;
    cfg[3] = (uint8_t)((cfg[3] & ~ADC_DELAY_MASK) | ADC_DELAY_200US);

    _adc_stage = "write-app-sid";
    if (spmi_write(apid, ADC_APP_SID, sid) != 0)
        goto out;
    _adc_stage = "write-cfg";
    for (unsigned i = 0; i < 4; i++) {
        if (spmi_write(apid, ADC_DIG_PARAM + i, cfg[i]) != 0)
            goto out;
    }
    _adc_stage = "conv-req";
    if (spmi_write(apid, ADC_CONV_REQ, ADC_CONV_REQ_GO) != 0)
        goto out;

    _adc_stage = "poll-eoc";
    for (unsigned poll = 0; poll < ADC_POLL_MS; poll++) {
        int status = spmi_read(apid, ADC_STATUS1);
        _adc_status1 = status;
        if (status < 0 || (status & ADC_STATUS1_FAULT))
            break;
        if ((status & ADC_STATUS1_MASK) == ADC_STATUS1_EOC) {
            _adc_stage = "read-data";
            int lo = spmi_read(apid, ADC_DATA0);
            int hi = spmi_read(apid, ADC_DATA1);
            if (lo >= 0 && hi >= 0)
                code = (uint32_t)lo | ((uint32_t)hi << 8);
            break;
        }
        usleep(1000);
    }
out:
    _adc_stage = "restore-app-sid";
    if (old_sid >= 0)
        (void)spmi_write(apid, ADC_APP_SID, (uint32_t)old_sid);
    if (code >= ADC_DATA_INVALID || code == 0 ||
            code > ADC_FULL_SCALE_CODE) {
        if (code != ADC_DATA_INVALID)
            _adc_stage = "bad-code";
        return -1;
    }
    _adc_stage = "ok";
    *code_out = code;
    return 0;
}

static uint64_t code2uv(uint32_t code) {
    return (uint64_t)code * ADC_VREF_UV * VBAT_PRESCALE_DEN /
            ADC_FULL_SCALE_CODE;
}

int power_pmu_init(void) {
    if (_mapped)
        return 0;
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
    _rw_slot_count = 0;
    VPRINTF("powerd: reading APID map\n");
    if (enumerate() != 0) {
        slog("q8b powerd: SPMI arbiter version check failed\n");
        VPRINTF("powerd: SPMI arbiter version check failed\n");
        return -1;
    }
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

/* Fully read-only sample: charger status + BATIF presence are single
 * observer reads, capacity and voltage come from the fuel gauge's ALG
 * registers.  No RW-window writes, no ADC conversions (the write path
 * froze the board during bring-up - see README). */
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
            /* TRICKLE/PRE/FULLON/TAPER mean current is flowing in. */
            out->charging = out->chgr_state >= 1U && out->chgr_state <= 4U;
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
        out->voltage_mv = (int)volt;
        out->vbat_code = (uint32_t)soc;
        out->vbat_sid = _fg_sids[i];
        out->vbat_valid = true;
        break;
    }
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

/* Stage 3: raw VBAT_SNS conversion attempts per ADC channel (includes
 * RW-window register writes).  Each attempt is announced before it runs;
 * a bus-stalling access shows up as the last announcement line. */
int power_pmu_dump_adc(void) {
    uint32_t charger_sid_u;

    if (!_mapped) {
        printf("powerd: SPMI not mapped\n");
        return -1;
    }
    if (_adc_count == 0) {
        printf("adc: no EE0-owned ADC channels\n");
        return 0;
    }
    charger_sid_u = _charger_sid >= 0 ? (uint32_t)_charger_sid : 0U;
    for (unsigned i = 0; i < _adc_count; i++) {
        uint32_t apid = (uint32_t)_adc_apids[i];
        uint32_t sids_try[3];
        unsigned sids_try_n = 0;

        sids_try[sids_try_n++] = charger_sid_u;
        if ((uint32_t)_adc_sids[i] != charger_sid_u)
            sids_try[sids_try_n++] = (uint32_t)_adc_sids[i];
        if (0U != charger_sid_u && 0U != (uint32_t)_adc_sids[i])
            sids_try[sids_try_n++] = 0U;
        for (unsigned s = 0; s < sids_try_n; s++) {
            uint32_t code;
            int rc;

            printf("adc apid=%u sid=%u app_sid=%u: converting\n",
                    apid, (unsigned)_adc_sids[i],
                    (unsigned)sids_try[s]);
            rc = adc7_convert(apid, sids_try[s], &code);
            printf("adc apid=%u app_sid=%u: rc=%d stage=%s "
                    "status1=%s%02x",
                    apid, (unsigned)sids_try[s], rc, _adc_stage,
                    _adc_status1 >= 0 ? "" : "?",
                    _adc_status1 >= 0 ?
                    (unsigned)_adc_status1 : 0);
            if (rc == 0)
                printf(" code=0x%04x uv=%u\n", (unsigned)code,
                        (unsigned)code2uv(code));
            else
                printf("\n");
            if (rc == 0)
                break;
        }
    }
    return 0;
}

/* Stage 4: one normal sample through the daemon path. */
int power_pmu_dump_sample(void) {
    power_sample_t s;

    if (!_mapped) {
        printf("powerd: SPMI not mapped\n");
        return -1;
    }
    printf("sample: reading\n");
    if (power_pmu_sample(&s) == 0) {
        printf("sample: battery=%d charging=%d capacity=%u%% "
                "voltage=%dmV vbat_code=0x%04x chgr_state=%u batif=%02x\n",
                s.battery ? 1 : 0, s.charging ? 1 : 0, (unsigned)s.capacity,
                s.voltage_mv, (unsigned)s.vbat_code,
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

/* Raw write diagnostic: announce, pre-read, one RW-window write, post-read.
 * Every step is printed before the next hardware access so a bus-stalling
 * write self-identifies as the last printed line.  Pass the pre-read value
 * back as <val> to make it a no-op write-back probe; spmi_write applies the
 * EE0-ownership gate from the kernel before touching hardware. */
int power_pmu_write_args(int n, const char* const* args) {
    uint32_t apid, reg, val;
    int pre;

    if (n != 3 || atou(args[0], &apid) != 0 ||
            atou(args[1], &reg) != 0 || atou(args[2], &val) != 0) {
        printf("write: usage: powerd --write <apid> <reg> <val> "
                "(decimal)\n");
        return -1;
    }
    if (!_mapped) {
        printf("write: SPMI not mapped\n");
        return -1;
    }
    if (apid > 511U || reg > 255U || val > 255U) {
        printf("write: bad args apid=%u reg=%u val=%u\n", apid, reg, val);
        return -1;
    }
    printf("write apid=%u reg=0x%02x: pre-reading\n", apid, reg);
    pre = spmi_read(apid, reg);
    printf("write apid=%u reg=0x%02x: pre=%02x st=%08x %s\n",
            apid, reg, pre < 0 ? 0 : (unsigned)pre & 0xff,
            (unsigned)_last_arb_status,
            arb_err_name(_last_arb_status));
    printf("write apid=%u reg=0x%02x val=%02x: writing\n",
            apid, reg, val);
    if (spmi_write(apid, reg, val) != 0) {
        printf("write apid=%u reg=0x%02x: st=%08x %s\n",
                apid, reg, (unsigned)_last_arb_status,
                arb_err_name(_last_arb_status));
        return -1;
    }
    {
        int post;

        printf("write apid=%u reg=0x%02x: post-reading\n", apid, reg);
        post = spmi_read(apid, reg);
        printf("write apid=%u reg=0x%02x: post=%02x st=%08x %s\n",
                apid, reg, post < 0 ? 0 : (unsigned)post & 0xff,
                (unsigned)_last_arb_status,
                arb_err_name(_last_arb_status));
    }
    return 0;
}
