/*
 * powerd.c - battery status device for the Robo & Kala TW220
 * (Qualcomm SC8280XP).
 *
 * Serves /dev/power0 with the same 3-byte protocol as the uConsole powerd,
 * consumed by statusbar's PowerInfo widget:
 *   byte0 = battery present, byte1 = charging, byte2 = capacity percent.
 *
 * The hardware side lives in power_pmu.c: read-only SMB5 charger status
 * and fuel-gauge registers.  Real capacity on this Windows-on-ARM laptop
 * comes from the ADSP firmware battery manager (PMIC GLINK battmgr, see
 * battmgr_wire.h) once the ADSP bring-up lands.  If the board turns out to
 * have no charger PMIC (EC-managed battery) the device still starts and
 * simply reports [0,0,0], which hides the battery widget.
 *
 * powerd also owns the PMIC PON power key (read-only KPDPWR status, the
 * same signal Linux's pm8941-pwrkey reports): a short press toggles
 * display sleep/wake through xserverd's "sleep"/"wake" dev commands, and
 * holding the key for 3 seconds requests PSCI SYSTEM_OFF.  Sleep here is
 * panel-off plus a paused compositor/input path - this WoA firmware has
 * no suspend-to-RAM door, so the SoC keeps running while the screen is
 * dark; the key is polled independently of xserverd, so it always wakes.
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

#include "power_pmu.h"

/* The loop ticks at the key poll cadence; the battery sample keeps its
 * old 1 s period by running every SAMPLE_TICKS-th tick.  50 ms gives the
 * 3 s long-press measurement ~1.7% resolution and a short press is
 * recognized within one tick of the release. */
#define KEY_POLL_US        50000U
#define SAMPLE_TICKS       20U
#define KEY_LONG_PRESS_MS  3000U
/* Releases shorter than this are glitches, not presses (the PON hardware
 * debounce is ~15 ms; this is the software guard on top of it). */
#define KEY_MIN_PRESS_MS   100U

static power_sample_t _sample;
static uint32_t _sample_tick;

/* Power-key state machine.  _key_ignore discards a press that was already
 * down when powerd started (the boot press) and re-arms on release; it
 * also latches after a long press whose SYSTEM_OFF unexpectedly returned,
 * so a failed off cannot re-fire every tick. */
static bool _key_armed;
static bool _key_down;
static bool _key_ignore;
static uint64_t _key_down_ms;
static bool _sleeping;

static char* power_shutdown(void);

/* Short press: toggle display sleep.  xserverd blanks the panel and
 * pauses compositing/input on "sleep", and repaints everything on
 * "wake".  powerd keeps its own state so the toggle still works when the
 * X server is not up (headless boot) - the command is best-effort. */
static void power_key_sleep_toggle(void) {
    const char* cmd = _sleeping ? "wake" : "sleep";
    char* ret = dev_cmd("/dev/x", cmd);

    _sleeping = !_sleeping;
    if (ret == NULL)
        slog("q8b powerd: power key %s: no reply from /dev/x\n", cmd);
    else
        free(ret);
    slog("q8b powerd: power key short press -> %s\n",
            _sleeping ? "sleep" : "wake");
}

static int power_step(vdevice_t* dev, void* p) {
    (void)dev;
    (void)p;

    int ks = power_pmu_key_state();
    if (ks >= 0) {
        uint64_t now = kernel_tic_ms(0);
        bool down = (ks != 0);

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
            char* ret = power_shutdown();
            if (ret != NULL) {
                slog("q8b powerd: %s", ret);
                free(ret);
            }
        }
    }

    if (++_sample_tick >= SAMPLE_TICKS) {
        _sample_tick = 0;
        (void)power_pmu_sample(&_sample);
    }
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
        "  status    re-sample and show battery/charging/capacity/voltage/key\n"
        "  reboot    request PSCI SYSTEM_RESET\n"
        "  shutdown  request PSCI SYSTEM_OFF\n"
        "power key: short press toggles display sleep/wake,\n"
        "           hold 3s requests PSCI SYSTEM_OFF\n");
}

static char* power_status(void) {
    char buf[256];
    const char* src;

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
        snprintf(buf, sizeof(buf),
                "battery=%s charging=%s capacity=%u%% (%s) "
                "voltage=%d.%03dV chg_state=%u batif=%02x "
                "sid(chg=%d vbat=%d)",
                _sample.battery ? "yes" : "no",
                _sample.charging ? "yes" : "no",
                (unsigned)_sample.capacity, src,
                _sample.voltage_mv / 1000, _sample.voltage_mv % 1000,
                (unsigned)_sample.chgr_state, (unsigned)_sample.batif_sts,
                _sample.charger_sid, _sample.vbat_sid);
    } else if (_sample.capacity > 0) {
        /* Charger-derived or remembered capacity with no voltage
         * reading: the TW220's fuel gauge boots dormant. */
        snprintf(buf, sizeof(buf),
                "battery=%s charging=%s capacity=%u%% (%s) "
                "chg_state=%u batif=%02x sid(chg=%d vbat=%d)",
                _sample.battery ? "yes" : "no",
                _sample.charging ? "yes" : "no",
                (unsigned)_sample.capacity, src,
                (unsigned)_sample.chgr_state, (unsigned)_sample.batif_sts,
                _sample.charger_sid, _sample.vbat_sid);
    } else {
        snprintf(buf, sizeof(buf),
                "battery=%s charging=%s capacity=unknown "
                "chg_state=%u batif=%02x sid(chg=%d vbat=%d)",
                _sample.battery ? "yes" : "no",
                _sample.charging ? "yes" : "no",
                (unsigned)_sample.chgr_state, (unsigned)_sample.batif_sts,
                _sample.charger_sid, _sample.vbat_sid);
    }
    {
        size_t len = strlen(buf);
        int k = power_pmu_key_state();
        snprintf(buf + len, sizeof(buf) - len, " key=%s sleeping=%s\n",
                k < 0 ? "none" : (k ? "down" : "up"),
                _sleeping ? "yes" : "no");
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
    /* The long-press path cuts power with no further warning: flush the
     * persistent state file (and anything else dirty) before PSCI off. */
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
 *   /drivers/8gen3/powerd --dump    map summary (safe core-register reads)
 *   /drivers/8gen3/powerd --probe   + observer-read sweep
 *   /drivers/8gen3/powerd --sample  + one live sample
 *   /drivers/8gen3/powerd --mapall  full APID map listing
 *   /drivers/8gen3/powerd --seed <chg_sid> <cap>
 *                                   bootstrap /etc/powerd.state (no SPMI
 *                                   access; run once at deployment)
 *   /drivers/8gen3/powerd --read <apid> <reg> <len> ...
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
            fprintf(stderr, "usage: powerd --key [seconds (1..3600)]\n");
            return -1;
        }
        secs = (uint32_t)v;
    }
    power_pmu_verbose(true);
    if (power_pmu_init() != 0) {
        printf("powerd: SPMI init failed\n");
        return -1;
    }
    if (power_pmu_key_info(&apid, &sid, &subtype) != 0) {
        printf("key: no usable PON power key found\n");
        return -1;
    }
    printf("key: pon apid=%d sid=%d subtype=%d, monitoring %u s\n",
            apid, sid, subtype, (unsigned)secs);
    end = kernel_tic_ms(0) + (uint64_t)secs * 1000U;
    while (kernel_tic_ms(0) < end) {
        int ks = power_pmu_key_state();
        if (ks != last) {
            printf("key: %s\n",
                    ks < 0 ? "unknown" : (ks ? "down" : "up"));
            last = ks;
        }
        usleep(KEY_POLL_US);
    }
    return 0;
}

int main(int argc, char** argv) {
    const char* mount = "/dev/power0";

    if (argc > 1 && strcmp(argv[1], "--read") == 0)
        return read_mode(argc - 2, (const char* const*)&argv[2]);
    if (argc > 1 && strcmp(argv[1], "--seed") == 0)
        return power_pmu_seed_args(argc - 2, (const char* const*)&argv[2]);
    if (argc > 1 && strcmp(argv[1], "--key") == 0)
        return key_mode(argc - 2, &argv[2]);
    if (argc > 1 &&
            (strcmp(argv[1], "--dump") == 0 ||
             strcmp(argv[1], "--probe") == 0 ||
             strcmp(argv[1], "--sample") == 0 ||
             strcmp(argv[1], "--mapall") == 0))
        return dump_mode(argv[1]);
    if (argc > 2 || (argc == 2 && argv[1][0] != '/')) {
        fprintf(stderr, "usage: powerd [/dev/power0] | "
                "--dump | --probe | --sample | --mapall | "
                "--key [seconds] | "
                "--seed <chg_sid> <cap> | "
                "--read <apid> <reg> <len> ...\n");
        return -1;
    }
    if (argc == 2)
        mount = argv[1];
    if (power_pmu_init() != 0) {
        slog("q8b powerd: battery hardware unreachable; "
                "/dev/power0 not started\n");
        return -1;
    }

    vdevice_t dev;
    memset(&dev, 0, sizeof(dev));
    strcpy(dev.desc, "powerd");
    dev.read = power_read;
    dev.loop_step = power_step;
    dev.cmd = power_dev_cmd;
    return device_run(&dev, mount, FS_TYPE_CHAR, 0444, false);
}
