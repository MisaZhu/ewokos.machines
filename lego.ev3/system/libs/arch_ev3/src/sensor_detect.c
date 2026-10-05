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
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <ewoksys/vdevice.h>
#include <ewoksys/proto.h>

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

/* ---------------- connection-type classification ----------------
 *
 * Faithful port of ev3dev's CON_STATE_NO_DEV tree (ev3/ev3_ports_in.c). See
 * include/arch/ev3/sensor_detect.h for the full decision-table commentary.
 */

/* After floating the port, wait long enough for the digital lines to settle
 * AND for adcd to re-sample pin 1's channel. adcd converts one channel per
 * ~2 ms loop step, so a full 16-channel cycle is ~32 ms; 45 ms guarantees the
 * pin 1 slot has been refreshed at the new float level (ev3dev's own settle is
 * SETTLE_CNT * 10 ms = 20 ms, but it reads a continuously-updated IIO buffer). */
#define CONN_SETTLE_US   45000

/* Sample pin 1's ADC channel in mV, or -1 when the ADC is unavailable / the
 * read fails. Uses the port's exact pin 1 channel: the EV3 DTS names it the
 * "pin1" io-channel (adc 6/8/10/12 for in1..in4) and adcd calibrates its
 * buffer so index N == physical channel N (touchd already samples the exact
 * pin 6 channel the same way). */
static int32_t read_pin1_mv(int fd, int port) {
    if (fd < 0)
        return -1;
    int p1 = ev3_input_port_adc_channel_pin1(port);
    if (p1 < 0 || p1 >= EV3_ADC_MAX_CH)
        return -1;
    uint16_t adc[EV3_ADC_MAX_CH];
    if (read(fd, adc, sizeof(adc)) < (int)sizeof(adc))
        return -1;
    return (int32_t)adc[p1];
}

ev3_conn_type_t ev3_sensor_conn_type(int fd, int port, int* type_id) {
    if (type_id)
        *type_id = EV3_SENSOR_TYPE_NONE;
    if (port < 0 || port >= EV3_IN_PORT_COUNT)
        return EV3_CONN_NONE;

    /* Sample in ev3dev's float state (pin 1 low, pin 2/5/6 GPIO inputs, buffer
     * off) and let the port settle before reading. */
    ev3_input_port_float(port);
    usleep(CONN_SETTLE_US);

    /* On the PRU soft-UART ports (physical 3/4) pin 5/6 are McASP serialiser
     * lines owned by the PRU, not GPIO: ev3_input_port_float() deliberately
     * leaves them muxed to McASP, so reading them here would be meaningless
     * (and re-muxing them to GPIO would storm PRU_EVTOUT interrupts). Classify
     * those ports from pin 2 + pin 1 alone by forcing both flags to 0, which
     * keeps the shared decision tree below identical for the hardware-UART
     * ports 1/2. */
    int pru = ev3_input_port_is_pru(port);

    /* pin 2/5/6 are GPIO_ACTIVE_HIGH in the DTS, so the raw level equals
     * ev3dev's gpiod_get_value(): pin 2 low => NXT family, pin 5 low => fault
     * (it has a pull-up), pin 6 high => an I2C device is pulling it up. */
    int pin2_low  = !ev3_gpio_read(ev3_input_port_gpio(port, EV3_PIN2));
    int pin5_low  = pru ? 0 : !ev3_gpio_read(ev3_input_port_gpio(port, EV3_PIN5));
    int pin6_high = pru ? 0 :  ev3_gpio_read(ev3_input_port_gpio(port, EV3_PIN6));
    int32_t pin1_mv = read_pin1_mv(fd, port);              /* -1 = no ADC     */
    int pin1_loaded = (pin1_mv >= 0 && pin1_mv < PIN1_NEAR_5V);

    /* Empty port: every pin still in its floating state (pin 1 pulled to ~5 V,
     * pin 2 high, pin 5 pulled up, pin 6 pulled down). ev3dev only enters the
     * tree when at least one flag differs; this is that guard. */
    if (!pin2_low && !pin1_loaded && !pin5_low && !pin6_high)
        return EV3_CONN_NONE;

    if (pin2_low) {
        /* NXT family: every NXT sensor ties pin 2 to GND internally. */
        if (pru)
            /* Soft-UART port: pin 6 is a McASP line we cannot read, so the
             * I2C-vs-analog split is unavailable. An NXT device on a PRU port
             * is the I2C ultrasonic (nxt-ultrasonicd bit-bangs pin 5/6 as GPIO
             * to talk to it), so report NXT_I2C; a wrong guess only costs a
             * failed I2C read that unbinds again. */
            return EV3_CONN_NXT_I2C;
        if (!pin5_low && pin6_high) {
            /* pin 5 up + pin 6 up: the NXT I2C signature. pin 1 near ground
             * would make it the NXT colour sensor instead; without the ADC we
             * cannot rule colour out, but pin 6 high with pin 5 high is the
             * I2C pull-up, so report I2C (the NXT ultrasonic path). */
            if (pin1_mv >= 0 && pin1_mv < PIN1_NEAR_GND)
                return EV3_CONN_NXT_COLOR;
            return EV3_CONN_NXT_I2C;
        }
        if (pin5_low)
            return EV3_CONN_NXT_ANALOG;    /* NXT light / analog (pin 5 low)  */
        if (pin1_mv >= 0 && pin1_mv < PIN1_NEAR_GND)
            return EV3_CONN_NXT_COLOR;     /* pin 1 grounded => NXT colour    */
        return EV3_CONN_NXT_ANALOG;        /* NXT touch / analog (pin 1 high) */
    }

    if (pin1_loaded) {
        /* EV3 family: pin 2 high, pin 1 pulled below ~5 V by the device. */
        if (pin1_mv > PIN1_NEAR_PIN2)
            return EV3_CONN_ERR;           /* motor shorted pin 1 to pin 2    */
        if (pin1_mv < PIN1_NEAR_GND)
            return EV3_CONN_EV3_UART;      /* colour / US / gyro / IR         */
        /* An ID-resistor voltage: an EV3/Analog sensor. ev3dev flags an
         * unrecognised resistor as ERR, but for gating we keep it as ANALOG so
         * a UART daemon never probes (and disturbs) an analog device - pin 1 is
         * clearly not near ground, so it is not a UART sensor either way. */
        if (type_id)
            *type_id = ev3_sensor_type_from_mv(pin1_mv);
        return EV3_CONN_EV3_ANALOG;
    }

    if (pin6_high)
        return EV3_CONN_NXT_I2C;           /* 3rd-party I2C, pin 2 not tied   */

    /* Nothing pulling pin 1 down and pin 6 low: either an empty port we could
     * not read (no ADC) or a pin 5 fault. Report NONE when the ADC is down so
     * callers still protocol-probe (a UART sensor is then not missed); with a
     * valid ADC this is the "something is holding pin 5 low" fault. */
    if (pin1_mv < 0 && !pin5_low)
        return EV3_CONN_NONE;
    return EV3_CONN_ERR;
}

int ev3_sensor_port_is_i2c(int fd, int port) {
    return ev3_sensor_conn_type(fd, port, NULL) == EV3_CONN_NXT_I2C;
}

int ev3_sensor_port_is_uart(int fd, int port) {
    return ev3_sensor_conn_type(fd, port, NULL) == EV3_CONN_EV3_UART;
}

/* The sensor daemons that share the four input ports. A daemon asks each peer
 * over its /dev node whether it has bound (or is mid-probe on) a port before
 * floating or opening it, so two daemons never drive the same line. This is the
 * same list the UART daemons use (uart_sensord.c's port_owned_by_peer). */
static const char* const _conn_peer_nodes[] = {
    "/dev/color0", "/dev/us0", "/dev/gyro0", "/dev/ir0",
    "/dev/touch0", "/dev/nxt-us0",
};

int ev3_sensor_port_busy(int port, const char* self_node) {
    for (unsigned i = 0; i < sizeof(_conn_peer_nodes) / sizeof(_conn_peer_nodes[0]); i++) {
        const char* node = _conn_peer_nodes[i];
        if (self_node && strcmp(node, self_node) == 0)
            continue;                    /* never query ourselves: it would
                                          * block waiting for our own reply   */
        if (dev_get_pid(node) <= 0)
            continue;                    /* daemon not running: holds nothing */
        proto_t ret;
        PF->init(&ret);
        ev3_sensor_data_t d;
        memset(&d, 0, sizeof(d));
        int ok = (dev_cntl(node, EV3_SENSOR_CNTL_GET_DATA, NULL, &ret) == 0 &&
                  proto_read_int(&ret) == 0 &&
                  proto_read_to(&ret, &d, sizeof(d)) == (int32_t)sizeof(d));
        PF->clear(&ret);
        if (ok && (d.port == port || d.probing == port + 1))
            return 1;
    }
    return 0;
}
