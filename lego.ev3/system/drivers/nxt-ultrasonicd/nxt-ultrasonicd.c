/*
 * nxt-ultrasonicd - LEGO NXT ultrasonic sensor (I2C, input port).
 *
 * The NXT ultrasonic is the older I2C-based rangefinder (the EV3 one
 * speaks the EV3 UART protocol, see ultrasonicd). It answers at 7-bit
 * address 0x01 (0x02 in the 8-bit notation used by LEGO documentation)
 * on the port's pin 5/6 GPIO I2C bus at 9.6 kHz, using the NXT-specific
 * STOP / extra clock / START register read sequence that
 * ev3_i2c_gpio_read_reg() implements.
 *
 * Register map (ev3dev nxt-i2c-sensor / NXT DDK):
 *   0x00  firmware version "V1.0"     0x08  vendor "LEGO"
 *   0x10  product "Sonar"
 *   0x41  command: 0 off, 1 single shot, 2 continuous, 3 reset
 *   0x42  distance #1 in cm (0xFF = no echo), 0x43..0x49 echoes #2..#8
 *
 * Protocol (arch/ev3/sensor_dev.h), type EV3_SENSOR_TYPE_NXT_US:
 *   read(fd, &ev3_sensor_data_t, sizeof)     value[0] = cm (255 = none)
 *   write(fd, &ev3_sensor_cmd_t, sizeof)     SET_MODE arg0 = NXTUS_MODE_*
 *   dev_cntl(EV3_SENSOR_CNTL_GET_DATA / SET_MODE / COMMAND)
 *
 * The port is not fixed at launch: the daemon scans the input ports for the
 * NXT ultrasonic ID voltage (arch/ev3/sensor_detect.h), binds the port it
 * finds and follows the sensor on hot-plug. The bound port is reported in
 * ev3_sensor_data_t.port (-1 while searching).
 *
 * Options:
 *   -p <1-4>   optional: restrict detection to one input port
 *   -m <mode>  initial mode (default 2 = continuous)
 *   [mount]    mount point (default /dev/nxt-us0)
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
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
#include <arch/ev3/i2c.h>
#include <arch/ev3/sensor_dev.h>
#include <arch/ev3/sensor_detect.h>

#define NXTUS_ADDR         0x01     /* 7-bit */
#define NXTUS_REG_VENDOR   0x08
#define NXTUS_REG_CMD      0x41
#define NXTUS_REG_DIST     0x42
#define NXTUS_NO_ECHO      0xFF

#define NXTUS_MAX_FAILS    3        /* consecutive I2C errors -> unplugged */
#define NXTUS_POLL_US      100000   /* sensor updates roughly every 60 ms   */
#define NXTUS_SCAN_MS      300      /* port (re)scan cadence while searching */
#define NXTUS_MISS_MAX     4        /* absent scans before re-binding a port */

static ev3_i2c_gpio_t _bus;
static int32_t _want = EV3_SENSOR_TYPE_NXT_US;   /* ID type we look for      */
static int32_t _port = EV3_SENSOR_PORT_NONE;     /* bound port, -1 = none    */
static int32_t _pinned = -1;      /* -p override: restrict to one port       */
static int     _adc_fd = -1;
static int32_t _bus_open = 0;     /* _bus currently opened on _port          */
static int32_t _miss = 0;         /* consecutive absent port scans           */
static uint32_t _scan_ms = 0;
static int32_t _mode = NXTUS_MODE_CONTINUOUS;
static int32_t _pending_mode = -1;  /* mode requested by a client, applied in loop */

static ev3_sensor_data_t _data;
static int32_t _fails = 0;
static bool _wakeup = false;

static int32_t us_write_reg(uint8_t reg, uint8_t val) {
    uint8_t b[2] = { reg, val };
    return ev3_i2c_gpio_write(&_bus, NXTUS_ADDR, b, 2);
}

static int32_t us_read_reg(uint8_t reg, uint8_t* buf, int32_t len) {
    return ev3_i2c_gpio_read_reg(&_bus, NXTUS_ADDR, reg, buf, len);
}

static bool valid_mode(int32_t m) {
    return m == NXTUS_MODE_OFF || m == NXTUS_MODE_SINGLE ||
           m == NXTUS_MODE_CONTINUOUS;
}

static void set_value(int32_t connected, int32_t cm) {
    if (_data.connected != connected || _data.value[0] != cm ||
        _data.mode != _mode)
        _wakeup = true;
    _data.connected = connected;
    _data.mode = _mode;
    _data.nvalues = 1;
    _data.value[0] = cm;
    _data.timestamp_ms = (int64_t)kernel_tic_ms(0);
}

/* Vendor string "LEGO" at 0x08. Clones may return junk; accept any ACK. */
static bool detect_sensor(void) {
    uint8_t id[4];
    return us_read_reg(NXTUS_REG_VENDOR, id, 4) == 0;
}

/* ---------------- port auto-detection / hot-plug ---------------- */

static void bind(int port) {
    if (ev3_i2c_gpio_open(&_bus, port, EV3_I2C_GPIO_HZ_NXT) != 0)
        return;
    _bus_open = 1;
    _port = port;
    _data.port = port;
    _fails = 0;
    _miss = 0;
    _wakeup = true;
    /* (re)arm the current mode on the freshly bound sensor */
    us_write_reg(NXTUS_REG_CMD, (uint8_t)_mode);
}

static void unbind(void) {
    if (_bus_open) {
        ev3_i2c_gpio_close(&_bus);
        _bus_open = 0;
    }
    _port = EV3_SENSOR_PORT_NONE;
    _data.port = EV3_SENSOR_PORT_NONE;
    _fails = 0;
    _miss = 0;
    set_value(0, NXTUS_NO_ECHO);
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

static int32_t do_command(const ev3_sensor_cmd_t* c) {
    switch (c->cmd) {
    case EV3_SENSOR_CMD_SET_MODE:
        if (!valid_mode(c->arg0))
            return -1;
        _pending_mode = c->arg0;
        return 0;
    case EV3_SENSOR_CMD_RESET:
        _pending_mode = 3;      /* warm reset, loop restores _mode after */
        return 0;
    default:
        return -1;
    }
}

/* ---------------- vdevice callbacks ---------------- */

static int us_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        void* buf, int size, off_t offset, void* p) {
    (void)dev; (void)fd; (void)from_pid; (void)node; (void)offset; (void)p;

    if (size < (int)sizeof(_data))
        return -1;
    memcpy(buf, &_data, sizeof(_data));
    return sizeof(_data);
}

static int us_write(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        const void* buf, int size, off_t offset, void* p) {
    (void)dev; (void)fd; (void)from_pid; (void)node; (void)offset; (void)p;

    if (buf == NULL || size < (int)sizeof(ev3_sensor_cmd_t))
        return -1;
    if (do_command((const ev3_sensor_cmd_t*)buf) != 0)
        return -1;
    return sizeof(ev3_sensor_cmd_t);
}

static int us_dev_cntl(vdevice_t* dev, int from_pid, int cmd,
        proto_t* in, proto_t* ret, void* p) {
    (void)dev; (void)from_pid; (void)p;

    switch (cmd) {
    case EV3_SENSOR_CNTL_GET_DATA:
        PF->clear(ret)->addi(ret, 0)->add(ret, &_data, sizeof(_data));
        return 0;
    case EV3_SENSOR_CNTL_SET_MODE:
    case EV3_SENSOR_CNTL_COMMAND: {
        ev3_sensor_cmd_t c;
        memset(&c, 0, sizeof(c));
        if (proto_read_to(in, &c, sizeof(c)) != (int32_t)sizeof(c)) {
            PF->clear(ret)->addi(ret, -1);
            return -1;
        }
        if (cmd == EV3_SENSOR_CNTL_SET_MODE)
            c.cmd = EV3_SENSOR_CMD_SET_MODE;
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

static char* us_cmd(vdevice_t* dev, int from_pid, int argc, char** argv, void* p) {
    (void)dev; (void)from_pid; (void)p;
    char* buf = (char*)malloc(160);
    if (!buf) return NULL;

    if (argc > 0 && strcmp(argv[0], "mode") == 0 && argc > 1) {
        ev3_sensor_cmd_t c = { EV3_SENSOR_CMD_SET_MODE, atoi(argv[1]), 0, 0 };
        snprintf(buf, 160, "mode %d -> %s\n", c.arg0,
                do_command(&c) == 0 ? "ok" : "error (0/1/2)");
        return buf;
    }
    if (argc > 0 && strcmp(argv[0], "scan") == 0) {
        if (_port >= 0)
            unbind();
        scan_and_bind((uint32_t)kernel_tic_ms(0));
        snprintf(buf, 160, "scan -> %s\n", _port >= 0 ? "bound" : "not found");
        return buf;
    }
    if (_port < 0) {
        snprintf(buf, 160, "nxt-us searching type=%d (no port)\n", _want);
        return buf;
    }
    snprintf(buf, 160,
            "nxt-us port %d: connected=%d distance=%d cm mode=%d nacks=%d\n",
            _port + 1, _data.connected, _data.value[0], _mode, _bus.nacks);
    return buf;
}

/* ---------------- loop ---------------- */

static int us_loop(vdevice_t* dev, void* p) {
    (void)p;

    uint32_t now = (uint32_t)kernel_tic_ms(0);

    if (_port < 0) {
        /* searching: scan the ports for an NXT ultrasonic ID */
        if ((int32_t)(now - _scan_ms) >= NXTUS_SCAN_MS)
            scan_and_bind(now);
    } else {
        ipc_disable();

        if (!_data.connected) {
            if (detect_sensor()) {
                _fails = 0;
                us_write_reg(NXTUS_REG_CMD, (uint8_t)_mode);
                set_value(1, NXTUS_NO_ECHO);
            }
        } else {
            if (_pending_mode >= 0) {
                int32_t m = _pending_mode;
                _pending_mode = -1;
                if (us_write_reg(NXTUS_REG_CMD, (uint8_t)m) == 0) {
                    if (valid_mode(m))
                        _mode = m;
                    else
                        _pending_mode = _mode;   /* reset: re-arm current mode */
                }
            }

            if (_mode != NXTUS_MODE_OFF) {
                uint8_t raw = NXTUS_NO_ECHO;
                if (us_read_reg(NXTUS_REG_DIST, &raw, 1) == 0) {
                    _fails = 0;
                    set_value(1, raw);
                    if (_mode == NXTUS_MODE_SINGLE)
                        us_write_reg(NXTUS_REG_CMD, NXTUS_MODE_SINGLE);
                } else if (++_fails >= NXTUS_MAX_FAILS) {
                    set_value(0, NXTUS_NO_ECHO);
                }
            }
        }

        ipc_enable();

        /* hot-plug: if the sensor is no longer on our port, release it and go
         * back to searching so we follow it wherever it is re-inserted */
        if ((int32_t)(now - _scan_ms) >= NXTUS_SCAN_MS) {
            _scan_ms = now;
            if (ev3_sensor_detect_port(_adc_fd, _port) == _want)
                _miss = 0;
            else if (++_miss >= NXTUS_MISS_MAX)
                unbind();
        }
    }

    if (_wakeup) {
        vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
        _wakeup = false;
    }
    proc_usleep(NXTUS_POLL_US);
    return 0;
}

static int doargs(int argc, char* argv[]) {
    int c;
    while ((c = getopt(argc, argv, "p:m:")) != -1) {
        switch (c) {
        case 'p': _pinned = atoi(optarg) - 1; break;   /* optional restriction */
        case 'm': _mode = atoi(optarg); break;
        default: break;
        }
    }
    return optind;
}

int main(int argc, char** argv) {
    int argind = doargs(argc, argv);
    const char* mnt_point = "/dev/nxt-us0";
    if (argind < argc)
        mnt_point = argv[argind];

    if (_pinned < -1 || _pinned >= EV3_IN_PORT_COUNT)
        _pinned = -1;
    if (!valid_mode(_mode))
        _mode = NXTUS_MODE_CONTINUOUS;

    ev3_gpio_init();

    _adc_fd = ev3_sensor_adc_open();
    if (_adc_fd < 0)
        printf("nxt-ultrasonicd: /dev/adc0 not ready; ADC detection off, will retry\n");

    memset(&_data, 0, sizeof(_data));
    _data.type = EV3_SENSOR_TYPE_NXT_US;
    _data.port = EV3_SENSOR_PORT_NONE;   /* searching */
    _data.mode = _mode;
    _data.nvalues = 1;
    _data.value[0] = NXTUS_NO_ECHO;

    /* bind right away when the sensor is already plugged in */
    scan_and_bind((uint32_t)kernel_tic_ms(0));

    vdevice_t dev;
    memset(&dev, 0, sizeof(vdevice_t));
    strcpy(dev.desc, "nxt-ultrasonicd");
    dev.read = us_read;
    dev.write = us_write;
    dev.dev_cntl = us_dev_cntl;
    dev.cmd = us_cmd;
    dev.loop_step = us_loop;
    device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666, false);

    if (_bus_open)
        ev3_i2c_gpio_close(&_bus);
    ev3_sensor_adc_close(_adc_fd);
    return 0;
}
