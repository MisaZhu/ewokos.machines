/*
 * colord - EV3 color sensor driver (input port, UART mode).
 *
 * Thin wrapper over arch/ev3/uart_sensord. Modes (ev3dev lego-ev3-color):
 *
 *   COLOR_MODE_REFLECT 0  COL-REFLECT : reflected light 0..100 %
 *   COLOR_MODE_AMBIENT 1  COL-AMBIENT : ambient light 0..100 %
 *   COLOR_MODE_COLOR   2  COL-COLOR   : COLOR_NONE..COLOR_BROWN (0..7)
 *   COLOR_MODE_REF_RAW 3  REF-RAW     : value[0..1] raw reflected
 *   COLOR_MODE_RGB_RAW 4  RGB-RAW     : value[0..2] = r g b (0..1020)
 *   COLOR_MODE_CAL     5  COL-CAL
 *
 * Protocol: arch/ev3/sensor_dev.h.
 * Options: -p <1-4> input port (default 2), -m <mode> (default 2).
 */
#include <arch/ev3/port.h>
#include <arch/ev3/uart_sensor.h>
#include <arch/ev3/uart_sensord.h>
#include <arch/ev3/sensor_dev.h>

int main(int argc, char** argv) {
    ev3_uart_sensord_cfg_t cfg = {
        .name         = "colord",
        .mnt_point    = "/dev/color0",
        .type_id      = EV3_UART_TYPE_EV3_COLOR,
        .default_port = EV3_IN_PORT_2,
        .default_mode = COLOR_MODE_COLOR,
    };
    return ev3_uart_sensord_main(&cfg, argc, argv);
}
