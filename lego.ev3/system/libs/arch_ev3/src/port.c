#include <stdint.h>
#include <stdbool.h>
#include <ewoksys/mmio.h>

#include "../include/arch/ev3/port.h"
#include "../include/arch/ev3/gpio.h"

/* Hardware UART controllers (AM1808) and their AINTC lines. */
#define UART0_BASE  0x01C42000
#define UART1_BASE  0x01D0C000
#define UART0_IRQ   25
#define UART1_IRQ   53

/* ---------------- input (sensor) ports ---------------- */

struct in_port {
    int pin1;     /* sensor supply enable (I_ONx)                   */
    int pin2;     /* digital detect (LEGDETx)                       */
    int pin5;     /* UART TX side / I2C SCL when used as GPIO       */
    int pin6;     /* UART RX side / I2C SDA when used as GPIO       */
    int buf_en;   /* RXINx_ENTXINx_EN : line buffer enable (active low) */
    int adc_pin1;
    int adc_pin6;
    uint32_t uart_base;
    uint32_t uart_irq;
    int pmux_reg; /* PINMUX register index for the UART pins        */
    uint32_t pmux_val;
    uint32_t pmux_mask;
};

/* Numbers from da850-lego-ev3.dts (ev3-ports/in1..in4) and da850.dtsi
 * (serial0_rxtx_pins = PINMUX3[23:16] = 0x22, serial1_rxtx_pins =
 * PINMUX4[31:24] = 0x22). */
static const struct in_port _in[EV3_IN_PORT_COUNT] = {
    /* port 1 */ { 138,  34,  2, 15, 139,  6,  5, UART1_BASE, UART1_IRQ, 4, 0x22000000, 0xff000000 },
    /* port 2 */ { 140, 143, 14, 13, 142,  8,  7, UART0_BASE, UART0_IRQ, 3, 0x00220000, 0x00ff0000 },
    /* port 3 */ { 137, 123, 12, 30, 121, 10,  9, 0,          0,         0, 0,          0          },
    /* port 4 */ { 100, 120,  1, 31, 122, 12, 11, 0,          0,         0, 0,          0          },
};

static inline bool in_ok(int port) {
    return port >= 0 && port < EV3_IN_PORT_COUNT;
}

int ev3_input_port_gpio(int port, int pin) {
    if (!in_ok(port))
        return -1;
    switch (pin) {
    case EV3_PIN1: return _in[port].pin1;
    case EV3_PIN2: return _in[port].pin2;
    case EV3_PIN5: return _in[port].pin5;
    case EV3_PIN6: return _in[port].pin6;
    default:       return -1;
    }
}

void ev3_input_port_power(int port, int on) {
    if (!in_ok(port))
        return;
    ev3_gpio_config(_in[port].pin1, GPIO_OUTPUT);
    ev3_gpio_write(_in[port].pin1, on ? 1 : 0);
}

int ev3_input_port_detect(int port) {
    if (!in_ok(port))
        return 0;
    ev3_gpio_config(_in[port].pin2, GPIO_INPUT);
    return ev3_gpio_read(_in[port].pin2);
}

uint32_t ev3_input_port_uart_base(int port) {
    if (!in_ok(port))
        return 0;
    return _in[port].uart_base;
}

uint32_t ev3_input_port_uart_irq(int port) {
    if (!in_ok(port))
        return 0;
    return _in[port].uart_irq;
}

int ev3_input_port_adc_channel(int port) {
    if (!in_ok(port))
        return -1;
    return _in[port].adc_pin6;
}

int ev3_input_port_adc_channel_pin1(int port) {
    if (!in_ok(port))
        return -1;
    return _in[port].adc_pin1;
}

int ev3_input_port_uart_enable(int port) {
    if (!in_ok(port))
        return -1;
    if (_in[port].uart_base == 0)
        return -1;   /* PRU soft-UART ports are not supported */

    /* Route the port TX/RX pins to the hardware UART (SYSCFG is
     * privileged-only, so this must go through the kernel). */
    ev3_syscfg_write(EV3_SYSCFG_PINMUX(_in[port].pmux_reg),
                     _in[port].pmux_val, _in[port].pmux_mask);

    /* Enable the UART line buffer (active low). */
    ev3_gpio_config(_in[port].buf_en, GPIO_OUTPUT);
    ev3_gpio_write(_in[port].buf_en, 0);
    return 0;
}

void ev3_input_port_uart_disable(int port) {
    if (!in_ok(port))
        return;
    ev3_gpio_config(_in[port].buf_en, GPIO_OUTPUT);
    ev3_gpio_write(_in[port].buf_en, 1);   /* disabled = high */
}

int ev3_input_port_i2c_enable(int port) {
    if (!in_ok(port))
        return -1;

    /* Keep the UART buffer off so pin5/pin6 are plain GPIO lines. */
    ev3_gpio_config(_in[port].buf_en, GPIO_OUTPUT);
    ev3_gpio_write(_in[port].buf_en, 1);

    /* Open-drain emulation: idle as inputs (pulled up by the sensor). */
    ev3_gpio_config(_in[port].pin5, GPIO_INPUT);
    ev3_gpio_config(_in[port].pin6, GPIO_INPUT);
    return 0;
}

/* ---------------- output (motor) ports ---------------- */

struct out_port {
    int in0;      /* pin1 : H-bridge direction 0 */
    int in1;      /* pin2 : H-bridge direction 1 */
    int tacho;    /* pin5 : encoder pulse (INT)  */
    int dir;      /* pin6 : encoder direction    */
    int det;      /* pin5-det : detection shorting transistor */
    int adc_pin5;
};

static const struct out_port _out[EV3_OUT_PORT_COUNT] = {
    /* A */ {  63,  54,  91,  4, 84,  1 },
    /* B */ {  33,   3,  88, 41, 37,  0 },
    /* C */ { 104,  89,  93, 62, 56, 13 },
    /* D */ {  83,  90, 105, 40, 95, 14 },
};

static inline bool out_ok(int port) {
    return port >= 0 && port < EV3_OUT_PORT_COUNT;
}

void ev3_output_port_init(int port) {
    if (!out_ok(port))
        return;
    ev3_gpio_config(_out[port].in0, GPIO_OUTPUT);
    ev3_gpio_config(_out[port].in1, GPIO_OUTPUT);
    ev3_gpio_write(_out[port].in0, 0);
    ev3_gpio_write(_out[port].in1, 0);
    ev3_gpio_config(_out[port].det, GPIO_OUTPUT);
    ev3_gpio_write(_out[port].det, 0);
    ev3_gpio_config(_out[port].tacho, GPIO_INPUT);
    ev3_gpio_config(_out[port].dir, GPIO_INPUT);
}

void ev3_output_port_bridge(int port, int state) {
    if (!out_ok(port))
        return;
    const struct out_port* p = &_out[port];

    switch (state) {
    case EV3_MOTOR_FWD:
        ev3_gpio_write(p->in0, 1);
        ev3_gpio_dir(p->in0, 1);
        ev3_gpio_dir(p->in1, 0);          /* floating */
        break;
    case EV3_MOTOR_REV:
        ev3_gpio_write(p->in1, 1);
        ev3_gpio_dir(p->in1, 1);
        ev3_gpio_dir(p->in0, 0);          /* floating */
        break;
    case EV3_MOTOR_BRAKE:
        ev3_gpio_write(p->in0, 1);
        ev3_gpio_write(p->in1, 1);
        ev3_gpio_dir(p->in0, 1);
        ev3_gpio_dir(p->in1, 1);
        break;
    case EV3_MOTOR_COAST:
    default:
        ev3_gpio_write(p->in0, 0);
        ev3_gpio_write(p->in1, 0);
        ev3_gpio_dir(p->in0, 1);
        ev3_gpio_dir(p->in1, 1);
        break;
    }
}

int ev3_output_port_tacho(int port, int* dir) {
    if (!out_ok(port))
        return 0;
    if (dir)
        *dir = ev3_gpio_read(_out[port].dir);
    return ev3_gpio_read(_out[port].tacho);
}

void ev3_output_port_tacho_pins(int port, int* int_pin, int* dir_pin) {
    if (!out_ok(port)) {
        if (int_pin) *int_pin = -1;
        if (dir_pin) *dir_pin = -1;
        return;
    }
    if (int_pin) *int_pin = _out[port].tacho;
    if (dir_pin) *dir_pin = _out[port].dir;
}

int ev3_output_port_adc_channel(int port) {
    if (!out_ok(port))
        return -1;
    return _out[port].adc_pin5;
}
