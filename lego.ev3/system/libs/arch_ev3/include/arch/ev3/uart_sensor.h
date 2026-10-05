#ifndef __EV3_UART_SENSOR_H__
#define __EV3_UART_SENSOR_H__

#include <stdint.h>

/*
 * EV3 UART sensor protocol (lms2012 d_uart_mod.c / ev3dev
 * ev3_uart_sensor_ld.c). This module implements the host side of the
 * protocol so that gyrod / colord / ird / ultrasonicd can all share the
 * same framing, checksum, sync/keep-alive and INFO parsing code.
 *
 * Protocol summary (see ev3dev ev3_uart_sensor_ld.c receive_buf2()):
 *   - device announces itself at 2400 baud with CMD_TYPE, CMD_MODES,
 *     CMD_SPEED and a set of INFO_* messages, terminated by SYS_ACK (0x04),
 *   - host validates that all required info arrived, replies SYS_ACK,
 *     waits >= 4 ms for the byte to leave the wire and both sides switch
 *     to the speed reported in CMD_SPEED (usually 57600 or 460800),
 *   - host keeps the link alive with SYS_NACK (0x02) every 100 ms,
 *   - device sends MESSAGE_DATA frames which carry the sensor readings;
 *     more than EV3_UART_MAX_DATA_ERR consecutive bad frames (or a
 *     watchdog timeout) drop the link back to 2400 baud and re-sync.
 *
 * A message frame is: HEADER [DATA...] CHECKSUM.
 *   HEADER bits 7-6 : message class (SYS/CMD/INFO/DATA)
 *   HEADER bits 5-3 : length code (0 -> 1 byte, 1 -> 2, ..., 5 -> 32)
 *   HEADER bits 2-0 : command / mode
 *   CHECKSUM        : 0xff XOR all preceding bytes of the message.
 *
 * RX is interrupt driven: the UART IRQ of the input port drains the 16550
 * FIFO into a ring buffer, ev3_uart_sensor_poll() consumes the ring from
 * loop_step and drives the state machine. Only the poll side sends.
 */

/* Device type IDs (subset of ev3dev "Assigned Numbers"). */
#define EV3_UART_TYPE_UNKNOWN    0
#define EV3_UART_TYPE_NXT_TOUCH  1
#define EV3_UART_TYPE_NXT_LIGHT  2
#define EV3_UART_TYPE_NXT_SOUND  3
#define EV3_UART_TYPE_NXT_COLOR  4
#define EV3_UART_TYPE_NXT_US     5
#define EV3_UART_TYPE_L_MOTOR    7
#define EV3_UART_TYPE_M_MOTOR    8
#define EV3_UART_TYPE_EV3_TOUCH  16
#define EV3_UART_TYPE_EV3_COLOR  29
#define EV3_UART_TYPE_EV3_US     30
#define EV3_UART_TYPE_EV3_GYRO   32
#define EV3_UART_TYPE_EV3_IR     33
#define EV3_UART_TYPE_MAX        101

#define EV3_UART_MAX_MODE        8
#define EV3_UART_MAX_DATASETS    8
#define EV3_UART_NAME_LEN        12
#define EV3_UART_MAX_DATA_SIZE   32
#define EV3_UART_MAX_MESSAGE_SIZE (EV3_UART_MAX_DATA_SIZE + 2)
#define EV3_UART_MAX_DATA_ERR    6
#define EV3_UART_KEEP_ALIVE_MS   100
#define EV3_UART_WATCHDOG_MS     1000
#define EV3_UART_SET_MODE_MS     50
#define EV3_UART_SET_MODE_RETRY  10

#define EV3_UART_SPEED_MIN       2400
#define EV3_UART_SPEED_MID       57600
#define EV3_UART_SPEED_MAX       460800

/* INFO_FORMAT <format> values. */
#define EV3_UART_DATA8   0
#define EV3_UART_DATA16  1
#define EV3_UART_DATA32  2
#define EV3_UART_DATAF   3

/* RX ring size: must be a power of two. 460800 baud ~ 46 bytes/ms; a
 * loop_step of a few ms plus a full 34-byte frame fits comfortably. */
#define EV3_UART_RX_RING 512

typedef struct {
    /* configuration */
    int32_t  port;             /* input port index 0..3 (EV3_IN_PORT_x) */
    int32_t  report_mode;      /* mode auto-selected after sync, -1 = keep default */

    /* link status */
    int32_t  synced;           /* 1 once info is complete and speed switched */
    int32_t  type_id;
    int32_t  modes;            /* total modes advertised by CMD_MODES */
    int32_t  view_modes;       /* modes shown on the brick (CMD_MODES byte 2) */
    int32_t  mode;             /* mode of the last DATA frame */
    uint32_t speed;            /* negotiated baud rate after sync */
    uint32_t errors;           /* total protocol errors since open */
    uint32_t resyncs;          /* times the link fell back to 2400 baud */

    /* per-mode info gathered from INFO_* messages */
    int32_t  datasets[EV3_UART_MAX_MODE];
    int32_t  fmt[EV3_UART_MAX_MODE];
    int32_t  figures[EV3_UART_MAX_MODE];
    int32_t  decimals[EV3_UART_MAX_MODE];
    char     name[EV3_UART_MAX_MODE][EV3_UART_NAME_LEN];
    char     units[EV3_UART_MAX_MODE][EV3_UART_NAME_LEN];

    /* last decoded sample per mode/dataset (float values are cast to int32) */
    int32_t  data[EV3_UART_MAX_MODE][EV3_UART_MAX_DATASETS];

    /* internal state - not for client use */
    uint32_t _base;
    uint32_t _irq;
    int32_t  _state;
    int32_t  _info_done;
    uint32_t _info_flags;
    int32_t  _requested_mode;   /* mode the client asked for, restored after resync */
    int32_t  _new_mode;         /* CMD_SELECT in flight, -1 = none */
    int32_t  _set_mode_retries;
    uint32_t _set_mode_ms;
    uint32_t _keep_alive_ms;
    uint32_t _last_data_ms;
    uint32_t _ack_ms;
    int32_t  _num_data_err;
    int32_t  _ignore_ff;
    int32_t  _msg_idx;
    int32_t  _msg_len;
    uint8_t  _msg[EV3_UART_MAX_MESSAGE_SIZE + 2];
    int32_t  _wakeup;

    /* RX ring shared with the UART interrupt handler */
    volatile uint32_t _rx_head;   /* written by IRQ */
    volatile uint32_t _rx_tail;   /* written by poll */
    volatile uint32_t _rx_overrun;
    volatile uint8_t  _rx_ring[EV3_UART_RX_RING];
} ev3_uart_sensor_t;

/*
 * Power the port, mux it to the hardware UART, install the RX interrupt
 * handler and start listening at 2400 baud. Only input ports 1 and 2 have
 * hardware UARTs on the EV3 (see arch/ev3/port.h); ports 3/4 return -1.
 * report_mode >= 0 is selected as soon as the link is up (and re-selected
 * after every resync).
 */
int  ev3_uart_sensor_open(ev3_uart_sensor_t* s, int32_t port, int32_t report_mode);

/* Disable RX interrupt and the line buffer, remove power from the sensor. */
void ev3_uart_sensor_close(ev3_uart_sensor_t* s);

/*
 * Consume the RX ring, drive the protocol state machine, send the periodic
 * keep-alive NACK, handle CMD_SELECT retries and the link watchdog. Call
 * from loop_step every few milliseconds.
 */
void ev3_uart_sensor_poll(ev3_uart_sensor_t* s);

/*
 * Request a mode switch (CMD_SELECT). Returns 0 if the request was queued,
 * -1 if the link is down or the mode is invalid. Completion is detected in
 * poll when a DATA frame for the new mode arrives; the request is retried
 * every EV3_UART_SET_MODE_MS up to EV3_UART_SET_MODE_RETRY times.
 */
int32_t ev3_uart_sensor_select(ev3_uart_sensor_t* s, int32_t mode);

/*
 * Send raw bytes to the sensor as CMD_WRITE (used by e.g. the gyro reset
 * and the IR beacon channel commands). count must be 1..32.
 */
int32_t ev3_uart_sensor_write(ev3_uart_sensor_t* s, const uint8_t* data, int32_t count);

/* Dataset 0 of a mode (the most common case). */
int32_t ev3_uart_sensor_value(ev3_uart_sensor_t* s, int32_t mode);

/* Any dataset of a mode (RGB-RAW / IR-SEEK / IR-REMOTE return several). */
int32_t ev3_uart_sensor_dataset(ev3_uart_sensor_t* s, int32_t mode, int32_t ds);

/* Return and clear the "value changed" flag (for vfs_wakeup integration). */
int32_t ev3_uart_sensor_consume_wakeup(ev3_uart_sensor_t* s);

#endif
