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
 * second on the same tick.  Sleep here is panel-off plus a paused
 * compositor/input path - this WoA firmware has no suspend-to-RAM door,
 * so the SoC keeps running while the screen is dark.  Both wake sources
 * are polled independently of xserverd, so they always wake; they share
 * one sleep state, so a power-key press can wake a lid-closed device
 * and only a real lid transition changes the state afterwards.
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

/* Lid (hall sensor) state machine.  _lid_closed is the last debounced
 * state; a level that differs from it must persist for LID_DEBOUNCE_MS
 * (_lid_pending/_lid_pending_ms) before it commits.  The first valid
 * read arms the machine and applies the boot state, so a device started
 * with the keyboard closed comes up asleep. */
static bool _lid_armed;
static bool _lid_closed;
static bool _lid_pending;
static uint64_t _lid_pending_ms;

/* Drive the display sleep state through xserverd's "sleep"/"wake" dev
 * commands.  xserverd blanks the panel and pauses compositing/input on
 * "sleep", and repaints everything on "wake".  powerd keeps its own
 * state so the toggle still works when the X server is not up (headless
 * boot) - the command is best-effort.  Shared by the power key and the
 * lid so the two sources never fight over _sleeping. */
static void power_set_sleep(bool sleep, const char* why) {
    const char* cmd = sleep ? "sleep" : "wake";
    char* ret;

    if (sleep == _sleeping)
        return;
    ret = dev_cmd("/dev/x", cmd);
    _sleeping = sleep;
    if (ret == NULL)
        slog("q8b powerd: %s %s: no reply from /dev/x\n", why, cmd);
    else
        free(ret);
    slog("q8b powerd: %s -> %s\n", why, cmd);
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
    if (off > 0 && (size_t)off < sizeof(buf))
        snprintf(buf + off, sizeof(buf) - (size_t)off,
                "key=%s lid=%s sleeping=%s\n",
                k < 0 ? "none" : (k ? "down" : "up"),
                lid < 0 ? "none" : (lid ? "closed" : "open"),
                _sleeping ? "yes" : "no");
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