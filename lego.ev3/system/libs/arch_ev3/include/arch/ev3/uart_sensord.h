#ifndef __EV3_UART_SENSORD_H__
#define __EV3_UART_SENSORD_H__

#include <stdint.h>

/*
 * Generic vdevice daemon for EV3 UART sensors (gyro / color / IR /
 * ultrasonic). Each daemon is a few lines: fill an ev3_uart_sensord_cfg_t
 * and call ev3_uart_sensord_main(). The daemon speaks the fixed-width
 * protocol from arch/ev3/sensor_dev.h:
 *
 *   read(fd, &ev3_sensor_data_t, sizeof)      latest sample of the
 *                                             current mode
 *   write(fd, &ev3_sensor_cmd_t, sizeof)      SET_MODE / RESET / RAW_WRITE
 *   dev_cntl(EV3_SENSOR_CNTL_GET_DATA)        out: ev3_sensor_data_t
 *   dev_cntl(EV3_SENSOR_CNTL_SET_MODE)        in : ev3_sensor_cmd_t
 *   dev_cntl(EV3_SENSOR_CNTL_COMMAND)         in : ev3_sensor_cmd_t
 *
 * Command line handled by ev3_uart_sensord_main():
 *   -p <1-4>   optional: restrict detection to one port (only 1 and 2 have a
 *              hardware UART). By default the daemon auto-detects the port
 *              from the sensor's pin 1 ID voltage and follows it on hot-plug.
 *   -m <mode>  initial mode
 *   [mount]    mount point
 *
 * Port auto-detection needs adcd (/dev/adc0) running. The daemon reports the
 * port it bound to in ev3_sensor_data_t.port (-1 while it is still searching).
 *
 * Text interface (dev.cmd) for the shell: "info", "mode <n>", "reset", "scan".
 */
typedef struct {
    const char* name;          /* vdevice description, e.g. "gyrod"        */
    const char* mnt_point;     /* default mount point                      */
    int32_t     type_id;       /* EV3_UART_TYPE_* to scan for (== the pin 1
                                * ID type; also reported while searching)   */
    int32_t     default_port;  /* legacy, ignored: the port is auto-detected */
    int32_t     default_mode;
} ev3_uart_sensord_cfg_t;

int ev3_uart_sensord_main(const ev3_uart_sensord_cfg_t* cfg, int argc, char** argv);

#endif
