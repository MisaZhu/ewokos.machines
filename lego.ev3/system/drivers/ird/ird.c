/*
 * ird - EV3 infrared sensor (UART, type 33) on input port 2.
 *
 * Modes (arch/ev3/sensor_dev.h): IR_MODE_PROX / SEEK / REMOTE / ...
 * All read/write/dev_cntl payloads are fixed-width structs; the shared
 * implementation lives in libarch_ev3 (uart_sensord.c).
 */
#include <arch/ev3/port.h>
#include <arch/ev3/uart_sensor.h>
#include <arch/ev3/uart_sensord.h>
#include <arch/ev3/sensor_dev.h>

int main(int argc, char** argv) {
    ev3_uart_sensord_cfg_t cfg = {
        .name         = "ird",
        .mnt_point    = "/dev/ir0",
        .type_id      = EV3_UART_TYPE_EV3_IR,
        .default_port = EV3_IN_PORT_2,
        .default_mode = IR_MODE_PROX,
    };
    return ev3_uart_sensord_main(&cfg, argc, argv);
}
