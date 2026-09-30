/*
 * EV3 UART sensor protocol host - see include/arch/ev3/uart_sensor.h.
 *
 * The state machine follows ev3dev's ev3_uart_sensor_ld.c receive_buf2():
 *
 *   ST_SYNC      scan for a valid 3-byte CMD_TYPE frame at 2400 baud
 *   ST_INFO      collect CMD_MODES / CMD_SPEED / INFO_* until SYS_ACK
 *   ST_ACK_DELAY 10 ms grace before answering SYS_ACK (ev3dev mdelay(10))
 *   ST_ACK_WAIT  SYS_ACK sent; wait >= 4 ms and TX shift register empty,
 *                then switch the divisor to the negotiated speed
 *   ST_DATA      link up: DATA frames, 100 ms keep-alive NACK, watchdog
 *
 * Any protocol violation during ST_INFO, or too many consecutive bad /
 * missing DATA frames in ST_DATA, drops back to 2400 baud and ST_SYNC.
 *
 * RX bytes arrive through the UART interrupt of the input port and are
 * queued in a ring inside ev3_uart_sensor_t; everything else runs from
 * ev3_uart_sensor_poll() in the driver's loop_step.
 */
#include <stdio.h>
#include <string.h>
#include <ewoksys/mmio.h>
#include <ewoksys/interrupt.h>
#include <ewoksys/kernel_tic.h>

#include "../include/arch/ev3/uart_sensor.h"
#include "../include/arch/ev3/uart.h"
#include "../include/arch/ev3/port.h"

/* ---- header bit fields ---- */
#define MSG_TYPE_MASK  0xC0
#define MSG_SYS        0x00
#define MSG_CMD        0x40
#define MSG_INFO       0x80
#define MSG_DATA       0xC0
#define MSG_CMD_MASK   0x07
#define MSG_SIZE(h)    (1 << (((h) >> 3) & 0x7))

/* ---- system bytes ---- */
#define SYS_SYNC       0x00
#define SYS_NACK       0x02
#define SYS_ACK        0x04

/* ---- CMD_* subcommands ---- */
#define CMD_TYPE       0x00
#define CMD_MODES      0x01
#define CMD_SPEED      0x02
#define CMD_SELECT     0x03
#define CMD_WRITE      0x04

/* ---- INFO_* subcommands ---- */
#define INFO_NAME      0x00
#define INFO_RAW       0x01
#define INFO_PCT       0x02
#define INFO_SI        0x03
#define INFO_UNITS     0x04
#define INFO_FORMAT    0x80
#define INFO_MODE_P8   0x20

/* ---- info bookkeeping (ev3dev info_flags) ---- */
#define IF_CMD_TYPE    (1u << 0)
#define IF_CMD_MODES   (1u << 1)
#define IF_CMD_SPEED   (1u << 2)
#define IF_INFO_NAME   (1u << 3)
#define IF_INFO_RAW    (1u << 4)
#define IF_INFO_PCT    (1u << 5)
#define IF_INFO_SI     (1u << 6)
#define IF_INFO_UNITS  (1u << 7)
#define IF_INFO_FORMAT (1u << 8)
#define IF_ALL_INFO    (IF_INFO_NAME | IF_INFO_RAW | IF_INFO_PCT | IF_INFO_SI | \
                        IF_INFO_UNITS | IF_INFO_FORMAT)
#define IF_REQUIRED    (IF_CMD_TYPE | IF_CMD_MODES | IF_INFO_NAME | IF_INFO_FORMAT)

/* ---- parser states ---- */
#define ST_SYNC        0
#define ST_INFO        1
#define ST_ACK_DELAY   2
#define ST_ACK_WAIT    3
#define ST_DATA        4

#define ACK_DELAY_MS   10
#define ACK_SETTLE_MS  4

#define RING_MASK      (EV3_UART_RX_RING - 1)

/* ---------------- interrupt side ---------------- */

static ev3_uart_sensor_t*   _irq_owner[EV3_IN_PORT_COUNT];
static interrupt_handler_t  _irq_handlers[EV3_IN_PORT_COUNT];

static void rx_irq(uint32_t irq, ewokos_addr_t data) {
    (void)irq;
    ev3_uart_sensor_t* s = (ev3_uart_sensor_t*)data;
    if (s == NULL || s->_base == 0)
        return;

    /* always drain the FIFO so the level interrupt is cleared */
    while (ev3_uart_can_read(s->_base)) {
        uint8_t b = (uint8_t)ev3_uart_getc(s->_base);
        uint32_t head = s->_rx_head;
        uint32_t next = (head + 1) & RING_MASK;
        if (next == s->_rx_tail) {
            s->_rx_overrun++;
            continue;
        }
        s->_rx_ring[head] = b;
        s->_rx_head = next;
    }
}

static int32_t rx_pop(ev3_uart_sensor_t* s, uint8_t* b) {
    uint32_t tail = s->_rx_tail;
    if (tail == s->_rx_head)
        return 0;
    *b = s->_rx_ring[tail];
    s->_rx_tail = (tail + 1) & RING_MASK;
    return 1;
}

static void rx_flush(ev3_uart_sensor_t* s) {
    s->_rx_tail = s->_rx_head;
}

/* ---------------- low-level helpers ---------------- */

static inline uint32_t now_ms(void) {
    return (uint32_t)kernel_tic_ms(0);
}

static void uart_write(ev3_uart_sensor_t* s, const uint8_t* buf, int32_t n) {
    /* THRE set means the 16-byte TX FIFO is empty: burst up to 16 bytes */
    for (int32_t i = 0; i < n; i++) {
        if ((i & 15) == 0) {
            int32_t guard = 0;
            while (!ev3_uart_can_write(s->_base) && guard++ < 200000)
                ;
        }
        ev3_uart_putc(s->_base, (char)buf[i]);
    }
}

static void uart_put(ev3_uart_sensor_t* s, uint8_t b) {
    uart_write(s, &b, 1);
}

static int32_t fmt_size(int32_t fmt) {
    switch (fmt) {
    case EV3_UART_DATA8:  return 1;
    case EV3_UART_DATA16: return 2;
    case EV3_UART_DATA32:
    case EV3_UART_DATAF:  return 4;
    default:              return 1;
    }
}

static int32_t decode_le(const uint8_t* p, int32_t fmt) {
    switch (fmt) {
    case EV3_UART_DATA8:
        return (int8_t)p[0];
    case EV3_UART_DATA16:
        return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
    case EV3_UART_DATA32:
        return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
    case EV3_UART_DATAF: {
        uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        float f;
        memcpy(&f, &u, sizeof(f));
        return (int32_t)f;
    }
    default:
        return p[0];
    }
}

static void copy_str(char* dst, int32_t dst_len, const uint8_t* src, int32_t n) {
    if (n > dst_len - 1) n = dst_len - 1;
    int32_t i = 0;
    for (; i < n && src[i] != 0; i++)
        dst[i] = (char)src[i];
    dst[i] = 0;
}

/* ---------------- link control ---------------- */

static void send_select(ev3_uart_sensor_t* s, int32_t mode) {
    /* MESSAGE_CMD | LENGTH_1 | CMD_SELECT, <mode>, <checksum> */
    uint8_t m[3];
    m[0] = (uint8_t)(MSG_CMD | CMD_SELECT);
    m[1] = (uint8_t)mode;
    m[2] = (uint8_t)(0xff ^ m[0] ^ m[1]);
    uart_write(s, m, 3);
}

static void start_select(ev3_uart_sensor_t* s, int32_t mode, uint32_t now) {
    s->_new_mode = mode;
    s->_set_mode_retries = 0;
    s->_set_mode_ms = now;
    send_select(s, mode);
}

/* drop the link back to 2400 baud and look for CMD_TYPE again */
static void resync(ev3_uart_sensor_t* s) {
    s->errors++;
    if (s->synced)
        s->resyncs++;
    s->synced = 0;
    s->_info_done = 0;
    s->_state = ST_SYNC;
    s->_msg_idx = 0;
    s->_msg_len = 0;
    s->_new_mode = -1;
    s->_num_data_err = 0;
    s->speed = EV3_UART_SPEED_MIN;
    ev3_uart_set_baud(s->_base, EV3_UART_SPEED_MIN);
    rx_flush(s);
    s->_wakeup = 1;
}

/* ev3_uart_change_bitrate(): TX drained, switch divisor, start keep-alive
 * and restore the mode the client asked for. */
static void link_up(ev3_uart_sensor_t* s, uint32_t now) {
    ev3_uart_set_baud(s->_base, (int)s->speed);
    rx_flush(s);
    s->_state = ST_DATA;
    s->_info_done = 1;
    s->synced = 1;
    s->mode = 0;              /* sensors come up in mode 0 after INFO */
    s->_ignore_ff = 1;
    s->_num_data_err = 0;
    s->_keep_alive_ms = now;
    s->_last_data_ms = now;
    s->_msg_idx = 0;
    s->_wakeup = 1;

    if (s->_requested_mode > 0 && s->_requested_mode < s->modes)
        start_select(s, s->_requested_mode, now);
    else
        s->_new_mode = -1;
}

/* ---------------- message processing ---------------- */

/* returns 0 on success, -1 if the link must be re-synchronised */
static int32_t handle_msg(ev3_uart_sensor_t* s, uint32_t now) {
    const uint8_t* m = s->_msg;
    int32_t size = s->_msg_len;
    int32_t type = m[0] & MSG_TYPE_MASK;
    int32_t cmd  = m[0] & MSG_CMD_MASK;
    int32_t cmd2 = (size > 1) ? m[1] : 0;

    if (size > 1) {
        uint8_t cs = 0xff;
        for (int32_t i = 0; i < size - 1; i++)
            cs ^= m[i];
        /* the EV3 color sensor sends bad checksums for RGB-RAW (0xDC) */
        if (cs != m[size - 1] &&
            !(s->type_id == EV3_UART_TYPE_EV3_COLOR && m[0] == 0xDC)) {
            s->errors++;
            if (s->_state == ST_DATA) {
                s->_num_data_err++;
                return 0;
            }
            return -1;
        }
    }

    switch (type) {
    case MSG_SYS:
        if (cmd == SYS_ACK && s->_state == ST_INFO) {
            if (s->modes <= 0 || (s->_info_flags & IF_REQUIRED) != IF_REQUIRED)
                return -1;         /* ACK before all required INFO */
            s->_state = ST_ACK_DELAY;
            s->_ack_ms = now;
        }
        /* SYNC / NACK from the sensor carry no information */
        return 0;

    case MSG_CMD:
        if (s->_state != ST_INFO)
            return -1;
        switch (cmd) {
        case CMD_MODES:
            if (s->_info_flags & IF_CMD_MODES)
                return -1;         /* duplicate */
            s->_info_flags |= IF_CMD_MODES;
            if (cmd2 >= EV3_UART_MAX_MODE)
                return -1;
            s->modes = cmd2 + 1;
            s->view_modes = (size > 3) ? (m[2] + 1) : s->modes;
            if (s->view_modes > s->modes)
                s->view_modes = s->modes;
            return 0;
        case CMD_SPEED: {
            if (s->_info_flags & IF_CMD_SPEED)
                return -1;
            s->_info_flags |= IF_CMD_SPEED;
            uint32_t sp = (uint32_t)m[1] | ((uint32_t)m[2] << 8) |
                    ((uint32_t)m[3] << 16) | ((uint32_t)m[4] << 24);
            if (sp < EV3_UART_SPEED_MIN || sp > EV3_UART_SPEED_MAX)
                return -1;
            s->speed = sp;
            return 0;
        }
        default:
            /* CMD_TYPE again means the sensor rebooted; anything else is bogus */
            return -1;
        }

    case MSG_INFO: {
        if (s->_state != ST_INFO)
            return -1;
        int32_t mode = cmd + ((cmd2 & INFO_MODE_P8) ? 8 : 0);
        int32_t info = cmd2 & ~INFO_MODE_P8;
        if (mode >= EV3_UART_MAX_MODE)
            return 0;              /* modes 8+ are not tracked, skip */

        if (info == INFO_NAME) {
            s->_info_flags &= ~IF_ALL_INFO;
            if (m[2] < 'A' || m[2] > 'z')
                return -1;
            copy_str(s->name[mode], EV3_UART_NAME_LEN, m + 2, size - 3);
            s->mode = mode;        /* INFO for this mode follows */
            s->_info_flags |= IF_INFO_NAME;
            return 0;
        }
        if (s->mode != mode)
            return -1;             /* INFO for the wrong mode */
        switch (info) {
        case INFO_RAW:
            if (s->_info_flags & IF_INFO_RAW) return -1;
            s->_info_flags |= IF_INFO_RAW;
            return 0;
        case INFO_PCT:
            if (s->_info_flags & IF_INFO_PCT) return -1;
            s->_info_flags |= IF_INFO_PCT;
            return 0;
        case INFO_SI:
            if (s->_info_flags & IF_INFO_SI) return -1;
            s->_info_flags |= IF_INFO_SI;
            return 0;
        case INFO_UNITS:
            if (s->_info_flags & IF_INFO_UNITS) return -1;
            s->_info_flags |= IF_INFO_UNITS;
            copy_str(s->units[mode], EV3_UART_NAME_LEN, m + 2, size - 3);
            return 0;
        case INFO_FORMAT:
            if (s->_info_flags & IF_INFO_FORMAT) return -1;
            s->_info_flags |= IF_INFO_FORMAT;
            /* <data-sets>, <format>, <figures>, <decimals> */
            if (m[2] == 0 || size < 7)
                return -1;
            if ((s->_info_flags & IF_REQUIRED) != IF_REQUIRED)
                return -1;
            if (m[3] > EV3_UART_DATAF)
                return -1;
            s->datasets[mode] = m[2];
            if (s->datasets[mode] > EV3_UART_MAX_DATASETS)
                s->datasets[mode] = EV3_UART_MAX_DATASETS;
            s->fmt[mode]      = m[3];
            s->figures[mode]  = m[4];
            s->decimals[mode] = m[5];
            return 0;
        default:
            return 0;              /* unknown INFO: ignore */
        }
    }

    case MSG_DATA: {
        if (s->_state != ST_DATA)
            return -1;             /* DATA before INFO complete */
        int32_t mode = cmd;
        if (mode != s->mode)
            s->mode = mode;        /* follow the sensor: a stale answer to a
                                    * superseded CMD_SELECT is legitimate */
        if (mode == s->_new_mode) {
            s->_new_mode = -1;     /* CMD_SELECT completed */
            s->_requested_mode = mode;
        }

        int32_t fmt = s->fmt[mode];
        int32_t sz  = fmt_size(fmt);
        int32_t ds  = s->datasets[mode];
        if (ds <= 0) ds = 1;
        for (int32_t i = 0; i < ds; i++) {
            int32_t off = 1 + i * sz;
            if (off + sz > size - 1)
                break;
            int32_t v = decode_le(m + off, fmt);
            if (v != s->data[mode][i])
                s->_wakeup = 1;
            s->data[mode][i] = v;
        }
        s->_last_data_ms = now;
        if (s->_num_data_err > 0)
            s->_num_data_err--;
        return 0;
    }
    }
    return 0;
}

static void handle_byte(ev3_uart_sensor_t* s, uint8_t b, uint32_t now) {
    if (s->_state == ST_SYNC) {
        /* look for CMD_TYPE, <type 1..MAX>, <checksum> */
        s->_msg[s->_msg_idx++] = b;
        if (s->_msg_idx < 3)
            return;
        uint8_t type = s->_msg[1];
        if (s->_msg[0] == (MSG_CMD | CMD_TYPE) &&
            type != 0 && type <= EV3_UART_TYPE_MAX &&
            s->_msg[2] == (uint8_t)(0xff ^ s->_msg[0] ^ type)) {
            s->type_id = type;
            s->modes = 1;
            s->view_modes = 1;
            s->mode = 0;
            memset(s->datasets, 0, sizeof(s->datasets));
            memset(s->fmt, 0, sizeof(s->fmt));
            memset(s->figures, 0, sizeof(s->figures));
            memset(s->decimals, 0, sizeof(s->decimals));
            memset(s->name, 0, sizeof(s->name));
            memset(s->units, 0, sizeof(s->units));
            s->_info_flags = IF_CMD_TYPE;
            s->_info_done = 0;
            s->_num_data_err = 0;
            s->_msg_idx = 0;
            s->_state = ST_INFO;
        } else {
            s->_msg[0] = s->_msg[1];
            s->_msg[1] = s->_msg[2];
            s->_msg_idx = 2;
        }
        return;
    }

    if (s->_state == ST_ACK_DELAY || s->_state == ST_ACK_WAIT)
        return;    /* nothing meaningful arrives while we change speed */

    if (s->_msg_idx == 0) {
        /* sometimes we get 0xFF after switching baud rates: ignore it */
        if (b == 0xff)
            return;
        int32_t type = b & MSG_TYPE_MASK;
        int32_t size = (type == MSG_SYS) ? 1 : (MSG_SIZE(b) + 2 + (type == MSG_INFO ? 1 : 0));
        if (s->_state == ST_DATA &&
            (type != MSG_DATA || size < 3 || size > EV3_UART_MAX_MESSAGE_SIZE)) {
            /* out of sync after an overrun: keep skipping until a DATA header */
            return;
        }
        if (size > EV3_UART_MAX_MESSAGE_SIZE) {
            resync(s);
            return;
        }
        s->_msg_len = size;
    }

    s->_msg[s->_msg_idx++] = b;
    if (s->_msg_idx < s->_msg_len)
        return;
    s->_msg_idx = 0;
    if (handle_msg(s, now) != 0)
        resync(s);
}

/* ---------------- public API ---------------- */

int ev3_uart_sensor_open(ev3_uart_sensor_t* s, int32_t port, int32_t report_mode) {
    if (s == NULL || port < 0 || port >= EV3_IN_PORT_COUNT)
        return -1;
    uint32_t base = ev3_input_port_uart_base(port);
    uint32_t irq  = ev3_input_port_uart_irq(port);
    if (base == 0 || irq == 0)
        return -1;

    memset(s, 0, sizeof(*s));
    s->port = port;
    s->report_mode = report_mode;
    s->_requested_mode = report_mode;
    s->_new_mode = -1;
    s->speed = EV3_UART_SPEED_MIN;
    s->_state = ST_SYNC;

    ev3_input_port_power(port, 1);
    if (ev3_input_port_uart_enable(port) != 0)
        return -1;
    s->_base = base;
    s->_irq = irq;
    ev3_uart_init(s->_base, EV3_UART_SPEED_MIN);

    _irq_owner[port] = s;
    _irq_handlers[port].handler = rx_irq;
    _irq_handlers[port].data = (ewokos_addr_t)s;
    sys_interrupt_setup(irq, &_irq_handlers[port]);
    ev3_uart_enable_irq(s->_base, EV3_IRQ_RX, EV3_IRQ_ENABLE);
    return 0;
}

void ev3_uart_sensor_close(ev3_uart_sensor_t* s) {
    if (s == NULL || s->_base == 0)
        return;
    ev3_uart_enable_irq(s->_base, EV3_IRQ_RX, EV3_IRQ_DISABLE);
    if (s->port >= 0 && s->port < EV3_IN_PORT_COUNT && _irq_owner[s->port] == s)
        _irq_owner[s->port] = NULL;
    ev3_input_port_uart_disable(s->port);
    ev3_input_port_power(s->port, 0);
    s->_base = 0;
    s->synced = 0;
    s->_state = ST_SYNC;
}

void ev3_uart_sensor_poll(ev3_uart_sensor_t* s) {
    if (s == NULL || s->_base == 0)
        return;

    uint32_t now = now_ms();
    uint8_t b;
    int32_t guard = 0;
    while (rx_pop(s, &b) && guard++ < EV3_UART_RX_RING)
        handle_byte(s, b, now);

    switch (s->_state) {
    case ST_ACK_DELAY:
        if (now - s->_ack_ms >= ACK_DELAY_MS) {
            uart_put(s, SYS_ACK);
            s->_ack_ms = now;
            s->_state = ST_ACK_WAIT;
        }
        break;

    case ST_ACK_WAIT:
        /* mdelay(4) + tty_wait_until_sent(): the ACK must have left the
         * wire at 2400 baud before the divisor changes */
        if (now - s->_ack_ms >= ACK_SETTLE_MS && ev3_uart_tx_empty(s->_base))
            link_up(s, now);
        break;

    case ST_DATA:
        /* the DATA rate is mode dependent and can be slower than the
         * keep-alive period, so silence counts against the watchdog only
         * (resync reports it once); per-tick accounting inflated errors on
         * healthy links */
        if (s->_num_data_err > EV3_UART_MAX_DATA_ERR ||
            now - s->_last_data_ms >= EV3_UART_WATCHDOG_MS) {
            resync(s);
            break;
        }
        if (now - s->_keep_alive_ms >= EV3_UART_KEEP_ALIVE_MS) {
            s->_keep_alive_ms = now;
            uart_put(s, SYS_NACK);
        }
        if (s->_new_mode >= 0 && now - s->_set_mode_ms >= EV3_UART_SET_MODE_MS) {
            if (++s->_set_mode_retries >= EV3_UART_SET_MODE_RETRY) {
                s->errors++;
                s->_new_mode = -1;     /* give up, keep whatever mode we are in */
            } else {
                s->_set_mode_ms = now;
                send_select(s, s->_new_mode);
            }
        }
        break;

    default:
        break;
    }
}

int32_t ev3_uart_sensor_select(ev3_uart_sensor_t* s, int32_t mode) {
    if (s == NULL || mode < 0 || mode >= EV3_UART_MAX_MODE)
        return -1;
    /* remember the wish so it is applied once (re)synced */
    s->_requested_mode = mode;
    if (!s->synced || s->_state != ST_DATA)
        return -1;
    if (mode >= s->modes)
        return -1;
    if (mode == s->mode && s->_new_mode < 0)
        return 0;
    start_select(s, mode, now_ms());
    return 0;
}

int32_t ev3_uart_sensor_write(ev3_uart_sensor_t* s, const uint8_t* data, int32_t count) {
    if (s == NULL || data == NULL || count <= 0 || count > EV3_UART_MAX_DATA_SIZE)
        return -1;
    if (!s->synced || s->_state != ST_DATA)
        return -1;

    /* payload is padded to the next power of two (1,2,4,8,16,32) */
    int32_t size;
    if (count <= 2)       size = count;
    else if (count <= 4)  size = 4;
    else if (count <= 8)  size = 8;
    else if (count <= 16) size = 16;
    else                  size = 32;

    uint8_t m[EV3_UART_MAX_MESSAGE_SIZE];
    memset(m, 0, sizeof(m));
    memcpy(m + 1, data, count);
    int32_t code = 0;
    while ((1 << code) < size) code++;
    m[0] = (uint8_t)(MSG_CMD | (code << 3) | CMD_WRITE);
    uint8_t cs = 0xff;
    for (int32_t i = 0; i <= size; i++)
        cs ^= m[i];
    m[size + 1] = cs;
    uart_write(s, m, size + 2);
    return count;
}

int32_t ev3_uart_sensor_value(ev3_uart_sensor_t* s, int32_t mode) {
    return ev3_uart_sensor_dataset(s, mode, 0);
}

int32_t ev3_uart_sensor_dataset(ev3_uart_sensor_t* s, int32_t mode, int32_t ds) {
    if (s == NULL) return 0;
    if (mode < 0 || mode >= EV3_UART_MAX_MODE) return 0;
    if (ds < 0 || ds >= EV3_UART_MAX_DATASETS) return 0;
    return s->data[mode][ds];
}

int32_t ev3_uart_sensor_consume_wakeup(ev3_uart_sensor_t* s) {
    if (s == NULL) return 0;
    int32_t w = s->_wakeup;
    s->_wakeup = 0;
    return w;
}
