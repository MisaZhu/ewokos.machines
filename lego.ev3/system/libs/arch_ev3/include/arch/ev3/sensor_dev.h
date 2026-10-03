#ifndef __EV3_SENSOR_DEV_H__
#define __EV3_SENSOR_DEV_H__

#include <stdint.h>

/*
 * Common client protocol for the EV3 sensor daemons
 * (touchd, gyrod, colord, ird, ultrasonicd, nxt-ultrasonicd).
 *
 *   read(fd, &ev3_sensor_data_t, sizeof)   -> latest sample
 *   write(fd, &ev3_sensor_cmd_t, sizeof)   -> mode switch / command
 *   dev_cntl(fd, EV3_SENSOR_CNTL_GET_DATA)     out: ev3_sensor_data_t
 *   dev_cntl(fd, EV3_SENSOR_CNTL_SET_MODE)     in : ev3_sensor_cmd_t
 *   dev_cntl(fd, EV3_SENSOR_CNTL_COMMAND)      in : ev3_sensor_cmd_t
 *
 * All payloads are fixed-width little-endian structs; the text dev_cmd
 * interface of each daemon is for shell debugging only.
 */

#define EV3_SENSOR_MAX_VALUES  8

/* sensor classes (UART sensors reuse their ev3dev type id) */
#define EV3_SENSOR_TYPE_NONE       0
#define EV3_SENSOR_TYPE_NXT_TOUCH  1
#define EV3_SENSOR_TYPE_NXT_US     5
#define EV3_SENSOR_TYPE_EV3_TOUCH  16
#define EV3_SENSOR_TYPE_EV3_COLOR  29
#define EV3_SENSOR_TYPE_EV3_US     30
#define EV3_SENSOR_TYPE_EV3_GYRO   32
#define EV3_SENSOR_TYPE_EV3_IR     33

typedef struct {
    int32_t type;         /* EV3_SENSOR_TYPE_*                         */
    int32_t port;         /* input port 0..3                           */
    int32_t connected;    /* 1 when a sensor is detected / UART synced */
    int32_t mode;         /* current mode (per-sensor *_MODE_*)        */
    int32_t nvalues;      /* valid entries in value[]                  */
    int32_t value[EV3_SENSOR_MAX_VALUES];
    int32_t raw_mv;       /* analog sensors: pin voltage in mV         */
    int32_t errors;       /* protocol error counter                    */
    int64_t timestamp_ms; /* kernel ms of the last update              */
    int32_t probing;      /* UART daemons only: 0 = not probing, else the
                           * port index + 1 the daemon is actively probing
                           * right now. A transient claim so peer daemons keep
                           * off that port; it is NOT a binding (see port).
                           * Encoded +1 so a zero-initialised struct (touchd,
                           * nxt-ultrasonicd) safely means "not probing". */
} ev3_sensor_data_t;

typedef struct {
    int32_t cmd;          /* EV3_SENSOR_CMD_*                          */
    int32_t arg0;
    int32_t arg1;
    int32_t arg2;
} ev3_sensor_cmd_t;

/* ev3_sensor_cmd_t.cmd */
#define EV3_SENSOR_CMD_SET_MODE   1   /* arg0 = mode                       */
#define EV3_SENSOR_CMD_RESET      2   /* gyro: re-calibrate (mode cycle)   */
#define EV3_SENSOR_CMD_RAW_WRITE  3   /* UART: CMD_WRITE arg0..2 as bytes  */

/* dev_cntl ids */
#define EV3_SENSOR_CNTL_GET_DATA  1
#define EV3_SENSOR_CNTL_SET_MODE  2
#define EV3_SENSOR_CNTL_COMMAND   3

/* ---- touch (analog, pin 6) ---- */
#define TOUCH_MODE_TOUCH   0   /* value[0] = pressed (0/1)             */

/* ---- gyro (UART type 32) ---- */
#define GYRO_MODE_ANG      0   /* value[0] = angle deg                 */
#define GYRO_MODE_RATE     1   /* value[0] = rate deg/s                */
#define GYRO_MODE_FAS      2   /* value[0] = fast rate                 */
#define GYRO_MODE_GA       3   /* value[0] = angle, value[1] = rate    */
#define GYRO_MODE_CAL      4

/* ---- color (UART type 29) ---- */
#define COLOR_MODE_REFLECT 0   /* value[0] = 0..100 %                  */
#define COLOR_MODE_AMBIENT 1   /* value[0] = 0..100 %                  */
#define COLOR_MODE_COLOR   2   /* value[0] = COLOR_*                   */
#define COLOR_MODE_REF_RAW 3   /* value[0..1]                          */
#define COLOR_MODE_RGB_RAW 4   /* value[0..2] = r g b (0..1020)        */
#define COLOR_MODE_CAL     5

#define COLOR_NONE   0
#define COLOR_BLACK  1
#define COLOR_BLUE   2
#define COLOR_GREEN  3
#define COLOR_YELLOW 4
#define COLOR_RED    5
#define COLOR_WHITE  6
#define COLOR_BROWN  7

/* ---- infrared (UART type 33) ---- */
#define IR_MODE_PROX       0   /* value[0] = 0..100 proximity          */
#define IR_MODE_SEEK       1   /* value[2c] = heading, value[2c+1] = distance, c = 0..3 */
#define IR_MODE_REMOTE     2   /* value[c] = button code for channel c */
#define IR_CHANNELS        4

/* ---- ultrasonic (UART type 30) ---- */
#define US_MODE_DIST_CM    0   /* value[0] = mm                        */
#define US_MODE_DIST_IN    1   /* value[0] = 0.1 inch                  */
#define US_MODE_LISTEN     2   /* value[0] = other US detected (0/1)   */
#define US_MODE_SI_CM      3
#define US_MODE_SI_IN      4

/* ---- NXT ultrasonic (I2C, addr 0x01) ---- */
#define NXTUS_MODE_OFF        0
#define NXTUS_MODE_SINGLE     1   /* value[0] = cm, one ping per read */
#define NXTUS_MODE_CONTINUOUS 2   /* value[0] = cm                    */

#endif
