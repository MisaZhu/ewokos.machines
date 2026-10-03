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
#include "../include/arch/ev3/uart.h"
#include "../include/arch/ev3/uart_sensor.h"
#include "../include/arch/ev3/uart_sensord.h"
#include "../include/arch/ev3/sensor_dev.h"
#include "../include/arch/ev3/sensor_detect.h"

/* How long a bound link may stay down (watchdog resyncing with no sensor
 * answering) before we treat the sensor as unplugged and start scanning the
 * ports again. The hot-plug search cadence itself is PROBE_RETRY_MS. */
#define UART_LOST_MS   2500
/* A bound-but-not-yet-synced link is still finishing the INFO/ACK handshake,
 * which for the colour sensor takes ~1.4 s after CMD_TYPE; the short unplug
 * timer must not abort it (that made colour flicker in and out). Only once the
 * link has synced at least once does UART_LOST_MS apply; before that we allow
 * UART_SYNC_TIMEOUT_MS for the first sync to complete. */
#define UART_SYNC_TIMEOUT_MS 6000

static const ev3_uart_sensord_cfg_t* _cfg;
static ev3_uart_sensor_t _s;
static int32_t _port = EV3_SENSOR_PORT_NONE;   /* current port, -1 = searching */
static int32_t _mode;          /* mode the client wants                    */
static int32_t _reset_mode;    /* mode to return to after a RESET, -1 none */
static int32_t _pinned = -1;   /* -p override: only ever bind this port    */
static int     _adc_fd = -1;
static uint32_t _scan_ms;      /* last hot-plug scan (diagnostic only)     */
static uint32_t _lost_ms;      /* when the link first dropped, 0 = up      */
static uint32_t _next_probe_ms; /* when the next search cycle may start, 0 = arm */
static const char* _self_node; /* our own /dev node, so peers skip us       */
static int32_t _probe_claim = EV3_SENSOR_PORT_NONE; /* port being probed now */
static int     _ever_synced;   /* 1 once the bound link has synced once      */

/* defined in the auto-detection section below */
static void unbind(void);
static void scan_and_bind(uint32_t now);

/* Last published sample, held across a link re-sync so the reading does not
 * blank to 0 while the sensor re-advertises (see fill_data). Reset on
 * bind/unbind/mode-change so it can never outlive the reading it stands for. */
static int32_t _held_n = 0;
static int32_t _held_value[EV3_SENSOR_MAX_VALUES];

static void fill_data(ev3_sensor_data_t* d) {
    memset(d, 0, sizeof(*d));
    /* while searching (not synced yet) still report the type we look for so
     * clients can label the daemon; port stays -1 until we bind */
    d->type = _s.synced ? _s.type_id : _cfg->type_id;
    d->port = _port;
    /* transient probe claim, encoded +1 so 0 means "not probing" (see
     * ev3_sensor_data_t.probing): lets peers keep off a port we are mid-probe
     * on, without ev3test mistaking the claim for a binding */
    d->probing = (_probe_claim >= 0) ? (_probe_claim + 1) : 0;
    d->connected = _s.synced;
    d->mode = _mode;
    d->errors = (int32_t)_s.errors;
    d->timestamp_ms = (int64_t)kernel_tic_ms(0);

    int32_t n = (_mode >= 0 && _mode < EV3_UART_MAX_MODE) ? _s.datasets[_mode] : 0;
    if (n > EV3_SENSOR_MAX_VALUES) n = EV3_SENSOR_MAX_VALUES;
    if (n < 0) n = 0;

    if (!_s.synced && _held_n > 0) {
        /* Link is re-syncing: a resync clears _s.datasets[] while the sensor
         * re-advertises, and colour's INFO block alone takes ~1.4 s, so n would
         * be 0 and the value would read 0 for that whole window - the "colour
         * periodically reads 0" symptom. Hold the last good sample instead, the
         * way ev3dev/sysfs keeps the previous reading until the device goes
         * away; d->connected still reports 0 so the hiccup is visible. */
        d->nvalues = _held_n;
        for (int32_t i = 0; i < _held_n; i++)
            d->value[i] = _held_value[i];
    } else {
        d->nvalues = n;
        for (int32_t i = 0; i < n; i++)
            d->value[i] = ev3_uart_sensor_dataset(&_s, _mode, i);
        if (_s.synced && n > 0) {
            _held_n = n;               /* remember for the next re-sync */
            for (int32_t i = 0; i < n; i++)
                _held_value[i] = d->value[i];
        }
    }
}

static int32_t set_mode(int32_t m) {
    if (m < 0 || m >= EV3_UART_MAX_MODE)
        return -1;
    if (_s.synced && m >= _s.modes)
        return -1;
    _mode = m;
    _held_n = 0;               /* held sample belongs to the previous mode */
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
    } else if (strcmp(op, "scan") == 0) {
        /* force an immediate re-scan of the ports (hot-plug on demand) */
        if (_port >= 0)
            unbind();
        scan_and_bind((uint32_t)kernel_tic_ms(0));
        snprintf(buf, 512, "scan -> %s\n",
                _port >= 0 ? "bound" : "not found");
    } else if (_port < 0) {
        snprintf(buf, 512, "%s searching type=%d (no port)\n",
                _cfg->name, _cfg->type_id);
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

/* ---------------- port auto-detection / hot-plug ---------------- */

/* Attach to a port: power it, mux the UART and start the 2400-baud sync. */
static void bind(int port) {
    if (port < 0)
        return;
    if (ev3_uart_sensor_open(&_s, port, _mode) == 0) {
        _port = port;
        _reset_mode = -1;
        _lost_ms = 0;
        _held_n = 0;                            /* fresh link: nothing to hold */
        _ever_synced = 0;                       /* first sync may be slow */
        _probe_claim = EV3_SENSOR_PORT_NONE;    /* bound now, not probing */
    }
}

/* Detach: drop the UART and power so the port is free (and re-detectable). */
static void unbind(void) {
    ev3_uart_sensor_close(&_s);
    _port = EV3_SENSOR_PORT_NONE;
    _reset_mode = -1;
    _lost_ms = 0;
    _held_n = 0;            /* sensor gone: stop holding its last reading */
    _ever_synced = 0;
    _probe_claim = EV3_SENSOR_PORT_NONE;
    _next_probe_ms = 0;   /* re-stagger the next probe from the current time */
}

/* Look for our sensor on the ports and bind the first match. */
static void scan_and_bind(uint32_t now) {
    _scan_ms = now;
    if (_adc_fd < 0)
        _adc_fd = ev3_sensor_adc_open();   /* adcd may come up late */
    ev3_sensor_power_ports();
    int port;
    if (_pinned >= 0)
        /* explicit -p: trust the operator. The ADC cannot name a UART sensor
         * (pin 1 is pulled to ground, no ID voltage), so gating on
         * ev3_sensor_detect_port here would reject every UART port - bind the
         * requested port directly as long as it has a hardware UART. This is
         * the fixed-port behaviour that is known to work. */
        port = (ev3_input_port_uart_base(_pinned) != 0) ? _pinned : EV3_SENSOR_PORT_NONE;
    else
        port = ev3_sensor_find_uart(_adc_fd, _cfg->type_id, EV3_SENSOR_PORT_NONE);
    if (port >= 0)
        bind(port);
}

/* ---------------- UART protocol probe (ADC-independent) ----------------
 *
 * An EV3/UART sensor pulls pin 1 to ground and carries no ID voltage, so the
 * ADC path can never name it - only its own UART handshake can, because the
 * CMD_TYPE frame reports the type id with no voltage table, polarity or
 * channel mapping involved. Several UART daemons run at once and opening a
 * port installs that process as the RX-interrupt owner AND reprograms the baud
 * divisor, so two daemons must never drive the same port at the same time.
 *
 * Only the FIRST probe is staggered: each daemon waits probe_slot() *
 * PROBE_STAGGER_MS after startup so the four daemons (which all start together
 * at boot) do not hit the UARTs on the same tick. After that every daemon
 * re-probes on the PROBE_RETRY_MS (~2 s) hot-plug cadence, and serialisation is
 * done by a transient CLAIM instead of a long stagger: a daemon publishes the
 * port it is about to drive in its own GET_DATA (ev3_sensor_data_t.probing) and
 * skips any port a peer has claimed or bound. Claiming before querying peers
 * (write-then-read) makes the check race-free - of two daemons racing for one
 * port, at least one sees the other's claim and backs off, so only one ever
 * drives the UART/divisor. (The old absolute clock grid tied the first probe to
 * the global phase and could delay it by a whole ~16.8 s cycle; a long fixed
 * stagger would make hot-plug sluggish.)
 *
 * A daemon learns which ports peers hold by asking them over their /dev nodes
 * (see port_owned_by_peer) rather than by peeking at the UART: the sensor UARTs
 * stay clock-gated until ev3_input_port_uart_enable() ungates them (see
 * port.c), and reading a gated 16550 on DA8xx returns all-ones, so an LSR.DR
 * peek saw EVERY empty port as busy and no daemon ever probed - the reason
 * auto-detect never bound. A peer answers GET_DATA even while it is mid-probe,
 * because device_run() services IPC with IPC_NON_BLOCK and the probe only masks
 * IPC around each individual poll, so the claim is always readable.
 *
 * Only input ports 1 and 2 have a hardware UART (ports 3 and 4 are PRU
 * soft-UART and cannot be driven here), so a probe must cover BOTH of them:
 * probing just the first and returning would starve input port 2 whenever
 * input port 1 is empty or holds a different sensor.
 */
#define PROBE_STAGGER_MS 300      /* per-daemon offset for the FIRST probe only,
                                   * so the four daemons (started together at
                                   * boot) do not all hit the UARTs on the same
                                   * tick. Ongoing serialisation comes from the
                                   * transient probing claim (see below), not a
                                   * long stagger. */
#define PROBE_RETRY_MS   2000     /* hot-plug detection cycle while searching,
                                   * timed from the END of the previous attempt
                                   * (see the search loop). A ~2 s quiet gap lets
                                   * a sensor finish a long handshake - colour's
                                   * INFO+ACK is ~1.4 s - and stops the bus being
                                   * re-tapped faster than a port can answer, so
                                   * scanning never churns or disturbs a peer's
                                   * live perception data. Deliberately not
                                   * tighter. */
#define PROBE_SYNC_MS    3500     /* The sensor is powered from the always-on
                                   * VCC5V rail and FREE-RUNS: it repeats
                                   * CMD_TYPE..INFO..SYNC until the brick ACKs
                                   * (pybricks uart-protocol). Enabling the
                                   * line buffer taps that stream at a random
                                   * point, so the next CMD_TYPE can be a whole
                                   * cycle away - and the 6-mode colour sensor
                                   * has the longest cycle (~1.4 s) plus an
                                   * inter-repeat gap. The window must span a
                                   * full cycle with margin, or CMD_TYPE is
                                   * missed and colour only binds "sometimes". */
#define PROBE_IDLE_MS    200      /* An EMPTY port idles high and never puts a
                                   * byte on the wire, so give up on it fast
                                   * (keeps hot-plug scanning snappy). Used only
                                   * until the first byte arrives: once a sensor
                                   * has spoken we ride out its inter-repeat gap
                                   * for the whole PROBE_SYNC_MS instead of
                                   * abandoning it mid-handshake. */

static int probe_slot(void) {
    switch (_cfg->type_id) {
    case EV3_SENSOR_TYPE_EV3_COLOR: return 0;
    case EV3_SENSOR_TYPE_EV3_US:    return 1;
    case EV3_SENSOR_TYPE_EV3_GYRO:  return 2;
    case EV3_SENSOR_TYPE_EV3_IR:    return 3;
    default:                        return 0;
    }
}

/* True when a peer sensor daemon already holds this port. Opening it here
 * would reprogram the divisor to 2400 baud and steal the RX interrupt out from
 * under the owner, tearing down its live link, so the probe skips any port a
 * peer reports as bound. We ask the peers over their /dev nodes (the same
 * GET_DATA ev3test reads) instead of peeking at the UART registers: the sensor
 * UARTs stay clock-gated until ev3_input_port_uart_enable() ungates them, and
 * an LSR read of a gated 16550 returns all-ones on DA8xx, which the old peek
 * mistook for "RX data ready" on every port - so nothing was ever probed.
 * A peer that is not running holds nothing, and a peer that answers with a
 * different struct simply fails the size check and is treated as free. */
static const char* const _peer_nodes[] = {
    "/dev/color0", "/dev/us0", "/dev/gyro0", "/dev/ir0",
    "/dev/touch0", "/dev/nxt-us0",
};

static int port_owned_by_peer(int port) {
    for (unsigned i = 0; i < sizeof(_peer_nodes) / sizeof(_peer_nodes[0]); i++) {
        const char* node = _peer_nodes[i];
        if (_self_node && strcmp(node, _self_node) == 0)
            continue;                    /* never query ourselves: it would
                                          * block waiting for our own reply */
        if (dev_get_pid(node) <= 0)
            continue;                    /* daemon not running: holds nothing */
        proto_t ret;
        PF->init(&ret);
        ev3_sensor_data_t d;
        memset(&d, 0, sizeof(d));
        int ok = (dev_cntl(node, EV3_SENSOR_CNTL_GET_DATA, NULL, &ret) == 0 &&
                  proto_read_int(&ret) == 0 &&
                  proto_read_to(&ret, &d, sizeof(d)) == (int32_t)sizeof(d));
        PF->clear(&ret);
        if (ok && (d.port == port || d.probing == port + 1))
            return 1;
    }
    return 0;
}

/* Probe every UART-capable port: skip any a peer already holds, otherwise open
 * it, give the handshake up to PROBE_SYNC_MS to report a type, and keep the
 * port only if the type is ours. Because probes are staggered per daemon (see
 * PROBE_STAGGER_MS), only one daemon probes at a time, so walking both ports
 * here cannot contend with another daemon's probe, and port_owned_by_peer()
 * keeps us off any port a peer has already bound and is driving. */
static void protocol_probe(void) {
    for (int p = 0; p < EV3_IN_PORT_COUNT; p++) {
        if (ev3_input_port_uart_base(p) == 0)
            continue;               /* PRU soft-UART ports 3/4: not drivable */

        /* Claim the port BEFORE asking the peers (write-then-read). With this
         * order two daemons can never both see the port free: if A and B race,
         * at least one reads the other's claim and backs off, so only one ever
         * drives the UART / reprograms the divisor. The claim is published via
         * our own GET_DATA (d.probing) and stays readable while we block in the
         * poll loop below, because device_run() services IPC with IPC_NON_BLOCK
         * and we only mask it around each individual poll. */
        _probe_claim = p;
        if (port_owned_by_peer(p)) {
            _probe_claim = EV3_SENSOR_PORT_NONE;
            continue;               /* a peer holds, or is mid-probe on, this port */
        }
        if (ev3_uart_sensor_open(&_s, p, _mode) != 0) {
            _probe_claim = EV3_SENSOR_PORT_NONE;
            continue;
        }

        /* Watch the free-running stream until it names itself. CMD_TYPE is the
         * first frame of each repetition and carries the type id, but our tap
         * lands mid-cycle so it can be up to PROBE_SYNC_MS away. An EMPTY port
         * never puts a byte on the wire, so bail after PROBE_IDLE_MS of silence
         * - but only until the first byte arrives: once a sensor has spoken we
         * ride out its inter-repeat gap and keep waiting for CMD_TYPE, or a
         * colour sensor (longest cycle + gap) gets abandoned mid-handshake and
         * only binds "sometimes". Stop the moment the type is known; if it is
         * ours we keep the port and let the main loop finish the INFO/ACK. */
        uint32_t t0 = (uint32_t)kernel_tic_ms(0);
        uint32_t last_head = _s._rx_head;
        uint32_t last_rx = t0;      /* last time the RX ring advanced */
        int saw_bytes = 0;          /* any RX at all => a sensor is present */
        for (;;) {
            ipc_disable();
            ev3_uart_sensor_poll(&_s);
            ipc_enable();

            if (_s.type_id != 0 || _s.synced)
                break;                       /* CMD_TYPE seen (or link up) */

            uint32_t t = (uint32_t)kernel_tic_ms(0);
            if (_s._rx_head != last_head) {  /* bytes arriving: sensor present */
                last_head = _s._rx_head;
                last_rx = t;
                saw_bytes = 1;
            } else if (!saw_bytes && t - last_rx >= PROBE_IDLE_MS) {
                break;                       /* silent from the start: empty */
            }
            if (t - t0 >= PROBE_SYNC_MS)
                break;                       /* a full cycle, still no type */
            usleep(2000);
        }
        if (_s.type_id != 0 && _s.type_id == _cfg->type_id) {
            _port = p;
            _reset_mode = -1;
            _lost_ms = 0;
            _held_n = 0;                         /* fresh link: nothing to hold */
            _ever_synced = 0;                    /* main loop finishes the sync */
            _probe_claim = EV3_SENSOR_PORT_NONE; /* bound now, stop claiming */
            return;
        }
        ev3_uart_sensor_close(&_s);
        _probe_claim = EV3_SENSOR_PORT_NONE;
    }
    _probe_claim = EV3_SENSOR_PORT_NONE;         /* nothing matched this round */
}

/* ---------------- loop ---------------- */

static int sensor_loop(vdevice_t* dev, void* p) {
    (void)p;

    /* never 0: _lost_ms and _next_probe_ms use 0 as "unset", and an unsigned
     * now < a stored timestamp would wrap the elapsed-time tests below */
    uint32_t now = (uint32_t)kernel_tic_ms(0);
    if (now == 0)
        now = 1;

    if (_port >= 0) {
        /* bound: drive the protocol */
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

        /* link health: an unplugged sensor keeps resyncing at 2400 baud and
         * never syncs again; after a grace period treat it as removed and
         * go back to scanning so we can follow it to another port */
        if (_s.synced) {
            _lost_ms = 0;
            _ever_synced = 1;
        } else {
            /* Before the first sync the colour sensor is still streaming its
             * long INFO block + ACK handshake, which can outlast the short
             * unplug timer; aborting it there made colour bind then immediately
             * drop ("occasionally recognised"). Allow UART_SYNC_TIMEOUT_MS for
             * the first sync, then the normal UART_LOST_MS unplug grace. */
            uint32_t limit = _ever_synced ? UART_LOST_MS : UART_SYNC_TIMEOUT_MS;
            if (_lost_ms == 0)
                _lost_ms = now;
            else if (now - _lost_ms >= limit)
                unbind();
        }
    } else {
        /* Searching: one hot-plug cycle powers the ports, ADC-binds an
         * analog/touch sensor, then protocol-probes the UART-capable ports.
         * The cycle is gated by _next_probe_ms and timed from the END of the
         * previous attempt, so a sensor that is still handshaking (colour's
         * INFO+ACK is ~1.4 s) always gets a full quiet PROBE_RETRY_MS before
         * the bus is tapped again, and a port is never re-opened faster than it
         * can answer - the scan never churns or disturbs live perception data.
         * The first cycle is offset by probe_slot() * PROBE_STAGGER_MS so the
         * four daemons (started together at boot) begin out of phase; the
         * transient claim inside protocol_probe() serialises them thereafter and
         * skips any port a peer has bound or is mid-probe on, so two daemons
         * never drive the same UART. */
        if (_next_probe_ms == 0)
            _next_probe_ms = now + (uint32_t)probe_slot() * PROBE_STAGGER_MS;
        if ((int32_t)(now - _next_probe_ms) >= 0) {
            scan_and_bind(now);
            if (_port < 0) {
                protocol_probe();
                _next_probe_ms = (uint32_t)kernel_tic_ms(0) + PROBE_RETRY_MS;
            }
        }
    }

    if (ev3_uart_sensor_consume_wakeup(&_s))
        vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);

    usleep(1000);
    return 0;
}

int ev3_uart_sensord_main(const ev3_uart_sensord_cfg_t* cfg, int argc, char** argv) {
    _cfg = cfg;
    _port = EV3_SENSOR_PORT_NONE;   /* auto-detect; no fixed port */
    _mode = cfg->default_mode;
    _reset_mode = -1;
    _pinned = -1;

    int c;
    while ((c = getopt(argc, argv, "p:m:")) != -1) {
        switch (c) {
        /* -p is now an optional restriction, not a requirement: without it
         * the daemon scans every hardware-UART port for its sensor type. */
        case 'p': _pinned = atoi(optarg) - 1; break;
        case 'm': _mode = atoi(optarg); break;
        default: break;
        }
    }
    const char* mnt_point = cfg->mnt_point;
    if (optind < argc)
        mnt_point = argv[optind];
    _self_node = mnt_point;   /* so port_owned_by_peer() never queries us */

    if (_pinned < -1 || _pinned >= EV3_IN_PORT_COUNT)
        _pinned = -1;
    if (_mode < 0 || _mode >= EV3_UART_MAX_MODE)
        _mode = cfg->default_mode;

    ev3_gpio_init();

    /* the sensor type is discovered from the pin 1 ID voltage published by
     * adcd; without it we cannot tell which port the sensor is on */
    _adc_fd = ev3_sensor_adc_open();
    if (_adc_fd < 0)
        printf("%s: /dev/adc0 not ready; ADC detection off, will retry\n",
                cfg->name);

    memset(&_s, 0, sizeof(_s));
    _s.port = EV3_SENSOR_PORT_NONE;
    _s._new_mode = -1;

    /* try to bind straight away so the port is up before the first client
     * read; if the sensor is not plugged in yet the loop keeps scanning */
    scan_and_bind((uint32_t)kernel_tic_ms(0));

    vdevice_t dev;
    memset(&dev, 0, sizeof(vdevice_t));
    strncpy(dev.desc, cfg->name, sizeof(dev.desc) - 1);
    dev.read = sensor_read;
    dev.write = sensor_write;
    dev.dev_cntl = sensor_dev_cntl;
    dev.cmd = sensor_cmd;
    dev.loop_step = sensor_loop;
    device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666, false);

    if (_port >= 0)
        ev3_uart_sensor_close(&_s);
    ev3_sensor_adc_close(_adc_fd);
    return 0;
}
