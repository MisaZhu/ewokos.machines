/*
 * ultrasonicd - EV3 ultrasonic sensor (UART, type 30) on input port 2.
 *
 * Modes (arch/ev3/sensor_dev.h): US_MODE_DIST_CM (value[0] = mm) /
 * DIST_IN / LISTEN / ...
 * All read/write/dev_cntl payloads are fixed-width structs; the shared
 * implementation lives in libarch_ev3 (uart_sensord.c).
 */
#include <arch/ev3/port.h>
#include <arch/ev3/uart_sensor.h>
#include <arch/ev3/uart_sensord.h>
#include <arch/ev3/sensor_dev.h>

int main(int argc, char** argv) {
    ev3_uart_sensord_cfg_t cfg = {
        .name         = "ultrasonicd",
        .mnt_point    = "/dev/us0",
        .type_id      = EV3_UART_TYPE_EV3_US,
        .default_port = EV3_IN_PORT_2,
        .default_mode = US_MODE_DIST_CM,
    };
    return ev3_uart_sensord_main(&cfg, argc, argv);
}
