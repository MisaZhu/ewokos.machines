/*
 * Generic EV3 UART sensor daemon - see include/arch/ev3/uart_sensord.h.
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

#include "../include/arch/ev3/gpio.h"
#include "../include/arch/ev3/port.h"
#include "../include/arch/ev3/uart_sensor.h"
#include "../include/arch/ev3/uart_sensord.h"
#include "../include/arch/ev3/sensor_dev.h"

static const ev3_uart_sensord_cfg_t* _cfg;
static ev3_uart_sensor_t _s;
static int32_t _port;
static int32_t _mode;          /* mode the client wants                    */
static int32_t _reset_mode;    /* mode to return to after a RESET, -1 none */

static void fill_data(ev3_sensor_data_t* d) {
    memset(d, 0, sizeof(*d));
    d->type = _s.type_id;
    d->port = _port;
    d->connected = _s.synced;
    d->mode = _mode;
    d->errors = (int32_t)_s.errors;
    d->timestamp_ms = (int64_t)kernel_tic_ms(0);

    int32_t n = (_mode >= 0 && _mode < EV3_UART_MAX_MODE) ? _s.datasets[_mode] : 0;
    if (n > EV3_SENSOR_MAX_VALUES) n = EV3_SENSOR_MAX_VALUES;
    if (n < 0) n = 0;
    d->nvalues = n;
    for (int32_t i = 0; i < n; i++)
        d->value[i] = ev3_uart_sensor_dataset(&_s, _mode, i);
}

static int32_t set_mode(int32_t m) {
    if (m < 0 || m >= EV3_UART_MAX_MODE)
        return -1;
    if (_s.synced && m >= _s.modes)
        return -1;
    _mode = m;
    _s.report_mode = m;
    ev3_uart_sensor_select(&_s, m);
    return 0;
}

/*
 * ev3dev resets the gyro (and re-zeroes other sensors) by bouncing to a
 * different mode and back; the return leg happens in the loop once the
 * sensor confirms the intermediate mode.
 */
static int32_t do_reset(void) {
    if (!_s.synced || _s.modes < 2)
        return -1;
    int32_t tmp = (_mode == 0) ? 1 : 0;
    _reset_mode = _mode;
    _s.report_mode = tmp;
    ev3_uart_sensor_select(&_s, tmp);
    return 0;
}

static int32_t do_command(const ev3_sensor_cmd_t* c) {
    switch (c->cmd) {
    case EV3_SENSOR_CMD_SET_MODE:
        return set_mode(c->arg0);
    case EV3_SENSOR_CMD_RESET:
        return do_reset();
    case EV3_SENSOR_CMD_RAW_WRITE: {
        uint8_t b[3] = { (uint8_t)c->arg0, (uint8_t)c->arg1, (uint8_t)c->arg2 };
        int32_t n = c->arg1 == -1 ? 1 : (c->arg2 == -1 ? 2 : 3);
        return ev3_uart_sensor_write(&_s, b, n) == n ? 0 : -1;
    }
    default:
        return -1;
    }
}

/* ---------------- vdevice callbacks ---------------- */

static int sensor_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        void* buf, int size, off_t offset, void* p) {
    (void)dev; (void)fd; (void)from_pid; (void)node; (void)offset; (void)p;

    if (size < (int)sizeof(ev3_sensor_data_t))
        return -1;
    ev3_sensor_data_t d;
    fill_data(&d);
    memcpy(buf, &d, sizeof(d));
    return sizeof(d);
}

static int sensor_write(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        const void* buf, int size, off_t offset, void* p) {
    (void)dev; (void)fd; (void)from_pid; (void)node; (void)offset; (void)p;

    if (buf == NULL || size < (int)sizeof(ev3_sensor_cmd_t))
        return -1;
    if (do_command((const ev3_sensor_cmd_t*)buf) != 0)
        return -1;
    return sizeof(ev3_sensor_cmd_t);
}

static int sensor_dev_cntl(vdevice_t* dev, int from_pid, int cmd,
        proto_t* in, proto_t* ret, void* p) {
    (void)dev; (void)from_pid; (void)p;

    switch (cmd) {
    case EV3_SENSOR_CNTL_GET_DATA: {
        ev3_sensor_data_t d;
        fill_data(&d);
        PF->clear(ret)->addi(ret, 0)->add(ret, &d, sizeof(d));
        return 0;
    }
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

static char* sensor_cmd(vdevice_t* dev, int from_pid, int argc, char** argv, void* p) {
    (void)dev; (void)from_pid; (void)p;
    char* buf = (char*)malloc(512);
    if (!buf) return NULL;

    const char* op = (argc > 0) ? argv[0] : "info";

    if (strcmp(op, "mode") == 0 && argc > 1) {
        int32_t r = set_mode(atoi(argv[1]));
        snprintf(buf, 512, "mode %d -> %s\n", _mode, r == 0 ? "ok" : "error");
    } else if (strcmp(op, "reset") == 0) {
        snprintf(buf, 512, "reset -> %s\n", do_reset() == 0 ? "ok" : "error");
    } else {
        int len = snprintf(buf, 512,
                "%s port %d type=%d synced=%d speed=%u modes=%d mode=%d errors=%u resyncs=%u\n",
                _cfg->name, _port + 1, _s.type_id, _s.synced, _s.speed,
                _s.modes, _mode, _s.errors, _s.resyncs);
        for (int32_t m = 0; m < _s.modes && m < EV3_UART_MAX_MODE && len < 480; m++) {
            len += snprintf(buf + len, 512 - len, " %d %-11s %s ds=%d:",
                    m, _s.name[m], _s.units[m], _s.datasets[m]);
            for (int32_t i = 0; i < _s.datasets[m] && i < EV3_UART_MAX_DATASETS && len < 500; i++)
                len += snprintf(buf + len, 512 - len, " %d", _s.data[m][i]);
            if (len < 511)
                len += snprintf(buf + len, 512 - len, "\n");
        }
    }
    return buf;
}

/* ---------------- loop ---------------- */

static int sensor_loop(vdevice_t* dev, void* p) {
    (void)p;

    ipc_disable();
    ev3_uart_sensor_poll(&_s);

    /* second leg of a RESET: sensor is now in the temporary mode */
    if (_reset_mode >= 0 && _s.synced && _s.mode != _reset_mode &&
        _s._new_mode < 0) {
        int32_t m = _reset_mode;
        _reset_mode = -1;
        _s.report_mode = m;
        ev3_uart_sensor_select(&_s, m);
    }
    ipc_enable();

    if (ev3_uart_sensor_consume_wakeup(&_s))
        vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);

    proc_usleep(1000);
    return 0;
}

int ev3_uart_sensord_main(const ev3_uart_sensord_cfg_t* cfg, int argc, char** argv) {
    _cfg = cfg;
    _port = cfg->default_port;
    _mode = cfg->default_mode;
    _reset_mode = -1;

    int c;
    while ((c = getopt(argc, argv, "p:m:")) != -1) {
        switch (c) {
        case 'p': _port = atoi(optarg) - 1; break;
        case 'm': _mode = atoi(optarg); break;
        default: break;
        }
    }
    const char* mnt_point = cfg->mnt_point;
    if (optind < argc)
        mnt_point = argv[optind];

    if (_port < 0 || _port >= EV3_IN_PORT_COUNT)
        _port = cfg->default_port;
    if (_mode < 0 || _mode >= EV3_UART_MAX_MODE)
        _mode = cfg->default_mode;

    ev3_gpio_init();

    if (ev3_uart_sensor_open(&_s, _port, _mode) != 0) {
        printf("%s: input port %d has no hardware UART\n", cfg->name, _port + 1);
        return -1;
    }

    vdevice_t dev;
    memset(&dev, 0, sizeof(vdevice_t));
    strncpy(dev.desc, cfg->name, sizeof(dev.desc) - 1);
    dev.read = sensor_read;
    dev.write = sensor_write;
    dev.dev_cntl = sensor_dev_cntl;
    dev.cmd = sensor_cmd;
    dev.loop_step = sensor_loop;
    device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666, false);

    ev3_uart_sensor_close(&_s);
    return 0;
}
