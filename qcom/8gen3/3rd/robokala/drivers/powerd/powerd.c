/*
 * powerd.c - the single power daemon for the Robo & Kala TW220
 * (Qualcomm SC8280XP).  Owns everything power-related on this board:
 *
 *  - battery status on /dev/power0, the same 3-byte protocol as the
 *    uConsole powerd, consumed by statusbar's PowerInfo widget:
 *      byte0 = battery present, byte1 = charging, byte2 = capacity percent.
 *    The hardware side lives in power_pmu.c: read-only SMB5 charger
 *    status and fuel-gauge registers over SPMI, with the real capacity
 *    preferred from the ADSP firmware battery manager (PMIC GLINK
 *    battmgr, see battmgr_wire.h).  If the board has no charger PMIC
 *    (EC-managed battery) the device still starts and simply reports
 *    [0,0,0], which hides the battery widget;
 *  - the PMIC PON power key (power_key.c, read-only KPDPWR status, the
 *    same signal Linux's pm8941-pwrkey reports): a short press toggles
 *    display sleep/wake through xserverd's "sleep"/"wake" dev commands,
 *    and holding the key for 3 seconds requests PSCI SYSTEM_OFF;
 *  - the keyboard/lid hall sensor (power_lid.c, TLMM GPIO 107,
 *    active-low - the pad the mainline TW220 DTS wires to SW_LID):
 *    folding the keyboard onto the panel sleeps the display, opening
 *    it wakes it again.
 *
 * The key and the lid are polled on a 50 ms tick, the battery once a
 * second on the same tick.  Sleep fans out best-effort to the whole board:
 * xserverd pauses compositing/input, the AMOLED panel enters DSI sleep-in,
 * the GPU drops into GMU IFPC, Wi-Fi enables STA power-save (link kept),
 * the touch IC is powered off, and the secondary CPU cores park via PSCI
 * CPU_SUSPEND.  There is no suspend-to-RAM door on this WoA firmware, so
 * core 0 keeps running the tick and the polled wake sources while the
 * screen is dark.  Both wake sources are polled independently of xserverd,
 * so they always wake; they share one sleep state, so a power-key press can
 * wake a lid-closed device and only a real lid transition changes the state
 * afterwards.
 *
 * dev commands: help / status / reboot / shutdown (the last two
 * root-gated PSCI requests).  Bring-up modes: --dump/--probe/--sample/
 * --mapall/--seed/--read (battery, read-only) and --key/--lid (live
 * input monitors).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>

#include <ewoksys/klog.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/proc.h>
#include <ewoksys/syscall.h>
#include <ewoksys/vdevice.h>

#include <q8b_npu.h>

#include "power_key.h"
#include "power_lid.h"
#include "power_pmu.h"

/* 50 ms gives the 3 s long-press measurement ~1.7% resolution and a
 * short press is recognized within one tick of the release. */
#define KEY_POLL_US        50000U
#define KEY_LONG_PRESS_MS  3000U
/* Releases shorter than this are glitches, not presses (the PON hardware
 * debounce is ~15 ms; this is the software guard on top of it). */
#define KEY_MIN_PRESS_MS   100U
/* The hall level must hold this long before a lid transition is acted
 * on: attaching/folding the keyboard bounces the magnet past the sensor
 * for a few ticks, and each bounce must not toggle the display. */
#define LID_DEBOUNCE_MS    200U
/* Battery sampling period in loop ticks (20 * 50 ms = 1 s). */
#define BATTERY_TICKS      20U

static power_sample_t _sample;
static unsigned _tick;

/* Power-key state machine.  _key_ignore discards a press that was already
 * down when powerd started (the boot press) and re-arms on release;
 * it also latches after a long press whose SYSTEM_OFF unexpectedly
 * returned, so a failed off cannot re-fire every tick. */
static bool _key_armed;
static bool _key_down;
static bool _key_ignore;
static uint64_t _key_down_ms;
static bool _sleeping;
static bool _cpu_sleep;   /* kernel secondary-core PSCI CPU_SUSPEND gate */
/* PSCI secondary-core power-down is OPT-IN.  powerd polls the hall sensor
 * and the power key (register reads, not interrupts), so a core parked in
 * CPU_SUSPEND has no lid-open IRQ to wake it: the wake fan-out can deadlock
 * (screen lit, input dead, no wake trace).  The sleep fan-out therefore
 * leaves cores online (WFI idle) unless the operator explicitly arms suspend
 * with `cpusleep on`.  The flag persists across lid cycles so `cpusleep off`
 * stays disarmed until explicitly re-armed, not re-overridden by each sleep. */
static bool _cpu_sleep_armed;

/* Lid (hall sensor) state machine.  _lid_closed is the last debounced
 * state; a level that differs from it must persist for LID_DEBOUNCE_MS
 * (_lid_pending/_lid_pending_ms) before it commits.  The first valid
 * read arms the machine and applies the boot state, so a device started
 * with the keyboard closed comes up asleep. */
static bool _lid_armed;
static bool _lid_closed;
static bool _lid_pending;
static uint64_t _lid_pending_ms;

/* Drive the whole-board sleep state.  "sleep" stops compositing (black
 * frame) and then parks each subsystem independently: the AMOLED panel
 * (DSI sleep-in), the GPU (GMU IFPC), Wi-Fi (STA power-save, link kept),
 * the touch IC (rails off) and finally the secondary CPU cores (PSCI
 * CPU_SUSPEND).  "wake" lights the display first (panel + compositor),
 * then restores the GPU, touch and Wi-Fi.  Every leg is
 * best-effort: a missing or unresponsive node is logged and never blocks
 * the others, so one failing subsystem cannot wedge the sleep/wake path.
 * powerd keeps its own state so the toggle still works when the X server
 * is not up (headless boot).  Shared by the power key and the lid so the
 * two sources never fight over _sleeping. */
/* Persistent sleep/wake trace.  /dev/log is a RAM ring that drains on read
 * and vanishes on reboot, so a slow-wake report cannot be diagnosed after
 * the fact.  Each transition is accumulated here and flushed ONCE to
 * /tmp/powerd.log (a real ext3 file, survives reboot, readable from the
 * host) AFTER the fan-out completes - so file I/O never delays the display
 * legs.  Every line carries the kernel-tic millisecond clock. */
static char _trace[1536];
static size_t _trace_len;

static void power_trace(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char line[256];
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    slog("q8b powerd: %s", line);
    if (_trace_len < sizeof(_trace) - 1) {
        size_t cpy = (size_t)n;
        if (cpy > sizeof(_trace) - 1 - _trace_len)
            cpy = sizeof(_trace) - 1 - _trace_len;
        memcpy(_trace + _trace_len, line, cpy);
        _trace_len += cpy;
        _trace[_trace_len] = '\0';
    }
}

static void power_trace_flush(void) {
    if (_trace_len == 0)
        return;
    int fd = open("/tmp/powerd.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0)
        return;
    (void)write(fd, _trace, _trace_len);
    fsync(fd);
    close(fd);
}

static void power_send_cmd(const char* node, const char* cmd, const char* why) {
    uint64_t t0 = kernel_tic_ms(0);
    char* ret = dev_cmd(node, cmd);
    unsigned ms = (unsigned)(kernel_tic_ms(0) - t0);
    if (ret == NULL) {
        power_trace("%s %s: no reply from %s (%u ms)\n", why, cmd, node, ms);
        return;
    }
    size_t len = strlen(ret);
    while (len > 0 && (ret[len - 1] == '\n' || ret[len - 1] == '\r'))
        ret[--len] = '\0';
    power_trace("%s %s %s: %u ms (%s)\n", why, cmd, node, ms, ret);
    free(ret);
}

static void power_cpu_sleep(bool sleep, const char* why) {
    uint64_t ret = syscall3(SYS_PLATFORM_CTL, Q8B_NPU_CPU_SLEEP,
            sleep ? 1 : 0, 0);
    _cpu_sleep = sleep;
    power_trace("%s cpu_sleep=%d rc=%llu\n", why, sleep ? 1 : 0,
            (unsigned long long)ret);
}

static void power_set_sleep(bool sleep, const char* why) {
    const char* cmd = sleep ? "sleep" : "wake";

    if (sleep == _sleeping)
        return;
    _trace_len = 0;
    power_trace("=== %s -> %s (t=%llu ms) ===\n", why, cmd,
            (unsigned long long)kernel_tic_ms(0));
    if (sleep) {
        power_send_cmd("/dev/x", cmd, why);
        power_send_cmd("/dev/disp0", cmd, why);
        power_send_cmd("/dev/g2d", cmd, why);
        power_send_cmd("/dev/wl0", cmd, why);
        power_send_cmd("/dev/touch0", cmd, why);
        /* Only park secondary cores when explicitly armed; otherwise the
         * polled lid/key wake path can deadlock on a parked core. */
        if (_cpu_sleep_armed)
            power_cpu_sleep(true, why);
    } else {
        /* Wake the display FIRST: the panel and the compositor are the only
         * legs the user sees, so they must never queue behind a slow
         * subsystem (every dev_cmd is a blocking IPC with a 10 s kernel
         * timeout).  cpu_sleep(false) also kicks every parked secondary
         * core awake in the kernel, so the fan-out IPCs below reach running
         * cores.  The GPU IFPC bit is cleared after the first repaint: GX
         * restore on that submit is GMU-firmware-autonomous by design. */
        if (_cpu_sleep)
            power_cpu_sleep(false, why);
        power_send_cmd("/dev/disp0", cmd, why);
        power_send_cmd("/dev/x", cmd, why);
        power_send_cmd("/dev/g2d", cmd, why);
        power_send_cmd("/dev/touch0", cmd, why);
        power_send_cmd("/dev/wl0", cmd, why);
    }
    _sleeping = sleep;
    /* Record the kernel resume mask, then persist the whole transition.
     * Done last so the display is already lit before any disk write. */
    uint64_t resumed = syscall3(SYS_PLATFORM_CTL,
            Q8B_NPU_CPU_SLEEP_STATS, 0, 0);
    power_trace("done sleeping=%d cpu_resumed=0x%02x (t=%llu ms)\n",
            sleep ? 1 : 0, (unsigned)(resumed & 0xffu),
            (unsigned long long)kernel_tic_ms(0));
    power_trace_flush();
}

/* Short press: toggle display sleep. */
static void power_key_sleep_toggle(void) {
    power_set_sleep(!_sleeping, "power key short press");
}

static void power_key_step(void) {
    int ks = power_key_state();
    uint64_t now;
    bool down;

    if (ks < 0)
        return;
    now = kernel_tic_ms(0);
    down = (ks != 0);
    if (!_key_armed) {
        _key_armed = true;
        _key_down = down;
        _key_ignore = down;
        _key_down_ms = now;
    }
    else if (down && !_key_down) {
        _key_down = true;
        _key_down_ms = now;
    }
    else if (!down && _key_down) {
        uint64_t held = now - _key_down_ms;

        _key_down = false;
        if (!_key_ignore && held >= KEY_MIN_PRESS_MS &&
                held < KEY_LONG_PRESS_MS)
            power_key_sleep_toggle();
        _key_ignore = false;
    }
    else if (down && _key_down && !_key_ignore &&
            now - _key_down_ms >= KEY_LONG_PRESS_MS) {
        slog("q8b powerd: power key held %u ms -> shutdown\n",
                (unsigned)(now - _key_down_ms));
        _key_ignore = true;
        /* The long-press path cuts power with no further warning: flush
         * the persistent state (and anything else dirty) before PSCI
         * off.  A functioning PSCI path never returns. */
        sync();
        (void)syscall3(SYS_PLATFORM_CTL, Q8B_NPU_SYS_OFF, 0, 0);
    }
}

/* Poll the hall sensor and sleep on close / wake on open.  A read
 * failure keeps the previous state (never treated as a transition). */
static void lid_step(void) {
    int ls = power_lid_state();
    uint64_t now;
    bool closed;

    if (ls < 0)
        return;
    closed = (ls != 0);
    now = kernel_tic_ms(0);
    if (!_lid_armed) {
        _lid_armed = true;
        _lid_closed = closed;
        _lid_pending = closed;
        slog("q8b powerd: lid %s at start\n",
                closed ? "closed" : "open");
        if (closed)
            power_set_sleep(true, "lid closed");
        return;
    }
    if (closed == _lid_closed) {
        _lid_pending = closed; /* bounce back: cancel the candidate */
        return;
    }
    if (closed != _lid_pending) {
        _lid_pending = closed;
        _lid_pending_ms = now;
        return;
    }
    if (now - _lid_pending_ms >= LID_DEBOUNCE_MS) {
        _lid_closed = closed;
        power_set_sleep(closed, closed ? "lid closed" : "lid opened");
    }
}

static int power_step(vdevice_t* dev, void* p) {
    (void)dev;
    (void)p;

    power_key_step();
    lid_step();
    /* Battery once a second; tick 0 samples immediately at start.
     * power_pmu_sample() is safe even when the SPMI init failed (it
     * still tries the ADSP battmgr door and zero-fills the rest). */
    if (_tick == 0)
        (void)power_pmu_sample(&_sample);
    _tick = (_tick + 1U) % BATTERY_TICKS;
    usleep(KEY_POLL_US);
    return 0;
}

static int power_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        void* buf, int size, off_t offset, void* p) {
    (void)dev;
    (void)fd;
    (void)from_pid;
    (void)node;
    (void)offset;
    (void)p;

    /* The callers of this protocol read 3 bytes, but never write past the
     * caller's buffer when a shorter read was requested. */
    uint8_t data[3];
    int n = size < 3 ? size : 3;
    data[0] = _sample.battery ? 1 : 0;
    data[1] = _sample.charging ? 1 : 0;
    data[2] = _sample.capacity;
    memcpy(buf, data, (size_t)n);
    return n;
}

static char* dup_text(const char* text) {
    char* ret = (char*)malloc(strlen(text) + 1);
    if (ret != NULL)
        strcpy(ret, text);
    return ret;
}

static char* power_help(void) {
    return dup_text(
        "usage: dev.cmd /dev/power0 <cmd>\n"
        "  help      show this help\n"
        "  status    re-sample and show battery + key / lid / sleep state\n"
        "  reboot    request PSCI SYSTEM_RESET (root only)\n"
        "  shutdown  request PSCI SYSTEM_OFF (root only)\n"
        "  cpusleep on|off  arm+gate kernel secondary-core PSCI suspend (root)\n"
        "  trace     show the last sleep/wake transition (also /tmp/powerd.log)\n"
        "sleep/wake fans out to /dev/x, /dev/disp0, /dev/g2d, /dev/wl0,\n"
        "  /dev/touch0 and the kernel CPU-sleep gate (best-effort)\n"
        "power key: short press toggles display sleep/wake,\n"
        "           hold 3s requests PSCI SYSTEM_OFF\n"
        "keyboard lid: closed (hall sensor) sleeps the display,\n"
        "              opened wakes it\n");
}

static char* power_status(void) {
    char buf[384];
    int off;
    const char* src;
    int k = power_key_state();
    int lid = power_lid_state();

    /* A status query re-samples so the reply reflects the live PMIC. */
    (void)power_pmu_sample(&_sample);
    switch (_sample.cap_src) {
    case CAP_SRC_BATTMGR:
        src = "battmgr (ADSP)";
        break;
    case CAP_SRC_CHARGER:
        src = "charger full";
        break;
    case CAP_SRC_FG:
        src = "fuel gauge";
        break;
    case CAP_SRC_REMEMBERED:
        src = "last known";
        break;
    default:
        src = "none";
        break;
    }
    if (_sample.vbat_valid) {
        off = snprintf(buf, sizeof(buf),
                "battery=%s charging=%s capacity=%u%% (%s) "
                "voltage=%d.%03dV chg_state=%u batif=%02x "
                "sid(chg=%d vbat=%d)\n",
                _sample.battery ? "yes" : "no",
                _sample.charging ? "yes" : "no",
                (unsigned)_sample.capacity, src,
                _sample.voltage_mv / 1000, _sample.voltage_mv % 1000,
                (unsigned)_sample.chgr_state, (unsigned)_sample.batif_sts,
                _sample.charger_sid, _sample.vbat_sid);
    } else if (_sample.capacity > 0) {
        /* Charger-derived or remembered capacity with no voltage
         * reading: the TW220's fuel gauge boots dormant. */
        off = snprintf(buf, sizeof(buf),
                "battery=%s charging=%s capacity=%u%% (%s) "
                "chg_state=%u batif=%02x sid(chg=%d vbat=%d)\n",
                _sample.battery ? "yes" : "no",
                _sample.charging ? "yes" : "no",
                (unsigned)_sample.capacity, src,
                (unsigned)_sample.chgr_state, (unsigned)_sample.batif_sts,
                _sample.charger_sid, _sample.vbat_sid);
    } else {
        off = snprintf(buf, sizeof(buf),
                "battery=%s charging=%s capacity=unknown "
                "chg_state=%u batif=%02x sid(chg=%d vbat=%d)\n",
                _sample.battery ? "yes" : "no",
                _sample.charging ? "yes" : "no",
                (unsigned)_sample.chgr_state, (unsigned)_sample.batif_sts,
                _sample.charger_sid, _sample.vbat_sid);
    }
    if (off > 0 && (size_t)off < sizeof(buf)) {
        /* Kernel-side proof of PSCI CPU_SUSPEND: bit N set = core N has
         * completed at least one suspend/resume cycle.  TW220 has no wired
         * kernel console, so the once-per-core kernel printf is invisible;
         * this mask is the observable verification hook.  >0xff means the
         * op is unsupported (non-SMP kernel) or the caller lacks root. */
        uint64_t resumed = syscall3(SYS_PLATFORM_CTL,
                Q8B_NPU_CPU_SLEEP_STATS, 0, 0);
        char resumed_txt[16];
        if (resumed <= 0xffu)
            snprintf(resumed_txt, sizeof(resumed_txt), "0x%02x",
                    (unsigned)resumed);
        else
            snprintf(resumed_txt, sizeof(resumed_txt), "n/a");
        snprintf(buf + off, sizeof(buf) - (size_t)off,
                "key=%s lid=%s sleeping=%s cpu_sleep=%s cpu_resumed=%s\n",
                k < 0 ? "none" : (k ? "down" : "up"),
                lid < 0 ? "none" : (lid ? "closed" : "open"),
                _sleeping ? "yes" : "no",
                _cpu_sleep ? "yes" : "no",
                resumed_txt);
    }
    return dup_text(buf);
}

static char* power_reboot(void) {
    uint64_t ret = syscall3(SYS_PLATFORM_CTL, Q8B_NPU_SYS_RESET, 0, 0);
    /* A functioning PSCI path never returns. */
    char buf[64];
    snprintf(buf, sizeof(buf), "reboot returned %llu\n",
            (unsigned long long)ret);
    return dup_text(buf);
}

static char* power_shutdown(void) {
    /* Cut power with no further warning: flush the persistent state file
     * (and anything else dirty) before PSCI off. */
    sync();
    uint64_t ret = syscall3(SYS_PLATFORM_CTL, Q8B_NPU_SYS_OFF, 0, 0);
    /* A functioning PSCI path never returns. */
    char buf[64];
    snprintf(buf, sizeof(buf), "shutdown returned %llu\n",
            (unsigned long long)ret);
    return dup_text(buf);
}

static bool power_caller_is_root(int from_pid) {
    procinfo_t pinfo;
    if (proc_info(from_pid, &pinfo) != 0)
        return false;
    return pinfo.uid == 0;
}

static char* power_dev_cmd(vdevice_t* dev, int from_pid, int argc,
        char** argv, void* p) {
    (void)dev;
    (void)p;

    if (argc <= 0 || argv == NULL || argv[0] == NULL)
        return NULL;
    if (strcmp(argv[0], "help") == 0)
        return power_help();
    if (strcmp(argv[0], "status") == 0)
        return power_status();
    if (strcmp(argv[0], "reboot") == 0) {
        if (!power_caller_is_root(from_pid))
            return dup_text("reboot: permission denied\n");
        return power_reboot();
    }
    if (strcmp(argv[0], "shutdown") == 0) {
        if (!power_caller_is_root(from_pid))
            return dup_text("shutdown: permission denied\n");
        return power_shutdown();
    }
    /* Opt-in arm/disarm for the kernel secondary-core PSCI CPU_SUSPEND
     * (default off; see _cpu_sleep_armed).  `cpusleep on` arms suspend for
     * subsequent sleep cycles, `cpusleep off` disarms it.  Does not itself
     * sleep/wake the display. */
    if (strcmp(argv[0], "cpusleep") == 0) {
        if (!power_caller_is_root(from_pid))
            return dup_text("cpusleep: permission denied\n");
        if (argc < 2)
            return dup_text("usage: cpusleep on|off\n");
        int on = strcmp(argv[1], "on") == 0;
        _cpu_sleep_armed = on;
        _trace_len = 0;
        power_cpu_sleep(on, "devcmd cpusleep");
        power_trace_flush();
        return dup_text(on ? "cpu_sleep armed + enabled\n"
                           : "cpu_sleep disarmed + disabled\n");
    }
    if (strcmp(argv[0], "trace") == 0)
        return dup_text(_trace_len ? _trace : "(no sleep/wake transition yet)\n");
    return NULL;
}

/* Bring-up diagnostics.  Each stage prints directly to stdout so a hang
 * leaves the last completed stage visible on the wire.  All modes are
 * strictly read-only - the driver has no SPMI write path (writes to the
 * battery PMIC channels wedge the TW220 bus).  Run from a shell:
 *   /drivers/robokala/powerd --dump    map summary (safe core-register reads)
 *   /drivers/robokala/powerd --probe   + observer-read sweep
 *   /drivers/robokala/powerd --sample  + one live sample
 *   /drivers/robokala/powerd --mapall  full APID map listing
 *   /drivers/robokala/powerd --seed <chg_sid> <cap>
 *                                   bootstrap /etc/powerd.state (no SPMI
 *                                   access; run once at deployment)
 *   /drivers/robokala/powerd --read <apid> <reg> <len> ...
 *                                   raw observer reads (read-only) */
static int dump_mode(const char* stage) {
    printf("powerd: SPMI init...\n");
    power_pmu_verbose(true);
    if (power_pmu_init() != 0) {
        printf("powerd: SPMI init failed\n");
        return -1;
    }
    power_pmu_dump_map();
    if (strcmp(stage, "--probe") == 0)
        power_pmu_dump_probe();
    if (strcmp(stage, "--sample") == 0)
        power_pmu_dump_sample();
    if (strcmp(stage, "--mapall") == 0)
        power_pmu_dump_mapall();
    return 0;
}

/* --read <apid> <reg> <len> ... : raw observer reads, no writes. */
static int read_mode(int n, const char* const* args) {
    if (n == 0 || n % 3 != 0) {
        fprintf(stderr, "usage: powerd --read <apid> <reg> <len> ...\n");
        return -1;
    }
    printf("powerd: SPMI init...\n");
    power_pmu_verbose(true);
    if (power_pmu_init() != 0) {
        printf("powerd: SPMI init failed\n");
        return -1;
    }
    return power_pmu_read(n, args);
}

/* --key [secs]: live power-key monitor, strictly read-only.  Prints the
 * selected PON and every state transition for the given window (default
 * 10 s) - the bring-up check that the right PON was picked and that a
 * press/release actually moves the KPDPWR status bit. */
static int key_mode(int n, char** args) {
    uint32_t secs = 10;
    int apid, sid, subtype;
    int last = -2;
    uint64_t end;

    if (n > 0) {
        int v = atoi(args[0]);
        if (v <= 0 || v > 3600) {
            fprintf(stderr,
                    "usage: powerd --key [seconds (1..3600)]\n");
            return -1;
        }
        secs = (uint32_t)v;
    }
    power_key_verbose(true);
    if (power_key_init() != 0) {
        printf("key: no usable PON power key found\n");
        return -1;
    }
    if (power_key_info(&apid, &sid, &subtype) != 0) {
        printf("key: no usable PON power key found\n");
        return -1;
    }
    power_key_dump();
    printf("key: pon apid=%d sid=%d subtype=%d, monitoring %u s\n",
            apid, sid, subtype, (unsigned)secs);
    end = kernel_tic_ms(0) + (uint64_t)secs * 1000U;
    while (kernel_tic_ms(0) < end) {
        int ks = power_key_state();
        if (ks != last) {
            printf("key: %s\n",
                    ks < 0 ? "unknown" : (ks ? "down" : "up"));
            last = ks;
        }
        usleep(KEY_POLL_US);
    }
    return 0;
}

/* --lid [secs]: live hall-sensor monitor.  Prints the GPIO level and
 * every open/closed transition for the given window (default 10 s) -
 * the bring-up check that GPIO 107 is the right pad and that folding
 * the keyboard actually moves it.  Maps TLMM only, no SPMI access. */
static int lid_mode(int n, char** args) {
    uint32_t secs = 10;
    int last = -2;
    uint64_t end;

    if (n > 0) {
        int v = atoi(args[0]);
        if (v <= 0 || v > 3600) {
            fprintf(stderr,
                    "usage: powerd --lid [seconds (1..3600)]\n");
            return -1;
        }
        secs = (uint32_t)v;
    }
    if (power_lid_init() != 0) {
        printf("lid: TLMM mapping failed\n");
        return -1;
    }
    printf("lid: gpio%u (active-low), monitoring %u s\n",
            (unsigned)POWER_LID_GPIO, (unsigned)secs);
    end = kernel_tic_ms(0) + (uint64_t)secs * 1000U;
    while (kernel_tic_ms(0) < end) {
        int ls = power_lid_state();
        if (ls != last) {
            printf("lid: %s\n",
                    ls < 0 ? "unknown" : (ls ? "closed" : "open"));
            last = ls;
        }
        usleep(KEY_POLL_US);
    }
    return 0;
}

int main(int argc, char** argv) {
    const char* mount = "/dev/power0";
    bool bat_ok, key_ok, lid_ok;
    vdevice_t dev;

    if (argc > 1 && strcmp(argv[1], "--key") == 0)
        return key_mode(argc - 2, &argv[2]);
    if (argc > 1 && strcmp(argv[1], "--lid") == 0)
        return lid_mode(argc - 2, &argv[2]);
    if (argc > 1 && strcmp(argv[1], "--read") == 0)
        return read_mode(argc - 2, (const char* const*)&argv[2]);
    if (argc > 1 && strcmp(argv[1], "--seed") == 0)
        return power_pmu_seed_args(argc - 2, (const char* const*)&argv[2]);
    if (argc > 1 &&
            (strcmp(argv[1], "--dump") == 0 ||
             strcmp(argv[1], "--probe") == 0 ||
             strcmp(argv[1], "--sample") == 0 ||
             strcmp(argv[1], "--mapall") == 0))
        return dump_mode(argv[1]);
    if (argc > 2 || (argc == 2 && argv[1][0] != '/')) {
        fprintf(stderr, "usage: powerd [/dev/power0] | "
                "--key [seconds] | --lid [seconds] | "
                "--dump | --probe | --sample | --mapall | "
                "--seed <chg_sid> <cap> | "
                "--read <apid> <reg> <len> ...\n");
        return -1;
    }
    if (argc == 2)
        mount = argv[1];
    /* Any one source is enough to run; each step stays idle when its
     * hardware is unavailable.  Without the charger PMIC the battery
     * protocol simply reports [0,0,0] (widget hidden), and without a
     * PON/lid the key/lid steps are no-ops. */
    bat_ok = power_pmu_init() == 0;
    key_ok = power_key_init() == 0;
    lid_ok = power_lid_init() == 0;
    if (!bat_ok && !key_ok && !lid_ok) {
        slog("q8b powerd: no battery hardware, no power key and no lid "
                "sensor; not started\n");
        return -1;
    }
    slog("q8b powerd: battery=%s key=%s lid=%s\n",
            bat_ok ? "yes" : "no", key_ok ? "yes" : "no",
            lid_ok ? "yes" : "no");

    memset(&dev, 0, sizeof(dev));
    strcpy(dev.desc, "powerd");
    dev.read = power_read;
    dev.loop_step = power_step;
    dev.cmd = power_dev_cmd;
    return device_run(&dev, mount, FS_TYPE_CHAR, 0444, false);
}