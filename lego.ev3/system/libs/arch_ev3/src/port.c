#include <stdint.h>
#include <stdbool.h>
#include <ewoksys/mmio.h>

#include "../include/arch/ev3/port.h"
#include "../include/arch/ev3/gpio.h"
#include "../include/arch/ev3/uart.h"      /* EV3_UART_PRU_BASE / EV3_UART_IS_PRU */
#include "../include/arch/ev3/pru_uart.h"  /* ev3_pru_uart_port_enable/disable    */

/* Hardware UART controllers (AM1808) and their AINTC lines. */
#define UART0_BASE  0x01C42000
#define UART1_BASE  0x01D0C000
#define UART0_IRQ   25
#define UART1_IRQ   53

/* Ports 3/4 have no 16550; they are driven by the PRU0 SUART firmware. Each
 * channel's RX host event reaches the ARM as a distinct PRU_EVTOUT->AINTC sysint
 * (EVTOUT2=5 for port 4 / SUART1, EVTOUT3=6 for port 3 / SUART2 - see
 * pru_uart.h), so the two ports never share an IRQ line. The sentinel base makes
 * uart_sensor.c's `base==0` gates pass while uart.c routes ev3_uart_* to the PRU
 * transport instead of a 16550. */
#define PRU_EVTOUT2_IRQ 5
#define PRU_EVTOUT3_IRQ 6

/* CFGCHIP3.ASYNC3_CLKSRC (bit 4): 0 = async3 fed by pll0_sysclk2 (parent0),
 * 1 = pll1_sysclk2 (parent1). UART1 (input port 1) takes its clock from
 * async3, while UART0 (input port 2) takes pll0_sysclk2 directly. */
#define CFGCHIP3_ASYNC3_CLKSRC  (1u << 4)

/* DA850 PSC (Power/Sleep Controller) LPSC registers. A peripheral receives no
 * functional clock until its module is switched to ENABLE, so each sensor UART
 * must be ungated here before its 16550 registers are touched. da850.dtsi
 * gates them through power-domains = <&psc0 9> (serial0/UART0) and <&psc1 12>
 * (serial1/UART1) and Linux's PSC driver turns those on at probe; nothing in
 * EwokOS does (the kernel has no PSC code and uart_dev_init() is a no-op), so
 * without this the UART stays clock-gated and every divisor/FIFO/PWREMU write
 * is inert on BOTH ports. sd.c (EDMA), sound.c/pwm.c (eHRPWM) and i2c.c (I2C1)
 * each enable their own module the same way, which is exactly why those
 * peripherals work and the sensor UARTs did not. */
#define PSC0_BASE       0x01C10000u
#define PSC1_BASE       0x01E27000u
#define PSC_MDSTAT(n)   (0x800 + (n) * 4)
#define PSC_MDCTL(n)    (0xA00 + (n) * 4)
#define PSC_PTCMD       0x120
#define PSC_PTSTAT      0x128
#define PSC_MD_ENABLE   0x3     /* MDSTAT/MDCTL.STATE field = ENABLE */

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
    uint32_t psc_base; /* PSC controller gating the UART clock (0 = none) */
    int psc_mod;       /* LPSC module number within that controller       */
};

/* Numbers from da850-lego-ev3.dts (ev3-ports/in1..in4) and da850.dtsi
 * (serial0_rxtx_pins = PINMUX3[23:16] = 0x22, serial1_rxtx_pins =
 * PINMUX4[31:24] = 0x22). */
static const struct in_port _in[EV3_IN_PORT_COUNT] = {
    /* port 1 */ { 138,  34,  2, 15, 139,  6,  5, UART1_BASE, UART1_IRQ, 4, 0x22000000, 0xff000000, PSC1_BASE, 12 },
    /* port 2 */ { 140, 143, 14, 13, 142,  8,  7, UART0_BASE, UART0_IRQ, 3, 0x00220000, 0x00ff0000, PSC0_BASE,  9 },
    /* port 3 */ { 137, 123, 12, 30, 121, 10,  9, EV3_UART_PRU_BASE(2), PRU_EVTOUT3_IRQ, 0, 0, 0, 0, 0 },
    /* port 4 */ { 100, 120,  1, 31, 122, 12, 11, EV3_UART_PRU_BASE(3), PRU_EVTOUT2_IRQ, 0, 0, 0, 0, 0 },
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

/* ev3dev's ev3_input_port_float(): the detection state the connection-type
 * classifier samples. pin 1 LOW (EV3 sensing, never the 9 V NXT supply),
 * pin 2/5/6 digital inputs, line buffer DISABLED so pin 5/6 are plain GPIO.
 * The buffer is active-low in hardware (DTS GPIO_ACTIVE_LOW): disabled = raw 1,
 * exactly what ev3_input_port_i2c_enable()/uart_disable() already use. */
void ev3_input_port_float(int port) {
    if (!in_ok(port))
        return;
    ev3_gpio_config(_in[port].pin1, GPIO_OUTPUT);
    ev3_gpio_write(_in[port].pin1, 0);            /* pin 1 LOW: EV3 sense level */
    ev3_gpio_config(_in[port].pin2, GPIO_INPUT);  /* NXT-vs-EV3 discriminator   */

    /* Ports 3/4 are the PRU0 soft-UART: pin 5/6 are McASP serialiser lines that
     * pru_pinmux() claims (PINMUX0 AHCLK + PINMUX2 AXR1..4), and buf_en must
     * stay enabled for PRU RX. ev3_gpio_config(pin,GPIO_INPUT) re-muxes the pin
     * via gpio_mux_cfg(MODE_GPIO), so floating pin 5/6 here would steal the McASP
     * pins back to GPIO: the RX input floats while the PRU keeps clocking it,
     * which turns into a garbage-RX PRU_EVTOUT (IRQ 5/6) interrupt storm that
     * freezes the single-core system, and it tears down any live soft-UART link.
     * pin 1 (I_ON) and pin 2 (LEGDET) are dedicated GPIOs, never McASP, and pin 2
     * low already tells an NXT sensor from an EV3 one - so on PRU ports float
     * only pin 1/pin 2 and leave pin 5/6/buf_en to pru_uart.c. */
    if (EV3_UART_IS_PRU(_in[port].uart_base))
        return;

    ev3_gpio_config(_in[port].pin5, GPIO_INPUT);  /* pull-up: low => fault      */
    ev3_gpio_config(_in[port].pin6, GPIO_INPUT);  /* pull-down: high => I2C     */
    ev3_gpio_config(_in[port].buf_en, GPIO_OUTPUT);
    ev3_gpio_write(_in[port].buf_en, 1);          /* buffer disabled = raw high */
}

/* True for the PRU0 soft-UART ports (physical 3/4). Their pin 5/6 belong to the
 * McASP serialisers, so callers must not treat them as GPIO (see float above). */
int ev3_input_port_is_pru(int port) {
    if (!in_ok(port))
        return 0;
    return EV3_UART_IS_PRU(_in[port].uart_base) ? 1 : 0;
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

/* Switch a DA850 PSC LPSC module to ENABLE so its peripheral is clocked.
 * No-op when already enabled; identical to the sequence sd.c (PSC0) and
 * sound.c/pwm.c/i2c.c (PSC1) use for theirs. */
static void psc_module_enable(uint32_t psc_base, int module) {
    if (psc_base == 0)
        return;
    volatile uint32_t* mdctl  = (volatile uint32_t*)(_mmio_base + psc_base + PSC_MDCTL(module));
    volatile uint32_t* mdstat = (volatile uint32_t*)(_mmio_base + psc_base + PSC_MDSTAT(module));
    volatile uint32_t* ptcmd  = (volatile uint32_t*)(_mmio_base + psc_base + PSC_PTCMD);
    volatile uint32_t* ptstat = (volatile uint32_t*)(_mmio_base + psc_base + PSC_PTSTAT);
    int t;

    if ((*mdstat & 0x1f) == PSC_MD_ENABLE)
        return;                                   /* already enabled */
    t = 100000; while (*ptstat && --t > 0) ;      /* wait for any in-flight transition */
    *mdctl = (*mdctl & ~0x1fu) | PSC_MD_ENABLE;   /* NEXT = ENABLE */
    *ptcmd = 0x1;                                 /* GO */
    t = 100000; while (*ptstat && --t > 0) ;
    t = 100000; while (((*mdstat & 0x1f) != PSC_MD_ENABLE) && --t > 0) ;
}

int ev3_input_port_uart_enable(int port) {
    if (!in_ok(port))
        return -1;

    /* Ports 3/4 are PRU soft-UART: the McASP serialisers (not the 16550 pins)
     * drive the port, so skip the CFGCHIP3 reparent / PSC / 16550-PINMUX below
     * and let pru_uart.c own the PRUSS+McASP bring-up and its own PINMUX. The
     * RX line buffer is still enabled (active low) exactly as for a HW UART. */
    if (EV3_UART_IS_PRU(_in[port].uart_base)) {
        ev3_gpio_config(_in[port].buf_en, GPIO_OUTPUT);
        ev3_gpio_write(_in[port].buf_en, 0);
        return ev3_pru_uart_port_enable(port);
    }

    if (_in[port].uart_base == 0)
        return -1;

    /* Input port 1 is UART1, whose functional clock is async3: a CFGCHIP3 mux
     * between pll0_sysclk2 and pll1_sysclk2. Linux reparents async3 to pll1
     * for CPU-frequency independence, but nothing here does that and pll1 may
     * not even be running - which would leave UART1 with no (or a different)
     * clock, so the baud divisor computed for pll0_sysclk2 would be wrong and
     * every byte garbage. Force async3 onto pll0_sysclk2: the same 150 MHz
     * clock that already feeds UART0 (input port 2) and the SD controller the
     * board boots from, so both sensor UARTs share one known baud reference.
     * Masked read-modify-write: only bit 4 changes. */
    ev3_syscfg_write(EV3_SYSCFG_CFGCHIP3, 0, CFGCHIP3_ASYNC3_CLKSRC);

    /* Ungate the UART's LPSC module so it actually receives its clock. Until
     * this the 16550 registers are inert and the divisor/FIFO/PWREMU writes in
     * ev3_uart_init() have no effect - the port looks permanently empty. */
    psc_module_enable(_in[port].psc_base, _in[port].psc_mod);

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
    if (EV3_UART_IS_PRU(_in[port].uart_base))
        ev3_pru_uart_port_disable(port);   /* stop RX, deactivate serialiser */
    ev3_gpio_config(_in[port].buf_en, GPIO_OUTPUT);
    ev3_gpio_write(_in[port].buf_en, 1);   /* disabled = high */
}

int ev3_input_port_i2c_enable(int port) {
    if (!in_ok(port))
        return -1;

    /* On the PRU soft-UART ports (3/4) pin 5/6 are McASP serialiser lines that
     * PRU0 actively clocks. The ev3_gpio_config(..., GPIO_INPUT) calls below
     * re-mux them to GPIO (gpio_mux_cfg MODE_GPIO); doing that while the soft-UART
     * still runs floats the PRU's RX input and storms PRU_EVTOUT (IRQ 5/6) - the
     * same single-core freeze ev3_input_port_float() guards against. Stop this
     * port's soft-UART channel first, so its RX serialiser is inactive and its
     * host event masked before the pins become GPIO. This is the I2C-side mirror
     * of ev3_input_port_uart_enable()'s PRU bring-up; the next uart_enable on this
     * port re-opens the channel. */
    if (EV3_UART_IS_PRU(_in[port].uart_base))
        ev3_pru_uart_port_disable(port);

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
