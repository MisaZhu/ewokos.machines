/*
 * touchd - EV3 / NXT touch sensor driver (input port, analog mode).
 *
 * The LEGO EV3 touch sensor is a momentary switch that pulls pin 6 high
 * when pressed; the NXT touch sensor pulls pin 1 low. This driver samples
 * the matching ADS7957 channel that adcd publishes on /dev/adc0, compares
 * it against a threshold and reports pressed/released, mirroring ev3dev's
 * ev3-analog-touch / nxt-analog-touch behaviour.
 *
 * Protocol (arch/ev3/sensor_dev.h), type EV3_SENSOR_TYPE_EV3_TOUCH /
 * EV3_SENSOR_TYPE_NXT_TOUCH, mode TOUCH_MODE_TOUCH:
 *   value[0] = pressed (0/1), raw_mv = pin voltage
 *
 * Options:
 *   -p <1-4>   input port (default 1)
 *   -n         NXT touch sensor (pin 1, pressed when low)
 *   -c <ch>    override ADC channel
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

#define ADC_MAX_CH  16
#define ADC_DEV     "/dev/adc0"

static int32_t _port = EV3_IN_PORT_1;
static int32_t _channel = -1;
static int32_t _threshold = 2500;
static int32_t _nxt = 0;
static int _adc_fd = -1;

static ev3_sensor_data_t _data;
static bool _wakeup = false;

static void sample(void) {
    uint16_t adc[ADC_MAX_CH];
    int n = read(_adc_fd, adc, sizeof(adc));
    if (n < (int)sizeof(adc))
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
    (void)dev; (void)from_pid; (void)argc; (void)argv; (void)p;
    char* buf = (char*)malloc(96);
    if (!buf) return NULL;
    snprintf(buf, 96, "port %d pressed=%d mv=%d thresh=%d\n",
            _port + 1, _data.value[0], _data.raw_mv, _threshold);
    return buf;
}

static int touch_loop(vdevice_t* dev, void* p) {
    (void)p;

    if (_adc_fd >= 0)
        sample();

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
        case 'p': _port = atoi(optarg) - 1; break;
        case 'c': _channel = atoi(optarg); break;
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

    if (_port < 0 || _port >= EV3_IN_PORT_COUNT)
        _port = EV3_IN_PORT_1;
    if (_channel < 0)
        _channel = _nxt ? ev3_input_port_adc_channel_pin1(_port) :
                          ev3_input_port_adc_channel(_port);
    if (_channel < 0 || _channel >= ADC_MAX_CH)
        return -1;

    ev3_gpio_init();
    ev3_input_port_power(_port, 1);   /* power the sensor */

    memset(&_data, 0, sizeof(_data));
    _data.type = _nxt ? EV3_SENSOR_TYPE_NXT_TOUCH : EV3_SENSOR_TYPE_EV3_TOUCH;
    _data.port = _port;
    _data.mode = TOUCH_MODE_TOUCH;
    _data.nvalues = 1;

    _adc_fd = open(ADC_DEV, O_RDONLY | O_NONBLOCK);

    vdevice_t dev;
    memset(&dev, 0, sizeof(vdevice_t));
    strcpy(dev.desc, "touchd");
    dev.read = touch_read;
    dev.dev_cntl = touch_dev_cntl;
    dev.cmd = touch_cmd;
    dev.loop_step = touch_loop;
    device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444, false);

    if (_adc_fd >= 0)
        close(_adc_fd);
    return 0;
}
