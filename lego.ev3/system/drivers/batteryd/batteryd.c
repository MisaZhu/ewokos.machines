/*
 * batteryd - EV3 battery monitor.
 *
 * Follows Linux drivers/power/supply/lego_ev3_battery.c: ADC channel 4
 * carries the divided pack voltage, channel 3 the voltage across the
 * 0.05 ohm shunt, and GPIO 136 (GP8[8], active low) tells whether the
 * Li-ion rechargeable pack is fitted.
 *
 *   voltage = adc4 * 2000 + 50000 + adc3 * 1000 / 15   [uV]
 *   current = adc3 * 20000 / 15                        [uA]
 *
 * Range used for the percentage estimate: 6xAA 5.5..7.5 V, Li-ion
 * 7.1..8.4 V (same numbers as the Linux driver).
 *
 * Protocol (arch/ev3/battery_dev.h):
 *   read(fd, &battery_info_t, sizeof) / dev_cntl(BATTERY_CNTL_GET)
 *   write(fd, &battery_cmd_t, sizeof) / dev_cntl(BATTERY_CNTL_SET_THRESH)
 *
 * Options:
 *   -l <mv>    low-battery threshold in millivolts (default 6200 / 7300)
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <ewoksys/vfs.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/proto.h>
#include <ewoksys/proc.h>
#include <ewoksys/ipc.h>
#include <ewoksys/mmio.h>

#include <arch/ev3/gpio.h>
#include <arch/ev3/battery_dev.h>

#define ADC_MAX_CH        16
#define ADC_DEV           "/dev/adc0"
#define ADC_CH_VOLTAGE    4
#define ADC_CH_CURRENT    3
#define PIN_RECHARGEABLE  136     /* GP8[8], low = Li-ion pack */

/* lego_ev3_battery.c voltage_min/max_design, mV */
#define AA_EMPTY_MV       5500
#define AA_FULL_MV        7500
#define LIION_EMPTY_MV    7100
#define LIION_FULL_MV     8400

static int32_t _low_mv = 0;          /* 0 = pick default from pack type */
static int _adc_fd = -1;

static battery_info_t _info;
static bool _wakeup = false;

static int32_t clamp(int32_t v, int32_t lo, int32_t hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void measure(void) {
    uint16_t adc[ADC_MAX_CH];
    int n = read(_adc_fd, adc, sizeof(adc));
    if (n < (int)sizeof(adc))
        return;

    battery_info_t bi;
    memset(&bi, 0, sizeof(bi));

    bi.adc_voltage_mv = adc[ADC_CH_VOLTAGE];
    bi.adc_current_mv = adc[ADC_CH_CURRENT];
    bi.rechargeable = ev3_gpio_read(PIN_RECHARGEABLE) ? 0 : 1;

    /* battery voltage is adc * 2 + Vce of the transistor, plus the drop
     * over the shunt; current is shunt voltage / 15 / 0.05 ohm */
    int32_t uv = bi.adc_voltage_mv * 2000 + 50000 + bi.adc_current_mv * 1000 / 15;
    int32_t ua = bi.adc_current_mv * 20000 / 15;
    bi.voltage_mv = uv / 1000;
    bi.current_ma = ua / 1000;

    int32_t empty = bi.rechargeable ? LIION_EMPTY_MV : AA_EMPTY_MV;
    int32_t full  = bi.rechargeable ? LIION_FULL_MV  : AA_FULL_MV;
    bi.percent = clamp((bi.voltage_mv - empty) * 100 / (full - empty), 0, 100);

    bi.low_threshold_mv = _low_mv > 0 ? _low_mv :
            (bi.rechargeable ? LIION_EMPTY_MV + 200 : AA_EMPTY_MV + 700);
    bi.low = (bi.voltage_mv > 0 && bi.voltage_mv < bi.low_threshold_mv) ? 1 : 0;

    if (memcmp(&bi, &_info, sizeof(bi)) != 0)
        _wakeup = true;
    _info = bi;
}

static int32_t do_command(const battery_cmd_t* c) {
    switch (c->cmd) {
    case BATTERY_CMD_SET_THRESH:
        _low_mv = c->value < 1000 ? 0 : c->value;
        return 0;
    default:
        return -1;
    }
}

/* ---------------- vdevice callbacks ---------------- */

static int batt_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        void* buf, int size, off_t offset, void* p) {
    (void)dev; (void)fd; (void)from_pid; (void)node; (void)offset; (void)p;

    if (size > (int)sizeof(_info))
        size = sizeof(_info);
    memcpy(buf, &_info, size);
    return size;
}

static int batt_write(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        const void* buf, int size, off_t offset, void* p) {
    (void)dev; (void)fd; (void)from_pid; (void)node; (void)offset; (void)p;

    if (buf == NULL || size < (int)sizeof(battery_cmd_t))
        return -1;
    if (do_command((const battery_cmd_t*)buf) != 0)
        return -1;
    return sizeof(battery_cmd_t);
}

static int batt_dev_cntl(vdevice_t* dev, int from_pid, int cmd,
        proto_t* in, proto_t* ret, void* p) {
    (void)dev; (void)from_pid; (void)p;

    switch (cmd) {
    case BATTERY_CNTL_GET:
        PF->clear(ret)->addi(ret, 0)->add(ret, &_info, sizeof(_info));
        return 0;
    case BATTERY_CNTL_SET_THRESH: {
        battery_cmd_t c;
        memset(&c, 0, sizeof(c));
        if (proto_read_to(in, &c, sizeof(c)) != (int32_t)sizeof(c)) {
            PF->clear(ret)->addi(ret, -1);
            return -1;
        }
        int32_t res = do_command(&c);
        PF->clear(ret)->addi(ret, res);
        return res;
    }
    default:
        PF->clear(ret)->addi(ret, -1);
        return -1;
    }
}

/* ---------------- text interface (shell debugging) ---------------- */

static char* batt_cmd(vdevice_t* dev, int from_pid, int argc, char** argv, void* p) {
    (void)dev; (void)from_pid; (void)p;
    char* buf = (char*)malloc(200);
    if (!buf) return NULL;

    if (argc > 0 && strcmp(argv[0], "thresh") == 0 && argc > 1) {
        battery_cmd_t c = { BATTERY_CMD_SET_THRESH, atoi(argv[1]) };
        do_command(&c);
        snprintf(buf, 200, "low threshold = %d mV\n", _low_mv);
        return buf;
    }
    snprintf(buf, 200,
            "battery: %d mV %d mA %d%% low=%d liion=%d (thresh=%d mV, adc v=%d i=%d)\n",
            _info.voltage_mv, _info.current_ma, _info.percent, _info.low,
            _info.rechargeable, _info.low_threshold_mv,
            _info.adc_voltage_mv, _info.adc_current_mv);
    return buf;
}

static int batt_loop(vdevice_t* dev, void* p) {
    (void)p;

    if (_adc_fd >= 0)
        measure();

    if (_wakeup) {
        vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
        _wakeup = false;
    }
    proc_usleep(200000);   /* ~5 Hz is plenty for a battery gauge */
    return 0;
}

static int doargs(int argc, char* argv[]) {
    int c;
    while ((c = getopt(argc, argv, "l:")) != -1) {
        switch (c) {
        case 'l': _low_mv = atoi(optarg); break;
        default: break;
        }
    }
    return optind;
}

int main(int argc, char** argv) {
    int argind = doargs(argc, argv);
    const char* mnt_point = "/dev/battery";
    if (argind < argc)
        mnt_point = argv[argind];

    ev3_gpio_init();
    ev3_gpio_config(PIN_RECHARGEABLE, GPIO_INPUT);

    memset(&_info, 0, sizeof(_info));
    _adc_fd = open(ADC_DEV, O_RDONLY | O_NONBLOCK);

    vdevice_t dev;
    memset(&dev, 0, sizeof(vdevice_t));
    strcpy(dev.desc, "batteryd");
    dev.read = batt_read;
    dev.write = batt_write;
    dev.dev_cntl = batt_dev_cntl;
    dev.cmd = batt_cmd;
    dev.loop_step = batt_loop;
    device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666, false);

    if (_adc_fd >= 0)
        close(_adc_fd);
    return 0;
}
