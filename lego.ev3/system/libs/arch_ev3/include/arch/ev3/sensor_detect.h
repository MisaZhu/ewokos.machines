#ifndef __EV3_SENSOR_DETECT_H__
#define __EV3_SENSOR_DETECT_H__

#include <stdint.h>

#include "port.h"
#include "sensor_dev.h"

/*
 * Input-port sensor auto-detection - mirrors ev3dev's ev3/ev3_ports_in.c.
 *
 * Pin 1 (GPIO "I_ONx") is NOT the EV3 sensor supply: it switches the ~9 V
 * battery rail onto pin 1 for NXT sensors only. EV3 sensors are powered from
 * the always-on VCC5V rail. For EV3 sensing pin 1 is therefore driven LOW
 * (ev3dev's ev3_input_port_float()); with it low the port's internal pull-up
 * holds pin 1 near 5 V and a plugged device loads it down:
 *   ~5 V          nothing attached
 *   > ~3.1 V      a motor is wrongly plugged in (pin 1 shorted to pin 2)
 *   < ~0.1 V      an EV3/UART sensor (colour/US/gyro/IR) - pulls pin 1 to
 *                 ground and carries no ID voltage, so it is identified by
 *                 the UART handshake (uart_sensord.c's protocol probe), never
 *                 from the ADC.
 *   in between    an EV3/Analog sensor's ID-resistor voltage (ev3dev
 *                 PIN1_ID_01..14; the EV3 touch sensor is 417 mV).
 *
 * The 16 raw channels are published by adcd on /dev/adc0;
 * ev3_input_port_adc_channel_pin1() gives a port's pin 1 channel. These
 * helpers let a daemon find which port its sensor is on (and follow hot-plug
 * moves). The returned type ids are the EV3_SENSOR_TYPE_* values from
 * sensor_dev.h. Only EV3/Analog sensors can be named from the ADC; the UART
 * sensors are found by the probe, so ev3_sensor_find_uart() mainly serves as
 * a presence hint and daemons must confirm the type over UART.
 */

#define EV3_ADC_MAX_CH        16    /* /dev/adc0 channel count */
#define EV3_SENSOR_PORT_NONE  (-1)  /* sensor not found on any port */

/* Open /dev/adc0 for detection. Returns an fd >= 0, or -1 on failure. */
int  ev3_sensor_adc_open(void);
void ev3_sensor_adc_close(int fd);

/*
 * Drive every input port's pin 1 to the EV3 sensing level (LOW), so the port's
 * internal pull-up lets adcd read a plugged device's ID / presence. Safe to
 * call repeatedly and harmless for ports already owned by a driver (it only
 * drives pin 1, never the pin 5/6 data lines). Call before scanning so freshly
 * inserted sensors become visible.
 */
void ev3_sensor_power_ports(void);

/*
 * The pin 1 (I_ON) level used for EV3 sensing: LOW. Every EV3-sensor path must
 * drive its port with ev3_input_port_power(port, ev3_sensor_power_level())
 * rather than a hardcoded 1 - pin 1 HIGH switches the ~9 V NXT supply onto the
 * line, which rails the ADC and disturbs EV3 sensors. (The NXT I2C path is the
 * exception: it deliberately drives pin 1 HIGH to feed NXT sensors 9 V.)
 */
int  ev3_sensor_power_level(void);

/* Map a pin 1 ID voltage (mV) to an EV3_SENSOR_TYPE_* id, NONE if unknown. */
int  ev3_sensor_type_from_mv(int32_t mv);

/*
 * Detected sensor type currently on `port` (reads its pin 1 channel through
 * fd). EV3_SENSOR_TYPE_NONE when nothing recognisable is attached or the
 * sample could not be read.
 */
int  ev3_sensor_detect_port(int fd, int port);

/*
 * First input port whose detected type equals want_type, or
 * EV3_SENSOR_PORT_NONE. `skip` (a port index, or -1 for none) is ignored so a
 * daemon can avoid a port it does not own.
 */
int  ev3_sensor_find(int fd, int want_type, int skip);

/*
 * Like ev3_sensor_find() but only considers ports wired to a hardware UART
 * (input ports 1 and 2 on the EV3). Used by the UART sensor daemons, which
 * cannot drive the PRU soft-UART ports 3 and 4.
 */
int  ev3_sensor_find_uart(int fd, int want_type, int skip);

#endif
