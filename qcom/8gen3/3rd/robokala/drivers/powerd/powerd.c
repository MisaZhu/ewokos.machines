/*
 * powerd.c - battery status device for the Robo & Kala TW220
 * (Qualcomm SC8280XP).
 *
 * Serves /dev/power0 with the same 3-byte protocol as the uConsole powerd,
 * consumed by statusbar's PowerInfo widget:
 *   byte0 = battery present, byte1 = charging, byte2 = capacity percent.
 *
 * The hardware side lives in power_pmu.c: SMB5 charger status plus an ADC7
 * VBAT_SNS voltage sample converted with the uConsole voltage curve.  If the
 * board turns out to have no charger PMIC (EC-managed battery) the device
 * still starts and simply reports [0,0,0], which hides the battery widget.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ewoksys/klog.h>
#include <ewoksys/vdevice.h>

#include "power_pmu.h"

#define SAMPLE_PERIOD_US 1000000U

static power_sample_t _sample;

static int power_step(vdevice_t* dev, void* p) {
    (void)dev;
    (void)p;
    (void)power_pmu_sample(&_sample);
    usleep(SAMPLE_PERIOD_US);
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
        "  help     show this help\n"
        "  status   re-sample and show battery/charging/capacity/voltage\n");
}

static char* power_status(void) {
    char buf[224];

    /* A status query re-samples so the reply reflects the live PMIC. */
    (void)power_pmu_sample(&_sample);
    if (_sample.vbat_valid) {
        snprintf(buf, sizeof(buf),
                "battery=%s charging=%s capacity=%u%% "
                "voltage=%d.%03dV chg_state=%u batif=%02x "
                "sid(chg=%d vbat=%d)\n",
                _sample.battery ? "yes" : "no",
                _sample.charging ? "yes" : "no",
                (unsigned)_sample.capacity,
                _sample.voltage_mv / 1000, _sample.voltage_mv % 1000,
                (unsigned)_sample.chgr_state, (unsigned)_sample.batif_sts,
                _sample.charger_sid, _sample.vbat_sid);
    } else {
        snprintf(buf, sizeof(buf),
                "battery=%s charging=%s capacity=unknown "
                "chg_state=%u batif=%02x sid(chg=%d vbat=%d)\n",
                _sample.battery ? "yes" : "no",
                _sample.charging ? "yes" : "no",
                (unsigned)_sample.chgr_state, (unsigned)_sample.batif_sts,
                _sample.charger_sid, _sample.vbat_sid);
    }
    return dup_text(buf);
}

static char* power_dev_cmd(vdevice_t* dev, int from_pid, int argc,
        char** argv, void* p) {
    (void)dev;
    (void)from_pid;
    (void)p;

    if (argc <= 0 || argv == NULL || argv[0] == NULL)
        return NULL;
    if (strcmp(argv[0], "help") == 0)
        return power_help();
    if (strcmp(argv[0], "status") == 0)
        return power_status();
    return NULL;
}

/* Bring-up diagnostics.  Each stage prints directly to stdout so a hang
 * leaves the last completed stage visible on the wire.  Run from a shell:
 *   /drivers/8gen3/powerd --dump    map summary (safe core-register reads)
 *   /drivers/8gen3/powerd --probe   + observer-read sweep
 *   /drivers/8gen3/powerd --adc     + raw VBAT_SNS conversion attempts
 *   /drivers/8gen3/powerd --sample  + one live sample
 *   /drivers/8gen3/powerd --mapall  full APID map listing
 *   /drivers/8gen3/powerd --read <apid> <reg> <len> ...
 *                                   raw observer reads (read-only)
 *   /drivers/8gen3/powerd --write <apid> <reg> <val>
 *                                   one gated RW-window write (pre/post
 *                                   reads; pass the pre-read value back
 *                                   for a no-op write-back probe).
 *                                   WARNING: proven to wedge the TW220
 *                                   SPMI bus for FG/ADC battery channels -
 *                                   do not use except for diagnostics. */
static int dump_mode(const char* stage) {
    printf("powerd: SPMI init...\n");
    power_pmu_verbose(true);
    if (power_pmu_init() != 0) {
        printf("powerd: SPMI init failed\n");
        return -1;
    }
    power_pmu_dump_map();
    if (strcmp(stage, "--probe") == 0 || strcmp(stage, "--adc") == 0)
        power_pmu_dump_probe();
    if (strcmp(stage, "--adc") == 0)
        power_pmu_dump_adc();
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

/* --write <apid> <reg> <val> : one RW-window write with pre/post reads
 * and the kernel's EE0-ownership gate applied inside spmi_write.
 * Diagnostic only: writes to battery PMIC channels (FG/ADC) are proven
 * to wedge the TW220 SPMI bus and require a reboot to recover. */
static int write_mode(int n, const char* const* args) {
    if (n != 3) {
        fprintf(stderr, "usage: powerd --write <apid> <reg> <val>\n");
        return -1;
    }
    printf("powerd: SPMI init...\n");
    power_pmu_verbose(true);
    if (power_pmu_init() != 0) {
        printf("powerd: SPMI init failed\n");
        return -1;
    }
    return power_pmu_write_args(n, args);
}

int main(int argc, char** argv) {
    const char* mount = "/dev/power0";

    if (argc > 1 && strcmp(argv[1], "--read") == 0)
        return read_mode(argc - 2, (const char* const*)&argv[2]);
    if (argc > 1 && strcmp(argv[1], "--write") == 0)
        return write_mode(argc - 2, (const char* const*)&argv[2]);
    if (argc > 1 &&
            (strcmp(argv[1], "--dump") == 0 ||
             strcmp(argv[1], "--probe") == 0 ||
             strcmp(argv[1], "--adc") == 0 ||
             strcmp(argv[1], "--sample") == 0 ||
             strcmp(argv[1], "--mapall") == 0))
        return dump_mode(argv[1]);
    if (argc > 2 || (argc == 2 && argv[1][0] != '/')) {
        fprintf(stderr, "usage: powerd [/dev/power0] | "
                "--dump | --probe | --adc | --sample | --mapall | "
                "--read <apid> <reg> <len> ... | "
                "--write <apid> <reg> <val>\n");
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
