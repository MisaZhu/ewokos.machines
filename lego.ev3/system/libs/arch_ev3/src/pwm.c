#include <stdint.h>
#include <stdbool.h>
#include <ewoksys/mmio.h>

#include "../include/arch/ev3/pwm.h"
#include "../include/arch/ev3/gpio.h"

#define BIT(x)  (0x1u<<(x))

#define writel(val, reg)  (*(volatile uint32_t*)(reg) = (val))
#define readl(reg)        (*(volatile uint32_t*)(reg))
#define writew(val, reg)  (*(volatile uint16_t*)(reg) = (val))
#define readw(reg)        (*(volatile uint16_t*)(reg))

/* AM1808 / DA850 physical addresses (offset from mmio base = physical 0) */
#define EHRPWM1_BASE  0x01F02000
#define ECAP0_BASE    0x01F06000
#define ECAP1_BASE    0x01F07000
#define PSC1_BASE     0x01E27000

/* PSC1 modules (from da850.dtsi: ehrpwm=<&psc1 17>, ecap=<&psc1 20>) */
#define PSC_MOD_EHRPWM  17
#define PSC_MOD_ECAP    20

/* CFGCHIP1 eHRPWM time-base clock sync gate */
#define CFGCHIP1_TBCLKSYNC  BIT(12)

/* ---- eHRPWM register offsets / bits (16-bit) ---- */
#define TBCTL     0x00
#define TBPRD     0x0A
#define CMPA      0x12
#define CMPB      0x14
#define AQCTLA    0x16
#define AQCTLB    0x18
#define AQSFRC    0x1A
#define AQCSFRC   0x1C

#define TBCTL_CTRMODE_MASK    (BIT(1)|BIT(0))
#define TBCTL_CTRMODE_UP      0
#define TBCTL_CTRMODE_FREEZE  (BIT(1)|BIT(0))
#define TBCTL_PRDLD           BIT(3)              /* 0 = shadow load */
/* CLKDIV[9:7] and HSPCLKDIV[12:10]; reset value of HSPCLKDIV is /2 */
#define TBCTL_CLKDIV_MASK     (0x3fu<<7)

/* Action qualifier: 2-bit fields, 1 = clear(low), 2 = set(high) */
#define AQCTL_ZRO_LOW         (1u<<0)
#define AQCTL_ZRO_HIGH        (2u<<0)
#define AQCTL_CAU_LOW         (1u<<4)
#define AQCTL_CAU_HIGH        (2u<<4)
#define AQCTL_CBU_LOW         (1u<<8)
#define AQCTL_CBU_HIGH        (2u<<8)

#define AQSFRC_RLDCSF_MASK    (BIT(7)|BIT(6))
#define AQSFRC_RLDCSF_IMDT    (BIT(7)|BIT(6))

#define AQCSFRC_CSFA_MASK     (BIT(1)|BIT(0))
#define AQCSFRC_CSFB_MASK     (BIT(3)|BIT(2))
#define AQCSFRC_CSFA_DIS      0
#define AQCSFRC_CSFB_DIS      0

/* ---- eCAP register offsets / bits ---- */
#define ECAP_CAP1   0x08   /* period (active)   */
#define ECAP_CAP2   0x0C   /* duty   (active)   */
#define ECAP_CAP3   0x10   /* period (shadow)   */
#define ECAP_CAP4   0x14   /* duty   (shadow)   */
#define ECAP_ECCTL2 0x2A
#define ECCTL2_APWM_POL_LOW   BIT(10)
#define ECCTL2_APWM_MODE      BIT(9)
#define ECCTL2_SYNC_SEL_DISA  (BIT(7)|BIT(6))
#define ECCTL2_TSCTR_FREERUN  BIT(4)

/* period cycles per channel (index = EV3_MOTOR_x) */
static uint32_t _period[EV3_MOTOR_COUNT];
static uint32_t _duty[EV3_MOTOR_COUNT];
static bool _enabled[EV3_MOTOR_COUNT];
static bool _inited = false;

/* motor -> (is_ecap, base, ehrpwm channel) */
struct pwm_map {
    int ecap;              /* 1 = eCAP, 0 = eHRPWM */
    uint32_t base;
    int chan;              /* eHRPWM channel: 0=A, 1=B */
};

static const struct pwm_map _map[EV3_MOTOR_COUNT] = {
    { 0, EHRPWM1_BASE, 1 },   /* Motor A -> eHRPWM1 channel B */
    { 0, EHRPWM1_BASE, 0 },   /* Motor B -> eHRPWM1 channel A */
    { 1, ECAP0_BASE,   0 },   /* Motor C -> eCAP0 */
    { 1, ECAP1_BASE,   0 },   /* Motor D -> eCAP1 */
};

static uint32_t mmio(uint32_t phys) {
    return (uint32_t)(_mmio_base + phys);
}

/* eHRPWM registers are 16-bit wide; always access them as such. */
static void reg16_set(uint32_t phys, uint16_t val, uint16_t mask) {
    volatile uint16_t* reg = (volatile uint16_t*)mmio(phys);
    uint16_t v = *reg;
    v &= ~mask;
    v |= (val & mask);
    *reg = v;
}

/* Enable a DaVinci PSC module (power/sleep controller). */
static void psc_enable(int module) {
    volatile uint32_t* mdctl  = (volatile uint32_t*)mmio(PSC1_BASE + 0xA00 + 4*module);
    volatile uint32_t* mdstat = (volatile uint32_t*)mmio(PSC1_BASE + 0x800 + 4*module);
    volatile uint32_t* ptcmd  = (volatile uint32_t*)mmio(PSC1_BASE + 0x120);
    volatile uint32_t* ptstat = (volatile uint32_t*)mmio(PSC1_BASE + 0x128);
    int timeout;

    if ((*mdstat & 0x1f) == 0x3)
        return;                          /* already enabled */

    timeout = 100000; while (*ptstat && --timeout > 0) ;
    *mdctl = (*mdctl & ~0x1f) | 0x3;   /* NEXT = ENABLE */
    *ptcmd = 0x1;                       /* GO */
    timeout = 100000; while (*ptstat && --timeout > 0) ;
    timeout = 100000; while (((*mdstat & 0x1f) != 0x3) && --timeout > 0) ;
}

static void ehrpwm1_hw_init(void) {
    /* Freeze counter while configuring the shared time base. */
    reg16_set(EHRPWM1_BASE + TBCTL, TBCTL_CTRMODE_FREEZE, TBCTL_CTRMODE_MASK);
    reg16_set(EHRPWM1_BASE + TBCTL, 0, TBCTL_CLKDIV_MASK);   /* TBCLK = SYSCLK2 */
    reg16_set(EHRPWM1_BASE + TBCTL, 0, TBCTL_PRDLD);         /* shadow period */

    /*
     * Inverted polarity (PWM_POLARITY_INVERTED in the DTS): low at zero,
     * high when the compare matches. CMPx = 0 makes CAU/CBU win over ZRO
     * so the line stays high (0 % duty); CMPx > TBPRD never matches so the
     * line stays low (100 % duty) - same as pwm-tiehrpwm.c.
     */
    writew(AQCTL_ZRO_LOW | AQCTL_CAU_HIGH, mmio(EHRPWM1_BASE + AQCTLA));
    writew(AQCTL_ZRO_LOW | AQCTL_CBU_HIGH, mmio(EHRPWM1_BASE + AQCTLB));

    /* No software force in effect. */
    reg16_set(EHRPWM1_BASE + AQSFRC,  AQSFRC_RLDCSF_IMDT, AQSFRC_RLDCSF_MASK);
    reg16_set(EHRPWM1_BASE + AQCSFRC, AQCSFRC_CSFA_DIS | AQCSFRC_CSFB_DIS,
              AQCSFRC_CSFA_MASK | AQCSFRC_CSFB_MASK);
}

static void ecap_hw_init(uint32_t base) {
    uint16_t ctl = readw(mmio(base + ECAP_ECCTL2));
    ctl |= ECCTL2_APWM_MODE | ECCTL2_SYNC_SEL_DISA | ECCTL2_APWM_POL_LOW;
    ctl &= ~ECCTL2_TSCTR_FREERUN;
    writew(ctl, mmio(base + ECAP_ECCTL2));
}

static void apply_duty(int motor) {
    uint32_t duty = _enabled[motor] ? _duty[motor] : 0;

    if (_map[motor].ecap) {
        writel(duty, mmio(_map[motor].base + ECAP_CAP2));
        writel(duty, mmio(_map[motor].base + ECAP_CAP4));
    } else {
        int off = (_map[motor].chan == 0) ? CMPA : CMPB;
        writew((uint16_t)duty, mmio(EHRPWM1_BASE + off));
    }
}

void ev3_pwm_init(void) {
    if (_inited)
        return;

    /* Power on the eHRPWM / eCAP modules. */
    psc_enable(PSC_MOD_EHRPWM);
    psc_enable(PSC_MOD_ECAP);

    /* Enable the eHRPWM time-base clock (CFGCHIP1.TBCLKSYNC). SYSCFG is
     * privileged-only, so this has to go through the kernel. */
    ev3_syscfg_write(EV3_SYSCFG_CFGCHIP1, CFGCHIP1_TBCLKSYNC, CFGCHIP1_TBCLKSYNC);

    /* Route the four motor PWM signals to their pins (da850.dtsi:
     *   ehrpwm1a = PINMUX5[3:0]=2, ehrpwm1b = PINMUX5[7:4]=2,
     *   ecap0    = PINMUX2[31:28]=2, ecap1 = PINMUX1[31:28]=4). */
    ev3_syscfg_write(EV3_SYSCFG_PINMUX(5), 2,       0x0000000f);  /* EPWM1A -> Motor B */
    ev3_syscfg_write(EV3_SYSCFG_PINMUX(5), 2u<<4,   0x000000f0);  /* EPWM1B -> Motor A */
    ev3_syscfg_write(EV3_SYSCFG_PINMUX(2), 2u<<28,  0xf0000000);  /* ECAP0  -> Motor C */
    ev3_syscfg_write(EV3_SYSCFG_PINMUX(1), 4u<<28,  0xf0000000);  /* ECAP1  -> Motor D */

    ehrpwm1_hw_init();
    ecap_hw_init(ECAP0_BASE);
    ecap_hw_init(ECAP1_BASE);

    for (int i = 0; i < EV3_MOTOR_COUNT; i++) {
        _duty[i] = 0;
        _enabled[i] = false;
        ev3_pwm_set_period(i, EV3_PWM_DEFAULT_PERIOD);
        apply_duty(i);
    }

    /* Start the time bases; outputs sit at the inactive (high) level. */
    reg16_set(EHRPWM1_BASE + TBCTL, TBCTL_CTRMODE_UP, TBCTL_CTRMODE_MASK);
    for (int i = 0; i < EV3_MOTOR_COUNT; i++) {
        if (!_map[i].ecap) continue;
        uint32_t base = _map[i].base;
        writew(readw(mmio(base + ECAP_ECCTL2)) | ECCTL2_TSCTR_FREERUN,
               mmio(base + ECAP_ECCTL2));
    }

    _inited = true;
}

void ev3_pwm_set_period(int motor, uint32_t period_cycles) {
    if (motor < 0 || motor >= EV3_MOTOR_COUNT)
        return;
    if (period_cycles < 2)
        period_cycles = 2;
    if (period_cycles > 0xFFFF)
        period_cycles = 0xFFFF;

    _period[motor] = period_cycles;

    if (_map[motor].ecap) {
        writel(period_cycles - 1, mmio(_map[motor].base + ECAP_CAP1));
        writel(period_cycles - 1, mmio(_map[motor].base + ECAP_CAP3));
    } else {
        /* eHRPWM1 shares one TBPRD between channels A and B. */
        writew((uint16_t)(period_cycles - 1), mmio(EHRPWM1_BASE + TBPRD));
    }
    if (_duty[motor] > period_cycles)
        _duty[motor] = period_cycles;
    apply_duty(motor);
}

void ev3_pwm_set_duty(int motor, uint32_t duty_cycles) {
    if (motor < 0 || motor >= EV3_MOTOR_COUNT)
        return;
    if (duty_cycles > _period[motor])
        duty_cycles = _period[motor];
    _duty[motor] = duty_cycles;
    apply_duty(motor);
}

void ev3_pwm_set_percent(int motor, int percent) {
    if (motor < 0 || motor >= EV3_MOTOR_COUNT)
        return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    ev3_pwm_set_duty(motor, (uint32_t)((uint64_t)_period[motor] * percent / 100));
}

void ev3_pwm_enable(int motor, int en) {
    if (motor < 0 || motor >= EV3_MOTOR_COUNT)
        return;
    _enabled[motor] = en ? true : false;
    apply_duty(motor);
}
