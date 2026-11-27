/*
 * battmgr.c - ADSP battery-manager client over the Q8B battery platform
 * control door (see battmgr.h and q8b_batt.h).
 *
 * Wire format and constants: battmgr_wire.h (mirrors Linux pmic_glink.h,
 * qcom_battmgr.c and pmic_glink.c).  Value scaling verified against the
 * upstream driver:
 *   BATT_PROP_CAPACITY   -> percent * 100 on the wire (Linux divides by 100)
 *   BATT_PROP_TEMP       -> deci-degrees Celsius
 *   BATT_PROP_VOLT_NOW   -> millivolts
 *   BATT_PROP_CURR_NOW   -> microamps
 *   BATT_PROP_CHG_FULL*  -> charge/energy counter; the TW220 firmware reports
 *                      these in mWh (its ENERGY_* sysfs attributes are the
 *                      populated ones, CHARGE_* read back empty)
 *   BATT_PROP_SOH        -> percent
 *
 * The daemon samples every second, so requests are paced: one property
 * request per second, round-robin, with BATT_PROP_CAPACITY refreshed more often.
 * A missing reply for three consecutive requests marks the channel down and
 * backs off; power_pmu then ignores the client and keeps its SPMI sources.
 */
#include <ewoksys/dma.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/syscall.h>
#include <stdio.h>
#include <syscalls.h>
#include <string.h>

#include <q8b_batt.h>

#include "battmgr.h"
#include "battmgr_wire.h"

#define BM_BUF_SIZE        256U
#define BM_REQ_PERIOD_NS   1000000000ull  /* one request per second */
#define BM_REPLY_TIMEOUT_NS 3000000000ull /* pending reply deadline */
#define BM_MAX_TIMEOUTS    3U             /* consecutive misses -> down */
#define BM_BACKOFF_NS      15000000000ull /* retry window after going down */
#define BM_MAX_RECV        4U             /* replies drained per slice */

/* ADSP firmware delivery (Q8B_BATT_BOOT): the kernel stages the image into
 * the pil_adsp_mem carveout and keeps 16 KB for PAS metadata after it. */
#define BM_ADSP_FW_MAX     16777216U
#define BM_ADSP_META       0x4000U
#define BM_ADSP_CHUNK      262144U

/* Round-robin order.  CAPACITY is inserted every third request regardless. */
static const uint32_t _props[] = {
    BATT_PROP_STATUS, BATT_PROP_PRESENT, BATT_PROP_VOLT_NOW, BATT_PROP_TEMP,
    BATT_PROP_CURR_NOW, BATT_PROP_SOH, BATT_PROP_HEALTH, BATT_PROP_CYCLE_COUNT,
    BATT_PROP_CHG_FULL, BATT_PROP_CHG_FULL_DESIGN, BATT_PROP_POWER_NOW,
};
#define BM_PROP_COUNT (sizeof(_props) / sizeof(_props[0]))

static battmgr_snapshot_t _snap;
static int _probed = -1;        /* -1 unprobed, 0 usable, 1 unsupported */
static ewokos_addr_t _buf_va;
static ewokos_addr_t _buf_pa;
static bool _have_buf;

static uint64_t _last_req_ns;
static uint64_t _pending_ns;
static int _pending_prop = -1;
static unsigned _rr;            /* round-robin cursor */
static unsigned _misses;        /* consecutive timeouts */
static uint64_t _backoff_ns;    /* when down: no requests until this tick */

static uint64_t now_ns(void) {
    uint64_t ns = 0;
    if (kernel_tic_nsec(&ns) != 0)
        return 0;
    return ns;
}

/* Talk to the kernel door.  Returns the raw syscall value: 0/len on success,
 * a Q8B_BATT_* error otherwise. */
static int batt_call(uint32_t op, ewokos_addr_t arg, ewokos_addr_t size) {
    return (int)syscall3(SYS_PLATFORM_CTL, (ewokos_addr_t)op, arg, size);
}

static void apply_property(uint32_t prop, uint32_t value) {
    switch (prop) {
    case BATT_PROP_CAPACITY:
        /* Firmware reports percent * 100 (Linux: value / 100). */
        _snap.capacity = (uint8_t)(value / 100U > 100U ? 100U : value / 100U);
        _snap.capacity_valid = true;
        break;
    case BATT_PROP_STATUS:
        _snap.status = value;
        _snap.status_valid = true;
        break;
    case BATT_PROP_PRESENT:
        _snap.present = value != 0U;
        _snap.present_valid = true;
        break;
    case BATT_PROP_VOLT_NOW:
        _snap.voltage_mv = (int)value;
        _snap.volt_valid = true;
        break;
    case BATT_PROP_CURR_NOW:
        _snap.current_ua = (int)value;
        _snap.curr_valid = true;
        break;
    case BATT_PROP_TEMP:
        _snap.temp_dc = (int)value;
        _snap.temp_valid = true;
        break;
    case BATT_PROP_SOH:
        _snap.soh = value > 100U ? 100U : (uint8_t)value;
        _snap.soh_valid = true;
        break;
    case BATT_PROP_HEALTH:
        _snap.health = (uint8_t)value;
        _snap.health_valid = true;
        break;
    case BATT_PROP_CYCLE_COUNT:
        _snap.cycle_count = value;
        _snap.cycle_valid = true;
        break;
    case BATT_PROP_CHG_FULL:
        _snap.full_mwh = value;
        _snap.full_valid = true;
        break;
    case BATT_PROP_CHG_FULL_DESIGN:
        _snap.design_mwh = value;
        _snap.design_valid = true;
        break;
    case BATT_PROP_POWER_NOW:
        _snap.power_now_uw = (int)value;
        _snap.power_valid = true;
        break;
    default:
        break;
    }
}

/* Decode one reply frame; returns 0 when it was a battmgr property reply. */
static int handle_frame(const uint8_t* data, uint32_t len) {
    uint32_t prop = 0, value = 0;

    if (battmgr_decode_property(data, len, &prop, &value) != 0)
        return -1;
    apply_property(prop, value);
    _snap.replies++;
    if (_pending_prop >= 0 && (uint32_t)_pending_prop == prop) {
        _pending_prop = -1;
        _misses = 0;
    }
    _snap.up = true;
    return 0;
}

static int drain(void) {
    unsigned i;
    int got = 0;

    for (i = 0; i < BM_MAX_RECV; i++) {
        int len = batt_call(Q8B_BATT_RECV, _buf_pa, BM_BUF_SIZE);
        if (len <= 0)
            break;
        if (len > (int)BM_BUF_SIZE)
            len = (int)BM_BUF_SIZE;
        if (handle_frame((const uint8_t*)(uintptr_t)_buf_va, (uint32_t)len) == 0)
            got++;
    }
    return got;
}

static int send_msg(const uint8_t* msg, uint32_t len) {
    if (!_have_buf || len > BM_BUF_SIZE)
        return -1;
    memcpy((void*)(uintptr_t)_buf_va, msg, len);
    return batt_call(Q8B_BATT_SEND, _buf_pa, len);
}

/* Hand the signed ADSP image to the kernel.  The kernel only accepts a DMA
 * buffer owned by this process, and it writes PAS metadata in the 16 KB that
 * follow the image, so the allocation covers both.  Returns 0 when the ADSP
 * reported ready. */
static int adsp_try_boot(void) {
    static const char* const paths[] = {
        "/etc/firmware/qcom/sc8280xp/Ntmer/tw220/qcadsp8280.mbn",
        "/etc/firmware/qcom/sc8280xp/qcadsp8280.mbn",
        "/etc/firmware/qcadsp8280.mbn",
    };
    FILE* f = 0;
    uint32_t size, got = 0;
    ewokos_addr_t va, pa;
    long len;

    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]) && !f; i++)
        f = fopen(paths[i], "rb");
    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) <= 0 ||
            (uint32_t)len > BM_ADSP_FW_MAX || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    size = (uint32_t)len;
    va = dma_alloc(0, size + BM_ADSP_META, false);
    if (va == 0) {
        fclose(f);
        return -1;
    }
    while (got < size) {
        uint32_t chunk = size - got;
        size_t n;
        if (chunk > BM_ADSP_CHUNK) chunk = BM_ADSP_CHUNK;
        n = fread((void*)(uintptr_t)(va + got), 1, chunk, f);
        if (n == 0) break;
        got += (uint32_t)n;
    }
    fclose(f);
    pa = dma_phy_addr(0, va);
    if (got != size || pa == 0) {
        dma_free(0, va);
        return -1;
    }
    {
        int rc = batt_call(Q8B_BATT_BOOT, pa, size);
        dma_free(0, va);
        return rc == 0 ? 0 : -1;
    }
}

int battmgr_init(void) {
    uint8_t msg[BATTMGR_ENABLE_MSG_SIZE];
    uint32_t size = 0;
    int caps;

    if (_probed >= 0)
        return _probed == 0 ? 0 : -1;

    caps = batt_call(Q8B_BATT_QUERY, 0, 0);
    if (caps == Q8B_BATT_UNSUPPORTED || caps < 0) {
        /* No ADSP channel in this kernel build: stay on the SPMI path. */
        _probed = 1;
        return -1;
    }
    if (!(caps & Q8B_BATT_CAP_ADSP_BOOTED)) {
        /* ADSP is never started by the boot firmware on this board (Linux
         * loads adsp.mdt itself), so stage 1 boots it here. */
        if (adsp_try_boot() == 0)
            caps = batt_call(Q8B_BATT_QUERY, 0, 0);
        else
            slog("q8b powerd: ADSP firmware image unavailable\n");
    }
    if (!(caps & Q8B_BATT_CAP_ADSP_BOOTED) || caps < 0) {
        /* No ADSP channel in this kernel build: stay on the SPMI path. */
        _probed = 1;
        return -1;
    }
    _snap.adsp_booted = (caps & Q8B_BATT_CAP_ADSP_BOOTED) != 0;
    _snap.channel_open = (caps & Q8B_BATT_CAP_CHANNEL_OPEN) != 0;
    _snap.pdr_ready = (caps & Q8B_BATT_CAP_PDR_READY) != 0;

    _buf_va = dma_alloc(0, BM_BUF_SIZE, false);
    if (_buf_va == 0) {
        _probed = 1;
        return -1;
    }
    size = (uint32_t)BM_BUF_SIZE;
    (void)size;
    _buf_pa = dma_phy_addr(0, _buf_va);
    if (_buf_pa == 0) {
        dma_free(0, _buf_va);
        _probed = 1;
        return -1;
    }
    _have_buf = true;

    /* Ask the firmware to start pushing notifications.  The Linux driver
     * sends the same enable request once the channel is open. */
    (void)battmgr_encode_enable(msg);
    if (send_msg(msg, BATTMGR_ENABLE_MSG_SIZE) == 0)
        _snap.notif_armed = true;

    _probed = 0;
    _last_req_ns = 0;
    return 0;
}

void battmgr_poll(void) {
    uint64_t t;
    uint32_t prop;
    uint8_t msg[BATTMGR_PROPERTY_MSG_SIZE];
    int rc;

    if (_probed != 0 || !_have_buf) {
        (void)drain();
        return;
    }
    t = now_ns();
    (void)drain();

    /* A pending request that never got an answer is dropped after its
     * deadline; consecutive misses take the channel down. */
    if (_pending_prop >= 0 && t != 0 && _pending_ns != 0 &&
            t > _pending_ns && (t - _pending_ns) > BM_REPLY_TIMEOUT_NS) {
        _pending_prop = -1;
        _snap.timeouts++;
        if (++_misses >= BM_MAX_TIMEOUTS) {
            _snap.up = false;
            _backoff_ns = t + BM_BACKOFF_NS;
        }
    }
    if (t != 0 && _backoff_ns != 0 && t < _backoff_ns)
        return;
    if (_backoff_ns != 0 && t >= _backoff_ns) {
        _backoff_ns = 0;
        _misses = 0;
    }
    if (_pending_prop >= 0)
        return;
    if (t != 0 && _last_req_ns != 0 && (t - _last_req_ns) < BM_REQ_PERIOD_NS)
        return;

    /* Every third request refreshes capacity so the status bar tracks the
     * pack closely; the rest of the round-robin fills the details. */
    if ((_snap.requests % 3U) == 0U)
        prop = BATT_PROP_CAPACITY;
    else {
        prop = _props[_rr % BM_PROP_COUNT];
        _rr++;
    }

    (void)battmgr_encode_property_get(msg, prop);
    rc = send_msg(msg, BATTMGR_PROPERTY_MSG_SIZE);
    _snap.requests++;
    _last_req_ns = t;
    if (rc == Q8B_BATT_UNSUPPORTED) {
        _probed = 1;
        _snap.up = false;
        _backoff_ns = t + BM_BACKOFF_NS;
        return;
    }
    if (rc == 0) {
        _pending_prop = (int)prop;
        _pending_ns = t;
    }
    _snap.up = _snap.up || (rc == 0);
}

const battmgr_snapshot_t* battmgr_state(void) {
    return &_snap;
}

bool battmgr_available(void) {
    return _probed == 0 && _snap.up && _snap.capacity_valid;
}
