/*
 * i2cd - generic I2C master for one EV3 input port.
 *
 * The EV3 input ports have no hardware I2C controller: pin 5 (SCL) and
 * pin 6 (SDA) are GPIOs that LEGO's firmware and ev3dev (legoev3-fiq.c)
 * bit-bang at 9.6 kHz. This daemon does the same via arch/ev3/i2c.h
 * (ev3_i2c_gpio_*) and exposes the bus with the fixed-width protocol of
 * arch/ev3/i2c_dev.h:
 *
 *   dev_cntl(I2C_CNTL_XFER)   in/out: i2c_xfer_t
 *   dev_cntl(I2C_CNTL_INFO)   out   : i2c_bus_info_t
 *   dev_cntl(I2C_CNTL_SPEED)  in    : i2c_bus_info_t (hz)
 *   write(fd, &i2c_xfer_t, sizeof) -> run the transfer, keep the result
 *   read(fd, &i2c_xfer_t, sizeof)  -> result of the last transfer
 *
 * Transfers run synchronously inside the request handler. The master
 * owns SCL, so being preempted only stretches the clock, which every
 * I2C slave tolerates; no ipc_disable() is needed (and it is not allowed
 * outside loop_step anyway).
 *
 * Text (dev.cmd, shell debugging):
 *   scan                       probe 7-bit addresses 0x01..0x77
 *   r <addr> <reg> <len>       read len bytes starting at reg
 *   w <addr> <b0> [b1..]       write bytes
 *   info
 *
 * Options:
 *   -p <1-4>   input port (default 1)
 *   -s <hz>    bus clock (default 9600, max 100000)
 *   [mount]    mount point (default /dev/i2c<port-1>)
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

#include <arch/ev3/gpio.h>
#include <arch/ev3/port.h>
#include <arch/ev3/i2c.h>
#include <arch/ev3/i2c_dev.h>

static ev3_i2c_gpio_t _bus;
static int32_t _port = EV3_IN_PORT_1;
static int32_t _hz = EV3_I2C_GPIO_HZ_NXT;
static int32_t _xfers = 0;

static i2c_xfer_t _last;      /* result of the most recent write()/XFER */

static int32_t do_xfer(i2c_xfer_t* x) {
    if (x->addr < 0 || x->addr > 0x7F ||
        x->wlen < 0 || x->wlen > I2C_DEV_MAX_DATA ||
        x->rlen < 0 || x->rlen > I2C_DEV_MAX_DATA ||
        (x->wlen == 0 && x->rlen == 0)) {
        x->result = -1;
        return -1;
    }
    x->result = ev3_i2c_gpio_xfer(&_bus, (uint8_t)x->addr,
            x->wlen > 0 ? x->wdata : NULL, x->wlen,
            x->rlen > 0 ? x->rdata : NULL, x->rlen);
    _xfers++;
    return x->result;
}

static void fill_info(i2c_bus_info_t* bi) {
    memset(bi, 0, sizeof(*bi));
    bi->port = _port;
    bi->hz = _hz;
    bi->xfers = _xfers;
    bi->nacks = _bus.nacks;
}

static int32_t set_speed(int32_t hz) {
    if (hz <= 0)
        hz = EV3_I2C_GPIO_HZ_NXT;
    if (hz > EV3_I2C_GPIO_HZ_MAX)
        return -1;
    ev3_i2c_gpio_close(&_bus);
    if (ev3_i2c_gpio_open(&_bus, _port, hz) != 0)
        return -1;
    _hz = hz;
    return 0;
}

/* ---------------- vdevice callbacks ---------------- */

static int i2c_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        void* buf, int size, off_t offset, void* p) {
    (void)dev; (void)fd; (void)from_pid; (void)node; (void)offset; (void)p;

    if (size < (int)sizeof(_last))
        return -1;
    memcpy(buf, &_last, sizeof(_last));
    return sizeof(_last);
}

static int i2c_write(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        const void* buf, int size, off_t offset, void* p) {
    (void)dev; (void)fd; (void)from_pid; (void)node; (void)offset; (void)p;

    if (buf == NULL || size < (int)sizeof(i2c_xfer_t))
        return -1;
    memcpy(&_last, buf, sizeof(_last));
    if (do_xfer(&_last) != 0)
        return -1;
    return sizeof(i2c_xfer_t);
}

static int i2c_dev_cntl(vdevice_t* dev, int from_pid, int cmd,
        proto_t* in, proto_t* ret, void* p) {
    (void)dev; (void)from_pid; (void)p;

    switch (cmd) {
    case I2C_CNTL_XFER: {
        i2c_xfer_t x;
        memset(&x, 0, sizeof(x));
        if (proto_read_to(in, &x, sizeof(x)) != (int32_t)sizeof(x)) {
            PF->clear(ret)->addi(ret, -1);
            return -1;
        }
        int32_t res = do_xfer(&x);
        _last = x;
        PF->clear(ret)->addi(ret, res)->add(ret, &x, sizeof(x));
        return res;
    }
    case I2C_CNTL_INFO: {
        i2c_bus_info_t bi;
        fill_info(&bi);
        PF->clear(ret)->addi(ret, 0)->add(ret, &bi, sizeof(bi));
        return 0;
    }
    case I2C_CNTL_SPEED: {
        i2c_bus_info_t bi;
        memset(&bi, 0, sizeof(bi));
        if (proto_read_to(in, &bi, sizeof(bi)) != (int32_t)sizeof(bi)) {
            PF->clear(ret)->addi(ret, -1);
            return -1;
        }
        int32_t res = set_speed(bi.hz);
        PF->clear(ret)->addi(ret, res);
        return res;
    }
    default:
        PF->clear(ret)->addi(ret, -1);
        return -1;
    }
}

/* ---------------- text interface (shell debugging) ---------------- */

static char* i2c_cmd(vdevice_t* dev, int from_pid, int argc, char** argv, void* p) {
    (void)dev; (void)from_pid; (void)p;
    char* buf = (char*)malloc(512);
    if (!buf) return NULL;

    const char* op = (argc > 0) ? argv[0] : "info";

    if (strcmp(op, "scan") == 0) {
        int len = snprintf(buf, 512, "i2c port %d (%d Hz):", _port + 1, _hz);
        for (int a = 0x01; a <= 0x77 && len < 480; a++) {
            uint8_t b;
            if (ev3_i2c_gpio_read(&_bus, (uint8_t)a, &b, 1) == 0)
                len += snprintf(buf + len, 512 - len, " 0x%02x", a);
        }
        snprintf(buf + len, 512 - len, "\n");
    } else if (strcmp(op, "r") == 0 && argc >= 4) {
        i2c_xfer_t x;
        memset(&x, 0, sizeof(x));
        x.addr = (int32_t)strtol(argv[1], NULL, 0);
        x.wdata[0] = (uint8_t)strtol(argv[2], NULL, 0);
        x.wlen = 1;
        x.rlen = atoi(argv[3]);
        if (x.rlen < 1 || x.rlen > I2C_DEV_MAX_DATA) x.rlen = 1;
        if (do_xfer(&x) != 0) {
            snprintf(buf, 512, "read failed (%d)\n", x.result);
        } else {
            int off = snprintf(buf, 512, "0x%02x[0x%02x] =", x.addr, x.wdata[0]);
            for (int32_t i = 0; i < x.rlen; i++)
                off += snprintf(buf + off, 512 - off, " %02x", x.rdata[i]);
            snprintf(buf + off, 512 - off, "\n");
        }
        _last = x;
    } else if (strcmp(op, "w") == 0 && argc >= 3) {
        i2c_xfer_t x;
        memset(&x, 0, sizeof(x));
        x.addr = (int32_t)strtol(argv[1], NULL, 0);
        for (int i = 2; i < argc && x.wlen < I2C_DEV_MAX_DATA; i++)
            x.wdata[x.wlen++] = (uint8_t)strtol(argv[i], NULL, 0);
        do_xfer(&x);
        snprintf(buf, 512, "write 0x%02x len=%d -> %d\n", x.addr, x.wlen, x.result);
        _last = x;
    } else if (strcmp(op, "speed") == 0 && argc >= 2) {
        int32_t r = set_speed(atoi(argv[1]));
        snprintf(buf, 512, "speed %d Hz -> %s\n", _hz, r == 0 ? "ok" : "error");
    } else {
        snprintf(buf, 512, "i2c port %d hz=%d xfers=%d nacks=%d\n"
                "cmds: scan | r <addr> <reg> <len> | w <addr> <b0> [b1..] | speed <hz>\n",
                _port + 1, _hz, _xfers, _bus.nacks);
    }
    return buf;
}

static int doargs(int argc, char* argv[]) {
    int c;
    while ((c = getopt(argc, argv, "p:s:")) != -1) {
        switch (c) {
        case 'p': _port = atoi(optarg) - 1; break;
        case 's': _hz = atoi(optarg); break;
        default: break;
        }
    }
    return optind;
}

int main(int argc, char** argv) {
    int argind = doargs(argc, argv);

    if (_port < 0 || _port >= EV3_IN_PORT_COUNT)
        _port = EV3_IN_PORT_1;
    if (_hz <= 0 || _hz > EV3_I2C_GPIO_HZ_MAX)
        _hz = EV3_I2C_GPIO_HZ_NXT;

    char mnt_buf[32];
    snprintf(mnt_buf, sizeof(mnt_buf), "/dev/i2c%d", _port);
    const char* mnt_point = mnt_buf;
    if (argind < argc)
        mnt_point = argv[argind];

    ev3_gpio_init();
    if (ev3_i2c_gpio_open(&_bus, _port, _hz) != 0) {
        printf("i2cd: cannot open input port %d\n", _port + 1);
        return -1;
    }
    memset(&_last, 0, sizeof(_last));

    vdevice_t dev;
    memset(&dev, 0, sizeof(vdevice_t));
    strcpy(dev.desc, "i2cd");
    dev.read = i2c_read;
    dev.write = i2c_write;
    dev.dev_cntl = i2c_dev_cntl;
    dev.cmd = i2c_cmd;
    device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666, false);

    ev3_i2c_gpio_close(&_bus);
    return 0;
}
