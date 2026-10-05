#ifndef __EV3_PORT_H__
#define __EV3_PORT_H__

#include <stdint.h>

typedef enum {
	EV3_ANALOG,
	EV3_I2C,
	EV3_UART,
	EV3_PWM,
}EV3_PORT_MODE;

/*
 * EV3 input (sensor) and output (motor) port helpers.
 *
 * All GPIO numbers and ADC channels come from the mainline
 * da850-lego-ev3.dts "ev3-ports" node (pin = gpio_bank * 16 + n).
 *
 *            in1   in2   in3   in4        outA  outB  outC  outD
 *  pin1      138   140   137   100  (I_ON)  63    33   104    83  (xIN0)
 *  pin2       34   143   123   120  (DET)   54     3    89    90  (xIN1)
 *  pin5        2    14    12     1  (SCL)   91    88    93   105  (tacho INT, active low)
 *  pin6       15    13    30    31  (SDA)    4    41    62    40  (tacho DIR, active low)
 *  pin5-det   -     -     -     -           84    37    56    95
 *  buf-ena   139   142   121   122  (active low)
 *  ADC pin1    6     8    10    12
 *  ADC pin6    5     7     9    11
 *  ADC pin5   -     -     -     -            1     0    13    14
 *  UART     UART1 UART0  PRU   PRU
 */

/* ---- Input (sensor) ports 1..4 ---- */
#define EV3_IN_PORT_1  0
#define EV3_IN_PORT_2  1
#define EV3_IN_PORT_3  2
#define EV3_IN_PORT_4  3
#define EV3_IN_PORT_COUNT 4

/* Connector pin ids for ev3_input_port_gpio(). */
#define EV3_PIN1  1
#define EV3_PIN2  2
#define EV3_PIN5  5
#define EV3_PIN6  6

/* GPIO number of a connector pin, -1 if not a GPIO. */
int      ev3_input_port_gpio(int port, int pin);

/* Switch the port's sensor supply on/off (pin 1). */
void     ev3_input_port_power(int port, int on);

/* Digital detect line (pin 2). */
int      ev3_input_port_detect(int port);

/*
 * Put the port in ev3dev's "float" detection state, mirroring
 * ev3_input_port_float() in ev3/ev3_ports_in.c: pin 1 driven LOW (the EV3
 * sensing level, never the ~9 V NXT supply), pin 2/5/6 as digital inputs and
 * the line buffer disabled (so pin 5/6 read as plain GPIO levels rather than
 * being driven by a UART). This is the state ev3dev samples to classify the
 * connection type; see sensor_detect.h's ev3_sensor_conn_type().
 *
 * MUST NOT be called on a port another daemon has bound: on the hardware-UART
 * ports (1/2) floating re-muxes pin 5/6 to GPIO and would tear down that
 * daemon's live UART / I2C link. On the PRU soft-UART ports (3/4) pin 5/6 are
 * McASP lines the PRU owns, so float leaves pin 5/6 and buf_en untouched there
 * and only drives pin 1 low / reads pin 2 - re-muxing the McASP pins to GPIO
 * would float the PRU's RX input and storm PRU_EVTOUT interrupts.
 */
void     ev3_input_port_float(int port);

/* True for the PRU0 soft-UART input ports (physical 3/4), whose pin 5/6 are
 * McASP serialiser lines rather than GPIO. ev3_sensor_conn_type() uses this to
 * avoid reading pin 5/6 (invalid there) and classify those ports from pin 2 +
 * pin 1 alone. */
int      ev3_input_port_is_pru(int port);

/*
 * Put the port in UART mode: enable the line buffer (active low) and mux
 * TX/RX to the hardware UART. Only input ports 1 and 2 are wired to a
 * hardware UART (UART1 / UART0); ports 3 and 4 use PRU soft-UARTs which
 * are not implemented here. Returns 0 on success, -1 otherwise.
 */
int      ev3_input_port_uart_enable(int port);
void     ev3_input_port_uart_disable(int port);

/* Physical UART controller base for a port, or 0 when unsupported. */
uint32_t ev3_input_port_uart_base(int port);

/* AINTC interrupt number of the port UART, or 0 when unsupported. */
uint32_t ev3_input_port_uart_irq(int port);

/*
 * Put pin5/pin6 in GPIO mode for bit-banged I2C (disables the UART
 * buffer so the lines are driven by the CPU). Returns 0 on success.
 */
int      ev3_input_port_i2c_enable(int port);

/*
 * /dev/adc0 channel of the analog sensor lines. EV3 analog sensors
 * (touch) report on pin 6, NXT analog sensors (touch/light/sound) on pin 1.
 */
int      ev3_input_port_adc_channel(int port);       /* pin 6 */
int      ev3_input_port_adc_channel_pin1(int port);  /* pin 1 */

/* ---- Output (motor) ports A..D ---- */
#define EV3_OUT_PORT_A  0
#define EV3_OUT_PORT_B  1
#define EV3_OUT_PORT_C  2
#define EV3_OUT_PORT_D  3
#define EV3_OUT_PORT_COUNT 4

/* H-bridge states driven by the two direction GPIOs (xIN0/xIN1). */
#define EV3_MOTOR_COAST  0
#define EV3_MOTOR_FWD    1
#define EV3_MOTOR_REV    2
#define EV3_MOTOR_BRAKE  3

/* Configure direction outputs and tacho inputs for a motor port. */
void     ev3_output_port_init(int port);

/*
 * Drive the H-bridge direction pins (speed is set separately via PWM).
 * Same truth table as ev3dev ev3_ports_out.c:
 *   FWD   : xIN0 = 1, xIN1 floating
 *   REV   : xIN0 floating, xIN1 = 1
 *   BRAKE : both 1
 *   COAST : both 0
 */
void     ev3_output_port_bridge(int port, int state);

/*
 * Raw tacho lines. Returns the INT level and stores the DIR level in *dir.
 * Quadrature decoding (ev3dev legoev3_motor.c): on every INT edge the
 * motor is turning forward when INT != DIR, backward when INT == DIR.
 */
int      ev3_output_port_tacho(int port, int* dir);

/* GPIO numbers of the tacho INT / DIR lines (for edge interrupts). */
void     ev3_output_port_tacho_pins(int port, int* int_pin, int* dir_pin);

/* /dev/adc0 channel of pin 5 (motor detection: 2400..2600 mV = empty). */
int      ev3_output_port_adc_channel(int port);

#endif
