/*
 * power_key.c - PMIC PON power-key access for the Robo & Kala TW220
 * (Qualcomm SC8280XP), extracted from 8gen3 powerd/power_pmu.c.
 *
 * All hardware access goes through the SPMI arbiter using the same
 * core/cfg/obs window pattern proven by goodixd on this board.  Nothing
 * else is mapped: GENI/GCC are unvalidated on the TW220 and must not be
 * touched.  This module is strictly READ-ONLY: observer reads only, no
 * RW window is mapped (RW-window writes to the battery PMIC channels
 * wedge the TW220 SPMI bus - see the powerd README).
 *
 * Arbitration model (mirrors the kernel spmi-pmic-arb v5 driver):
 *   - the APID map lives at core+0x900+4*n and covers up to 512 slots; a
 *     zero word marks an unused slot, bits[8:19] hold the PPID
 *     (peripheral = bits[8:15], SID = bits[16:19]);
 *   - channel ownership sits at cnfg+0x700+4*n, owner = low 3 bits;
 *     observer reads are allowed on any EE-owned channel;
 *   - OBS window: obsrvr + 0x10000*ee + 0x80*apid.  Status: bit0 done,
 *     bit1 transaction failed, bit2 denied, bit3 dropped.
 *
 * The power key is read the same read-only way: the PON peripheral's
 * debounced KPDPWR real-time status bit (PON_INT_RT_STS), exactly the
 * signal mainline Linux's pm8941-pwrkey driver reports as KEY_POWER.
 * No PON interrupt or debounce configuration is written.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <ewoksys/klog.h>
#include <ewoksys/syscall.h>

#include "power_key.h"

/* SPMI arbiter windows (addresses proven by goodixd on the TW220).
 * Shared with power_pmu.c in this process: SYS_MEM_MAP on an already
 * mapped VA/PA pair just rewrites the same PTEs, so whichever module
 * initializes first owns the mapping and the second is a no-op. */
#define SPMI_CORE_PHY      0x0c440000U
#define SPMI_CORE_VA       0x5a080000U
#define SPMI_CORE_SIZE     0x00004000U
#define SPMI_CFG_PHY       0x0c40a000U
#define SPMI_CFG_VA        0x5a0c0000U
#define SPMI_CFG_SIZE      0x00004000U
#define SPMI_OBS_PHY       0x0e600000U
#define SPMI_OBS_VA        0x5a100000U
#define SPMI_OBS_SIZE      0x00010000U

/* v5 APID map word: PPID = bits[8:19]. */
#define APID_PPID_SHIFT    8U
#define APID_PERIPH_MASK   0xffU
#define APID_SID_SHIFT     16U
#define APID_SID_MASK      0x0fU
#define APID_MAP_SLOTS     512U
/* Channel owner: cnfg+0x700+4*n, low 3 bits. */
#define OWNER_REG(n)       (0x700U + 4U * (n))

/* Power key: the PMIC PON (power-on) peripheral, read-only.  Register
 * layout and status bits follow mainline Linux pm8941-pwrkey.c: the
 * debounced KPDPWR real-time status lives in PON_INT_RT_STS (0x10),
 * bit 0 on gen1/gen2 PONs and bit 7 on gen3 (pmk8350, the SC8280XP
 * companion PMIC).  Peripheral type 0x04 == 0x01 identifies a PON;
 * subtype 0x05 tells the generation.  PON bases: gen3 HLOS pon@1300
 * (peripheral 0x13), gen3 PBS / gen1/gen2 pon@800 (peripheral 0x08). */
#define PERIPH_PON_HLOS    0x13U
#define PERIPH_PON_PBS     0x08U
#define PON_PERPH_TYPE     0x04U
#define PON_PERPH_SUBTYPE  0x05U
#define PON_TYPE_PON       0x01U
#define PON_INT_RT_STS     0x10U
#define PON_SUBTYPE_PRIMARY        0x01U
#define PON_SUBTYPE_SECONDARY      0x02U
#define PON_SUBTYPE_1REG           0x03U
#define PON_SUBTYPE_GEN2_PRIMARY   0x04U
#define PON_SUBTYPE_GEN2_SECONDARY 0x05U
#define PON_SUBTYPE_GEN3_PBS       0x08U
#define PON_SUBTYPE_GEN3_HLOS      0x09U
#define PON_KPDPWR_N_SET       0x01U  /* gen1/gen2 RT_STS bit 0 */
#define PON_GEN3_KPDPWR_N_SET  0x80U  /* gen3 RT_STS bit 7 */
#define PON_CANDIDATE_MAX  12U

static volatile uint8_t* _core;
static volatile uint8_t* _cfg;
static volatile uint8_t* _obs;
static bool _mapped;
static bool _verbose;

/* PON power-key discovery: every pon@1300/pon@800 candidate from the map,
 * then the one selected by pon_select() (-1 = no usable power key). */
static int _pon_cand_apids[PON_CANDIDATE_MAX];
static int _pon_cand_sids[PON_CANDIDATE_MAX];
static unsigned _pon_cand_count;
static int _pon_apid = -1, _pon_sid = -1;
static uint32_t _pon_subtype;
static uint32_t _pon_kpdpwr_mask;

#define VPRINTF(...) \
    do { \
        if (_verbose) \
            printf(__VA_ARGS__); \
    } while (0)

void power_key_verbose(bool on) {
    _verbose = on;
}

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
 * contract as goodixd). */
static int spmi_wait(volatile uint8_t* channel) {
    for (unsigned poll = 0; poll < 1000; poll++) {
        uint32_t status = rd(channel, 8);
        if (status & 1U)
            return (status & 14U) ? -1 : 0;
        usleep(10);
    }
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

/* Rank PON subtypes for power-key selection: the HLOS side of a gen3 PON
 * (pmk8350 pon@1300) is where Linux wires KEY_POWER on SC8280XP-class
 * boards, so it wins over every legacy layout.  Secondary PONs (subtypes
 * 2/5) carry only RESIN, no KPDPWR - they are not candidates at all. */
static int pon_rank(uint32_t subtype) {
    switch (subtype) {
    case PON_SUBTYPE_GEN3_HLOS:      return 0;
    case PON_SUBTYPE_GEN2_PRIMARY:   return 1;
    case PON_SUBTYPE_PRIMARY:        return 2;
    case PON_SUBTYPE_GEN3_PBS:       return 3;
    case PON_SUBTYPE_1REG:           return 4;
    default:                         return -1;
    }
}

/* Observer-read every PON candidate's type/subtype/RT_STS and keep the
 * best-ranked one that answers and reads RELEASED.  A candidate stuck
 * "pressed" before any press is an unwired/floating KPDPWR input (the
 * board has several PMICs but only one power key).  All reads are
 * observer reads: no PON configuration is written. */
static void pon_select(void) {
    int best_rank = 99;

    _pon_apid = -1;
    _pon_sid = -1;
    _pon_subtype = 0;
    _pon_kpdpwr_mask = 0;
    for (unsigned i = 0; i < _pon_cand_count; i++) {
        uint32_t apid = (uint32_t)_pon_cand_apids[i];
        int type = spmi_read(apid, PON_PERPH_TYPE);
        int subtype = spmi_read(apid, PON_PERPH_SUBTYPE);
        int rank;
        int sts;
        uint32_t mask;

        if (type < 0 || (uint32_t)type != PON_TYPE_PON || subtype < 0)
            continue;
        rank = pon_rank((uint32_t)subtype);
        if (rank < 0 || rank >= best_rank)
            continue;
        mask = ((uint32_t)subtype == PON_SUBTYPE_GEN3_PBS ||
                (uint32_t)subtype == PON_SUBTYPE_GEN3_HLOS) ?
                PON_GEN3_KPDPWR_N_SET : PON_KPDPWR_N_SET;
        sts = spmi_read(apid, PON_INT_RT_STS);
        if (sts < 0) {
            VPRINTF("powerd: pon apid=%u sid=%d subtype=%d read failed\n",
                    apid, _pon_cand_sids[i], subtype);
            continue;
        }
        if ((uint32_t)sts & mask) {
            VPRINTF("powerd: pon apid=%u sid=%d subtype=%d stuck "
                    "pressed, skipped\n", apid, _pon_cand_sids[i], subtype);
            continue;
        }
        best_rank = rank;
        _pon_apid = (int)apid;
        _pon_sid = _pon_cand_sids[i];
        _pon_subtype = (uint32_t)subtype;
        _pon_kpdpwr_mask = mask;
    }
}

/* Walk the APID map for PON candidates.  The scan runs over all 512
 * slots; the FEATURES register (core+4) is unstable on the TW220, so it
 * is only used as a display hint.  Reading all 512 slots has proven
 * safe on this board. */
static int enumerate(void) {
    uint32_t version = rd(_core, 0);
    uint32_t features = rd(_core, 4) & 0x7ffU;
    uint32_t slots = features != 0 && features <= APID_MAP_SLOTS ?
            features : APID_MAP_SLOTS;

    if (version < 0x50000000U || version >= 0x70000000U)
        return -1;
    _pon_cand_count = 0;
    for (uint32_t i = 0; i < slots; i++) {
        uint32_t map = rd(_core, 0x900U + 4U * i);
        if (map == 0)
            continue;
        uint32_t periph = (map >> APID_PPID_SHIFT) & APID_PERIPH_MASK;
        uint32_t sid = (map >> APID_SID_SHIFT) & APID_SID_MASK;
        uint32_t ee = rd(_cfg, OWNER_REG(i)) & 7U;

        /* PON candidates: gen3 HLOS pon@1300 and legacy/PBS pon@800.
         * pon_select() verifies type/subtype after the scan. */
        if (ee == 0 && (periph == PERIPH_PON_HLOS ||
                periph == PERIPH_PON_PBS) &&
                _pon_cand_count < PON_CANDIDATE_MAX) {
            _pon_cand_apids[_pon_cand_count] = (int)i;
            _pon_cand_sids[_pon_cand_count] = (int)sid;
            _pon_cand_count++;
        }
    }
    pon_select();
    return 0;
}

int power_key_init(void) {
    if (_mapped)
        return _pon_apid >= 0 ? 0 : -1;
    VPRINTF("powerd: mapping SPMI windows\n");
    if (map_at(SPMI_CORE_VA, SPMI_CORE_PHY, SPMI_CORE_SIZE) != 0 ||
            map_at(SPMI_CFG_VA, SPMI_CFG_PHY, SPMI_CFG_SIZE) != 0 ||
            map_at(SPMI_OBS_VA, SPMI_OBS_PHY, SPMI_OBS_SIZE) != 0) {
        slog("qcom8gen3 powerd: SPMI window mapping failed\n");
        return -1;
    }
    _core = (volatile uint8_t*)(uintptr_t)SPMI_CORE_VA;
    _cfg = (volatile uint8_t*)(uintptr_t)SPMI_CFG_VA;
    _obs = (volatile uint8_t*)(uintptr_t)SPMI_OBS_VA;
    _mapped = true;
    VPRINTF("powerd: reading APID map\n");
    if (enumerate() != 0) {
        slog("qcom8gen3 powerd: SPMI arbiter version check failed\n");
        return -1;
    }
    if (_pon_apid >= 0)
        slog("qcom8gen3 powerd: power key pon apid=%d sid=%d subtype=%u\n",
                _pon_apid, _pon_sid, (unsigned)_pon_subtype);
    else
        slog("qcom8gen3 powerd: no usable PON power key found\n");
    return _pon_apid >= 0 ? 0 : -1;
}

/* Live power-key state: one observer read of the selected PON's
 * INT_RT_STS.  -1 means "unknown" (no PON selected or the read failed);
 * the caller must keep its previous key state in that case, never treat
 * it as a release. */
int power_key_state(void) {
    int sts;

    if (!_mapped || _pon_apid < 0)
        return -1;
    sts = spmi_read((uint32_t)_pon_apid, PON_INT_RT_STS);
    if (sts < 0)
        return -1;
    return ((uint32_t)sts & _pon_kpdpwr_mask) != 0 ? 1 : 0;
}

int power_key_info(int* apid, int* sid, int* subtype) {
    if (_pon_apid < 0)
        return -1;
    if (apid != NULL)
        *apid = _pon_apid;
    if (sid != NULL)
        *sid = _pon_sid;
    if (subtype != NULL)
        *subtype = (int)_pon_subtype;
    return 0;
}

void power_key_dump(void) {
    printf("pon candidates:");
    for (unsigned i = 0; i < _pon_cand_count; i++)
        printf(" %d(sid%d)", _pon_cand_apids[i], _pon_cand_sids[i]);
    if (_pon_apid >= 0)
        printf(" | key: apid=%d sid=%d subtype=%u mask=%02x sts=%d\n",
                _pon_apid, _pon_sid, (unsigned)_pon_subtype,
                (unsigned)_pon_kpdpwr_mask, power_key_state());
    else
        printf(" | key: none\n");
}