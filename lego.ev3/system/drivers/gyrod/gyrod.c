/*
 * gyrod - EV3 gyroscope sensor driver (input port, UART mode).
 *
 * Thin wrapper over arch/ev3/uart_sensord. Modes (ev3dev lego-ev3-gyro):
 *
 *   GYRO_MODE_ANG  0  GYRO-ANG  : angle in degrees, wraps at +/-32767
 *   GYRO_MODE_RATE 1  GYRO-RATE : angular rate in deg/s
 *   GYRO_MODE_FAS  2  GYRO-FAS  : fast rate (unscaled)
 *   GYRO_MODE_GA   3  GYRO-G&A  : value[0] = angle, value[1] = rate
 *   GYRO_MODE_CAL  4  GYRO-CAL  : calibration
 *
 * EV3_SENSOR_CMD_RESET re-zeroes the angle by bouncing the mode, the way
 * ev3dev does it. Protocol: arch/ev3/sensor_dev.h.
 *
 * Options: -p <1-4> optional port restriction (default: auto-detect the port
 * from the sensor ID voltage and follow it on hot-plug), -m <mode> (default 0).
 */
#include <arch/ev3/port.h>
#include <arch/ev3/uart_sensor.h>
#include <arch/ev3/uart_sensord.h>
#include <arch/ev3/sensor_dev.h>

int main(int argc, char** argv) {
    ev3_uart_sensord_cfg_t cfg = {
        .name         = "gyrod",
        .mnt_point    = "/dev/gyro0",
        .type_id      = EV3_UART_TYPE_EV3_GYRO,
        .default_port = EV3_IN_PORT_2,
        .default_mode = GYRO_MODE_ANG,
    };
    return ev3_uart_sensord_main(&cfg, argc, argv);
}
