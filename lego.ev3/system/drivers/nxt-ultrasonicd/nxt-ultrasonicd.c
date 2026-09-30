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
 * Options:
 *   -p <1-4>   input port (default 1)
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

#define NXTUS_ADDR         0x01     /* 7-bit */
#define NXTUS_REG_VENDOR   0x08
#define NXTUS_REG_CMD      0x41
#define NXTUS_REG_DIST     0x42
#define NXTUS_NO_ECHO      0xFF

#define NXTUS_MAX_FAILS    3        /* consecutive I2C errors -> unplugged */
#define NXTUS_POLL_US      100000   /* sensor updates roughly every 60 ms   */

static ev3_i2c_gpio_t _bus;
static int32_t _port = EV3_IN_PORT_1;
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
    snprintf(buf, 160,
            "nxt-us port %d: connected=%d distance=%d cm mode=%d nacks=%d\n",
            _port + 1, _data.connected, _data.value[0], _mode, _bus.nacks);
    return buf;
}

/* ---------------- loop ---------------- */

static int us_loop(vdevice_t* dev, void* p) {
    (void)p;

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
        case 'p': _port = atoi(optarg) - 1; break;
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

    if (_port < 0 || _port >= EV3_IN_PORT_COUNT)
        _port = EV3_IN_PORT_1;
    if (!valid_mode(_mode))
        _mode = NXTUS_MODE_CONTINUOUS;

    ev3_gpio_init();
    if (ev3_i2c_gpio_open(&_bus, _port, EV3_I2C_GPIO_HZ_NXT) != 0) {
        printf("nxt-ultrasonicd: cannot open input port %d\n", _port + 1);
        return -1;
    }

    memset(&_data, 0, sizeof(_data));
    _data.type = EV3_SENSOR_TYPE_NXT_US;
    _data.port = _port;
    _data.mode = _mode;
    _data.nvalues = 1;
    _data.value[0] = NXTUS_NO_ECHO;

    vdevice_t dev;
    memset(&dev, 0, sizeof(vdevice_t));
    strcpy(dev.desc, "nxt-ultrasonicd");
    dev.read = us_read;
    dev.write = us_write;
    dev.dev_cntl = us_dev_cntl;
    dev.cmd = us_cmd;
    dev.loop_step = us_loop;
    device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666, false);

    ev3_i2c_gpio_close(&_bus);
    return 0;
}
