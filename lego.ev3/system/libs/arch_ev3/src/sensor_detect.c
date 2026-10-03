/*
 * EV3 input-port sensor auto-detection - see include/arch/ev3/sensor_detect.h.
 *
 * This mirrors ev3dev's ev3/ev3_ports_in.c, which is the authoritative model:
 *
 *   - Pin 1 (GPIO "I_ONx") is NOT the EV3 sensor supply. It switches the ~9 V
 *     battery rail onto pin 1, which only NXT sensors want. EV3 sensors are
 *     powered from the always-on VCC5V rail. So for EV3 detection pin 1 must
 *     be driven LOW (ev3dev's ev3_input_port_float() does exactly
 *     gpiod_direction_output(pin1_gpio, 0)); driving it HIGH injects 9 V and
 *     both rails the ADC and disturbs the sensor.
 *   - With pin 1 low, the port pulls pin 1 up to ~5 V through an internal
 *     resistor. A plugged device loads it down:
 *       pin1 >= ~4.9 V  -> nothing attached
 *       pin1 >  ~3.1 V  -> a motor is wrongly plugged in (pin1 shorted to pin2)
 *       pin1 <  ~0.1 V  -> an EV3/UART sensor (colour/US/gyro/IR): these pull
 *                          pin 1 straight to ground and carry NO ID voltage,
 *                          so their type can only be read over the UART
 *                          handshake (see uart_sensord.c's protocol probe).
 *       otherwise       -> an EV3/Analog sensor presenting an ID-resistor
 *                          voltage (ev3dev's PIN1_ID_01..14, 206..2826 mV;
 *                          the EV3 touch sensor is ID_02 = 417 mV).
 *
 * Consequence: the ADC path can name an EV3/Analog sensor and can tell that
 * "some" EV3/UART sensor is present, but it cannot tell colour from US from
 * gyro from IR - the UART probe does that. The ADC is therefore only used to
 * confirm presence / classify analog sensors; UART daemons bind via the probe.
 */
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#include "../include/arch/ev3/gpio.h"
#include "../include/arch/ev3/port.h"
#include "../include/arch/ev3/sensor_dev.h"
#include "../include/arch/ev3/sensor_detect.h"

#define ADC_DEV        "/dev/adc0"

/* Pin-1 voltage thresholds, straight from ev3dev ev3_ports_in.c. */
#define PIN1_NEAR_5V    4900   /* >= this: pin 1 unloaded, nothing attached   */
#define PIN1_NEAR_PIN2  3100   /* > this : pin 1 shorted to pin 2 (a motor)   */
#define PIN1_NEAR_GND    100   /* < this : an EV3/UART sensor pulls pin 1 low */

/* Tolerance around each analog ID-resistor nominal (mV). ev3dev allows +/-50. */
#define EV3_ID_TOL_MV     50

/* Pin 1 level used for EV3 sensing: LOW. I_ON high would put ~9 V (the NXT
 * supply) on pin 1, which rails the ADC and disturbs EV3 sensors. */
#define EV3_PIN1_LEVEL     0

struct id_ent {
    int     type;      /* EV3_SENSOR_TYPE_* */
    int32_t mv;        /* nominal pin 1 analog ID-resistor voltage */
};

/* EV3/Analog ID-resistor voltages (ev3dev PIN1_ID_01..14). Only the sensors
 * that have a daemon in this tree and are analog-identifiable are listed; the
 * EV3 touch sensor is ev3dev's ID_02 = 417 mV. The EV3 colour/US/gyro/IR
 * sensors are EV3/UART devices that pull pin 1 to ground and have NO ID
 * voltage, so they are deliberately absent here - they are identified by the
 * UART probe, not by this table. NXT sensors are found via pin 2, not pin 1. */
static const struct id_ent _ids[] = {
    { EV3_SENSOR_TYPE_EV3_TOUCH, 417 },    /* ev3dev PIN1_ID_02 */
};

int ev3_sensor_adc_open(void) {
    return open(ADC_DEV, O_RDONLY | O_NONBLOCK);
}

void ev3_sensor_adc_close(int fd) {
    if (fd >= 0)
        close(fd);
}

/* Pin 1 (I_ON) level for EV3 sensing. Fixed LOW per ev3dev: pin 1 is the 9 V
 * NXT supply switch, not the EV3 sensor power, and low is the state in which
 * the port's internal pull-up lets a plugged device show its ID / presence.
 * Every path that touches a port - ev3_uart_sensor_open()/close(), touchd's
 * bind() and i2c_gpio - must use this level via ev3_sensor_power_level()
 * instead of hardcoding 1 (which would inject 9 V). */
int ev3_sensor_power_level(void) {
    return EV3_PIN1_LEVEL;
}

/* Drive every input port's pin 1 to the EV3 sensing level (low). Safe to call
 * repeatedly and harmless for ports already owned by a driver - it only drives
 * pin 1, never the pin 5/6 data lines. */
void ev3_sensor_power_ports(void) {
    for (int p = 0; p < EV3_IN_PORT_COUNT; p++)
        ev3_input_port_power(p, EV3_PIN1_LEVEL);
}

int ev3_sensor_type_from_mv(int32_t mv) {
    int best = EV3_SENSOR_TYPE_NONE;
    int32_t bestd = EV3_ID_TOL_MV + 1;
    for (unsigned i = 0; i < sizeof(_ids) / sizeof(_ids[0]); i++) {
        int32_t d = mv - _ids[i].mv;
        if (d < 0) d = -d;
        if (d < bestd) {
            bestd = d;
            best = _ids[i].type;
        }
    }
    if (bestd > EV3_ID_TOL_MV)
        return EV3_SENSOR_TYPE_NONE;
    return best;
}

int ev3_sensor_detect_port(int fd, int port) {
    if (fd < 0 || port < 0 || port >= EV3_IN_PORT_COUNT)
        return EV3_SENSOR_TYPE_NONE;
    int p1 = ev3_input_port_adc_channel_pin1(port);
    if (p1 < 0 || p1 >= EV3_ADC_MAX_CH)
        return EV3_SENSOR_TYPE_NONE;

    uint16_t adc[EV3_ADC_MAX_CH];
    if (read(fd, adc, sizeof(adc)) < (int)sizeof(adc))
        return EV3_SENSOR_TYPE_NONE;

    /* Consider pin 1 and its immediate neighbours to absorb a possible
     * one-step rotation in adcd's channel->buffer mapping (pin 1 is the ID
     * line per ev3dev's DTS; pin 6 is the data line and carries no ID).
     *
     * Only an EV3/Analog ID-resistor voltage positively names a device here.
     * Everything else yields NONE:
     *   - empty port: pin 1 idles at the ~5 V pull-up (>= PIN1_NEAR_5V)
     *   - motor wrongly plugged in: pin 1 shorted toward pin 2 (> NEAR_PIN2)
     *   - EV3/UART sensor (colour/US/gyro/IR): pulls pin 1 to ground
     *     (< PIN1_NEAR_GND) and carries NO ID voltage, so it is identified by
     *     the protocol probe, never here. Naming a UART type from voltage is
     *     impossible and would let daemons cross-bind each other's ports. */
    for (int adj = 1; adj >= -1; adj--) {
        int c = (p1 + adj + EV3_ADC_MAX_CH) % EV3_ADC_MAX_CH;
        int32_t mv = adc[c];
        if (mv < PIN1_NEAR_GND || mv >= PIN1_NEAR_5V)
            continue;               /* nothing loaded, or a UART sensor */
        if (mv > PIN1_NEAR_PIN2)
            continue;               /* motor shorted pin 1 to pin 2 */
        int t = ev3_sensor_type_from_mv(mv);
        if (t != EV3_SENSOR_TYPE_NONE)
            return t;
    }
    return EV3_SENSOR_TYPE_NONE;
}

static int find_common(int fd, int want_type, int skip, int uart_only) {
    if (fd < 0)
        return EV3_SENSOR_PORT_NONE;
    for (int p = 0; p < EV3_IN_PORT_COUNT; p++) {
        if (p == skip)
            continue;
        if (uart_only && ev3_input_port_uart_base(p) == 0)
            continue;
        if (ev3_sensor_detect_port(fd, p) == want_type)
            return p;
    }
    return EV3_SENSOR_PORT_NONE;
}

int ev3_sensor_find(int fd, int want_type, int skip) {
    return find_common(fd, want_type, skip, 0);
}

int ev3_sensor_find_uart(int fd, int want_type, int skip) {
    return find_common(fd, want_type, skip, 1);
}
