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

/*
 * ---------------- connection-type classification ----------------
 *
 * A faithful port of ev3dev's CON_STATE_NO_DEV decision tree
 * (ev3/ev3_ports_in.c). Where the helpers above name a sensor purely from the
 * pin 1 analog ID voltage, this classifier reads the port's DIGITAL pin states
 * (pin 2, pin 5, pin 6) together with pin 1 to separate the four sensor
 * families exactly the way the LEGO / ev3dev driver does - which is what lets
 * every one of the four input ports tell an I2C sensor from a UART sensor:
 *
 *   pin 2 LOW            -> an NXT-family device (NXT sensors tie pin 2 to GND
 *                           internally). With pin 5 HIGH + pin 6 HIGH and pin 1
 *                           not near ground this is the NXT I2C sensor (e.g. the
 *                           NXT ultrasonic); pin 1 near ground makes it NXT
 *                           colour; pin 5 LOW makes it NXT analog.
 *   pin 1 loaded (<4.9V) -> an EV3 sensor. pin 1 near ground = EV3/UART
 *                           (colour/US/gyro/IR); an ID-resistor voltage =
 *                           EV3/Analog (touch); >3.1V = a motor wrongly plugged
 *                           into an input port (pin 1 shorted to pin 2).
 *   pin 6 HIGH only      -> a 3rd-party I2C sensor that does not tie pin 2.
 *   otherwise            -> fault (something is pulling pin 5 low).
 *
 * pin 2/5/6 are declared GPIO_ACTIVE_HIGH in the EV3 DTS, so a raw
 * ev3_gpio_read() equals ev3dev's gpiod_get_value() and the pin flags map 1:1.
 */
typedef enum {
    EV3_CONN_NONE = 0,     /* nothing attached (every pin in the float state) */
    EV3_CONN_NXT_ANALOG,   /* NXT touch / light / sound (analog on pin 1)     */
    EV3_CONN_NXT_COLOR,    /* NXT colour (pin 2 low, pin 1 near ground)       */
    EV3_CONN_NXT_I2C,      /* NXT / 3rd-party I2C sensor (e.g. NXT ultrasonic)*/
    EV3_CONN_EV3_ANALOG,   /* EV3 analog ID resistor (e.g. EV3 touch)         */
    EV3_CONN_EV3_UART,     /* EV3 UART sensor (colour / US / gyro / IR)       */
    EV3_CONN_ERR,          /* motor on an input port, or a pin 5 fault        */
} ev3_conn_type_t;

/*
 * Classify what is attached to `port` and return its connection type. The port
 * is first put in the float state (ev3_input_port_float) and left ~10 ms to
 * settle, then pin 2/5/6 (digital) and pin 1 (ADC via fd) are sampled and run
 * through the ev3dev decision tree.
 *
 * MUST NOT be called on a port another daemon has bound - floating it would
 * tear down that daemon's live UART / I2C link. Gate with ev3_sensor_port_busy()
 * first.
 *
 * For EV3_CONN_EV3_ANALOG, *type_id (when non-NULL) receives the matching
 * EV3_SENSOR_TYPE_* analog id from the pin 1 voltage, or EV3_SENSOR_TYPE_NONE
 * when the resistor is not a sensor this tree knows. fd may be < 0 (ADC not up
 * yet): the pin 1 voltage is then unknown, so only the GPIO-decidable families
 * (NXT_I2C: pin 2 low + pin 6 high; and the 3rd-party pin6-high case) can be
 * named positively - EV3_UART / EV3_ANALOG need the ADC and fall back to NONE.
 */
ev3_conn_type_t ev3_sensor_conn_type(int fd, int port, int* type_id);

/* Convenience wrappers over ev3_sensor_conn_type() (same float/settle cost). */
int  ev3_sensor_port_is_i2c(int fd, int port);   /* 1 when NXT_I2C   */
int  ev3_sensor_port_is_uart(int fd, int port);  /* 1 when EV3_UART  */

/*
 * 1 when a peer sensor daemon has bound `port` or is mid-probe on it, so the
 * caller must keep its hands off - floating or opening the port would reprogram
 * the shared line and tear down the peer's live link. self_node (this daemon's
 * own /dev node, or NULL) is skipped so a daemon never queries itself. This is
 * the shared form of the UART daemons' port_owned_by_peer(); it asks each peer
 * over its /dev node with EV3_SENSOR_CNTL_GET_DATA and treats a bound port
 * (ev3_sensor_data_t.port) or a transient probe claim (.probing == port + 1) as
 * busy. A peer that is not running, or that answers with a different struct,
 * holds nothing.
 */
int  ev3_sensor_port_busy(int port, const char* self_node);

#endif
