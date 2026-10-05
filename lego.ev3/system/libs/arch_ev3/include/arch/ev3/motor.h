#ifndef __EV3_MOTOR_H__
#define __EV3_MOTOR_H__

#include <stdint.h>

/*
 * Client interface for the EV3 motor daemon (motord), mounted at /dev/motor.
 *
 * All payloads are fixed-width structs:
 *   write(fd, &motor_cmd_t, sizeof)              -> issue a command
 *   read(fd, motor_info_t[MOTOR_PORT_COUNT])      -> state of all ports
 *   dev_cntl(fd, MOTOR_CNTL_COMMAND)  in : motor_cmd_t
 *   dev_cntl(fd, MOTOR_CNTL_GET_INFO) in : motor_cmd_t{port} out: motor_info_t
 *   dev_cntl(fd, MOTOR_CNTL_GET_ALL)  out: motor_info_t[MOTOR_PORT_COUNT]
 * dev_cmd("/dev/motor", "run A 60") style text commands remain for the
 * shell only.
 *
 * Ports are addressed as A/B/C/D (0..3) and speeds/duties are signed
 * percentages in -100..100 (sign selects direction).
 */

/* dev_cntl selectors */
#define MOTOR_CNTL_COMMAND   1
#define MOTOR_CNTL_GET_INFO  2
#define MOTOR_CNTL_GET_ALL   3

/* motor_cmd_t.cmd */
#define MOTOR_CMD_RUN          1  /* arg0 duty(-100..100), arg1 stop_action   */
#define MOTOR_CMD_STOP         2  /* arg0 stop_action                         */
#define MOTOR_CMD_SET_DUTY     3  /* arg0 duty(-100..100)                     */
#define MOTOR_CMD_SET_POSITION 5  /* arg0 position(counts)                    */
/* Closed-loop helpers (see motord PID block):                            */
#define MOTOR_CMD_RUN_AT_SPEED 7  /* arg0 speed(deg/s), arg1 stop_action      */
#define MOTOR_CMD_RUN_TO_POS   8  /* arg0 target(counts), arg1 speed, arg2 act*/
#define MOTOR_CMD_HOLD         9  /* arg0 stop_action when released           */
#define MOTOR_CMD_SET_PID      11 /* arg0 kp, arg1 ki, arg2 kd (x1000 ints)   */

typedef struct {
    int32_t cmd;        /* MOTOR_CMD_*                  */
    int32_t port;       /* MOTOR_PORT_A..D              */
    int32_t arg0;
    int32_t arg1;
    int32_t arg2;
} motor_cmd_t;

/* stop_action */
#define MOTOR_STOP_COAST  0
#define MOTOR_STOP_BRAKE  1

/* PID control modes reported by MOTOR_CMD_GET_INFO. */
#define MOTOR_MODE_IDLE     0
#define MOTOR_MODE_DUTY     1   /* open-loop PWM percentage              */
#define MOTOR_MODE_SPEED    2   /* closed-loop speed (deg/s)             */
#define MOTOR_MODE_POS      3   /* closed-loop position (counts)         */
#define MOTOR_MODE_HOLD     4   /* closed-loop position lock at 0 speed  */

#define MOTOR_PORT_A  0
#define MOTOR_PORT_B  1
#define MOTOR_PORT_C  2
#define MOTOR_PORT_D  3
#define MOTOR_PORT_COUNT 4

/* EV3 tacho motors report 360 counts per output-shaft revolution
 * (1 count == 1 degree). */
#define MOTOR_COUNTS_PER_ROT  360

/* Per-port state (read / MOTOR_CNTL_GET_INFO / MOTOR_CNTL_GET_ALL). */
typedef struct {
    int32_t present;    /* motor detected on the port   */
    int32_t running;    /* 1 while power is applied     */
    int32_t duty;       /* current signed duty (-100..100) */
    int32_t position;   /* accumulated tacho counts     */
    int32_t speed;      /* measured speed in deg/s      */
    int32_t mode;       /* MOTOR_MODE_*                 */
    int32_t target;     /* setpoint (deg/s or counts)   */
    int32_t stalled;    /* 1 when driven but not moving */
} motor_info_t;

#endif
