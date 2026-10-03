/*
 * touchd - EV3 / NXT touch sensor driver (input port, analog mode).
 *
 * The LEGO EV3 touch sensor is a momentary switch that pulls pin 6 high
 * when pressed; the NXT touch sensor pulls pin 1 low. This driver samples
 * the matching ADS7957 channel that adcd publishes on /dev/adc0, compares
 * it against a threshold and reports pressed/released, mirroring ev3dev's
 * ev3-analog-touch / nxt-analog-touch behaviour.
 *
 * The port is not fixed at launch: touchd scans the input ports for the ID
 * voltage of its sensor type (arch/ev3/sensor_detect.h), binds the port it
 * finds and follows the sensor when it is unplugged and re-inserted on
 * another port (hot-plug). The bound port is reported in ev3_sensor_data_t
 * .port (-1 while searching), so it is a runtime property of the device.
 *
 * Protocol (arch/ev3/sensor_dev.h), type EV3_SENSOR_TYPE_EV3_TOUCH /
 * EV3_SENSOR_TYPE_NXT_TOUCH, mode TOUCH_MODE_TOUCH:
 *   value[0] = pressed (0/1), raw_mv = pin voltage
 *
 * Options:
 *   -p <1-4>   optional: restrict detection to one input port
 *   -n         NXT touch sensor (pin 1, pressed when low)
 *   -c <ch>    override ADC channel (skips auto-detection of the channel)
 *   -t <mV>    press threshold in millivolts (default 2500)
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
#include <ewoksys/kernel_tic.h>

#include <arch/ev3/gpio.h>
#include <arch/ev3/port.h>
#include <arch/ev3/sensor_dev.h>
#include <arch/ev3/sensor_detect.h>

#define TOUCH_SCAN_MS   200   /* port (re)scan cadence while searching       */
#define TOUCH_MISS_MAX  8     /* consecutive absent scans before unbinding   */

static int32_t _want = EV3_SENSOR_TYPE_EV3_TOUCH;  /* type we look for       */
static int32_t _port = EV3_SENSOR_PORT_NONE;       /* bound port, -1 = none  */
static int32_t _channel = -1;      /* ADC channel of the bound port          */
static int32_t _channel_ovr = -1;  /* -c override                            */
static int32_t _pinned = -1;       /* -p override                            */
static int32_t _threshold = 2500;
static int32_t _nxt = 0;
static int32_t _miss = 0;
static uint32_t _scan_ms = 0;
static int _adc_fd = -1;

static ev3_sensor_data_t _data;
static bool _wakeup = false;

static void sample(void) {
    uint16_t adc[EV3_ADC_MAX_CH];
    int n = read(_adc_fd, adc, sizeof(adc));
    if (n < (int)sizeof(adc) || _channel < 0 || _channel >= EV3_ADC_MAX_CH)
        return;

    int32_t mv = adc[_channel];
    int32_t pressed = _nxt ? (mv < _threshold) : (mv >= _threshold);
    if (mv != _data.raw_mv || pressed != _data.value[0])
        _wakeup = true;
    _data.raw_mv = mv;
    _data.value[0] = pressed;
    _data.connected = 1;
    _data.timestamp_ms = (int64_t)kernel_tic_ms(0);
}

/* ---------------- port auto-detection / hot-plug ---------------- */

static void bind(int port) {
    _port = port;
    if (_channel_ovr >= 0)
        _channel = _channel_ovr;
    else
        _channel = _nxt ? ev3_input_port_adc_channel_pin1(port) :
                          ev3_input_port_adc_channel(port);
    ev3_input_port_power(port, ev3_sensor_power_level());   /* power the sensor */
    _data.port = port;
    _miss = 0;
    _wakeup = true;
}

static void unbind(void) {
    /* keep pin 1 powered so the port stays detectable for a re-insert */
    _port = EV3_SENSOR_PORT_NONE;
    _channel = -1;
    _miss = 0;
    _data.port = EV3_SENSOR_PORT_NONE;
    if (_data.connected || _data.value[0])
        _wakeup = true;
    _data.connected = 0;
    _data.value[0] = 0;
    _data.raw_mv = 0;
}

static void scan_and_bind(uint32_t now) {
    _scan_ms = now;
    if (_adc_fd < 0)
        _adc_fd = ev3_sensor_adc_open();   /* adcd may come up late */
    ev3_sensor_power_ports();
    int port;
    if (_pinned >= 0)
        port = (ev3_sensor_detect_port(_adc_fd, _pinned) == _want) ?
                _pinned : EV3_SENSOR_PORT_NONE;
    else
        port = ev3_sensor_find(_adc_fd, _want, EV3_SENSOR_PORT_NONE);
    if (port >= 0)
        bind(port);
}

/* ---------------- vdevice callbacks ---------------- */

static int touch_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        void* buf, int size, off_t offset, void* p) {
    (void)dev; (void)fd; (void)from_pid; (void)node; (void)offset; (void)p;

    if (size < (int)sizeof(_data))
        return -1;
    memcpy(buf, &_data, sizeof(_data));
    return sizeof(_data);
}

static int touch_dev_cntl(vdevice_t* dev, int from_pid, int cmd,
        proto_t* in, proto_t* ret, void* p) {
    (void)dev; (void)from_pid; (void)in; (void)p;

    if (cmd == EV3_SENSOR_CNTL_GET_DATA) {
        PF->clear(ret)->addi(ret, 0)->add(ret, &_data, sizeof(_data));
        return 0;
    }
    PF->clear(ret)->addi(ret, -1);
    return -1;
}

static char* touch_cmd(vdevice_t* dev, int from_pid, int argc, char** argv, void* p) {
    (void)dev; (void)from_pid; (void)p;
    char* buf = (char*)malloc(96);
    if (!buf) return NULL;

    if (argc > 0 && strcmp(argv[0], "scan") == 0) {
        if (_port >= 0)
            unbind();
        scan_and_bind((uint32_t)kernel_tic_ms(0));
        snprintf(buf, 96, "scan -> %s\n", _port >= 0 ? "bound" : "not found");
        return buf;
    }
    if (_port < 0)
        snprintf(buf, 96, "touchd searching type=%d (no port)\n", _want);
    else
        snprintf(buf, 96, "port %d pressed=%d mv=%d thresh=%d\n",
                _port + 1, _data.value[0], _data.raw_mv, _threshold);
    return buf;
}

static int touch_loop(vdevice_t* dev, void* p) {
    (void)p;

    uint32_t now = (uint32_t)kernel_tic_ms(0);

    if (_port >= 0) {
        sample();
        /* hot-plug: confirm the sensor is still on our port, with hysteresis
         * so a pressed NXT touch (pin 1 pulled low) is not seen as removed */
        if ((int32_t)(now - _scan_ms) >= TOUCH_SCAN_MS) {
            _scan_ms = now;
            if (ev3_sensor_detect_port(_adc_fd, _port) == _want)
                _miss = 0;
            else if (++_miss >= TOUCH_MISS_MAX)
                unbind();
        }
    } else if ((int32_t)(now - _scan_ms) >= TOUCH_SCAN_MS) {
        scan_and_bind(now);
    }

    if (_wakeup) {
        vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
        _wakeup = false;
    }
    proc_usleep(20000);   /* ~50 Hz */
    return 0;
}

static int doargs(int argc, char* argv[]) {
    int c;
    while ((c = getopt(argc, argv, "p:c:t:n")) != -1) {
        switch (c) {
        case 'p': _pinned = atoi(optarg) - 1; break;
        case 'c': _channel_ovr = atoi(optarg); break;
        case 't': _threshold = atoi(optarg); break;
        case 'n': _nxt = 1; break;
        default: break;
        }
    }
    return optind;
}

int main(int argc, char** argv) {
    int argind = doargs(argc, argv);
    const char* mnt_point = "/dev/touch0";
    if (argind < argc)
        mnt_point = argv[argind];

    if (_pinned < -1 || _pinned >= EV3_IN_PORT_COUNT)
        _pinned = -1;
    _want = _nxt ? EV3_SENSOR_TYPE_NXT_TOUCH : EV3_SENSOR_TYPE_EV3_TOUCH;

    ev3_gpio_init();

    _adc_fd = ev3_sensor_adc_open();
    if (_adc_fd < 0)
        printf("touchd: /dev/adc0 not ready; will retry while scanning\n");

    memset(&_data, 0, sizeof(_data));
    _data.type = _want;
    _data.port = EV3_SENSOR_PORT_NONE;   /* searching */
    _data.mode = TOUCH_MODE_TOUCH;
    _data.nvalues = 1;

    /* bind immediately when the sensor is already plugged in */
    scan_and_bind((uint32_t)kernel_tic_ms(0));

    vdevice_t dev;
    memset(&dev, 0, sizeof(vdevice_t));
    strcpy(dev.desc, "touchd");
    dev.read = touch_read;
    dev.dev_cntl = touch_dev_cntl;
    dev.cmd = touch_cmd;
    dev.loop_step = touch_loop;
    device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444, false);

    ev3_sensor_adc_close(_adc_fd);
    return 0;
}
