#include <stdint.h>
#include <stdbool.h>
#include <ewoksys/mmio.h>

#include "../include/arch/ev3/sound.h"
#include "../include/arch/ev3/gpio.h"

#define BIT(x)  (0x1u<<(x))

#define writew(val, reg)  (*(volatile uint16_t*)(reg) = (val))
#define readw(reg)        (*(volatile uint16_t*)(reg))

/* AM1808 physical addresses (offset from mmio base = physical 0) */
#define EHRPWM0_BASE  0x01F00000
#define PSC1_BASE     0x01E27000

#define PSC_MOD_EHRPWM  17
#define CFGCHIP1_TBCLKSYNC  BIT(12)

/* eHRPWM register offsets (16-bit), same layout as eHRPWM1. */
#define TBCTL     0x00
#define TBPRD     0x0A
#define CMPB      0x14
#define AQCTLB    0x18
#define AQSFRC    0x1A
#define AQCSFRC   0x1C

#define TBCTL_CTRMODE_MASK   (BIT(1)|BIT(0))
#define TBCTL_CTRMODE_UP     0
#define TBCTL_CTRMODE_FREEZE (BIT(1)|BIT(0))
#define TBCTL_PRDLD          BIT(3)
/* CLKDIV[9:7] : divide ratio = 1 << CLKDIV ; HSPCLKDIV[12:10] forced to /1 */
#define TBCTL_CLKDIV_SHIFT   7
#define TBCTL_CLKDIV_MASK    (0x3fu << TBCTL_CLKDIV_SHIFT)

#define AQCTL_ZRO_HIGH       (2u<<0)
#define AQCTL_CBU_LOW        (1u<<8)

#define AQSFRC_RLDCSF_MASK   (BIT(7)|BIT(6))
#define AQSFRC_RLDCSF_ZRO    0
#define AQSFRC_RLDCSF_IMDT   (BIT(7)|BIT(6))

#define AQCSFRC_CSFB_MASK    (BIT(3)|BIT(2))
#define AQCSFRC_CSFB_FRCLOW  BIT(2)
#define AQCSFRC_CSFB_FRCDIS  0

/* Amp enable: GP6[15] = 111, active high (da850-lego-ev3.dts "sound"). */
#define PIN_AMP_ENABLE  111

static bool _inited = false;

static uint32_t mmio(uint32_t phys) {
    return (uint32_t)(_mmio_base + phys);
}

static void reg16_set(uint32_t phys, uint16_t val, uint16_t mask) {
    volatile uint16_t* reg = (volatile uint16_t*)mmio(phys);
    uint16_t v = *reg;
    v &= ~mask;
    v |= (val & mask);
    *reg = v;
}

static void psc_enable_ehrpwm(void) {
    volatile uint32_t* mdctl  = (volatile uint32_t*)mmio(PSC1_BASE + 0xA00 + 4*PSC_MOD_EHRPWM);
    volatile uint32_t* mdstat = (volatile uint32_t*)mmio(PSC1_BASE + 0x800 + 4*PSC_MOD_EHRPWM);
    volatile uint32_t* ptcmd  = (volatile uint32_t*)mmio(PSC1_BASE + 0x120);
    volatile uint32_t* ptstat = (volatile uint32_t*)mmio(PSC1_BASE + 0x128);
    int t;
    if ((*mdstat & 0x1f) == 0x3)
        return;
    t = 100000; while (*ptstat && --t > 0) ;
    *mdctl = (*mdctl & ~0x1f) | 0x3;   /* NEXT = ENABLE */
    *ptcmd = 0x1;
    t = 100000; while (*ptstat && --t > 0) ;
    t = 100000; while (((*mdstat & 0x1f) != 0x3) && --t > 0) ;
}

void ev3_sound_init(void) {
    if (_inited) return;

    psc_enable_ehrpwm();

    /* SYSCFG registers are privileged-only: go through the kernel. */
    ev3_syscfg_write(EV3_SYSCFG_CFGCHIP1, CFGCHIP1_TBCLKSYNC, CFGCHIP1_TBCLKSYNC);
    /* ehrpwm0b_pins in da850.dtsi: PINMUX3[7:4] = 2 */
    ev3_syscfg_write(EV3_SYSCFG_PINMUX(3), 2u << 4, 0xf0);

    /* Amp enable: default off, active high. */
    ev3_gpio_config(PIN_AMP_ENABLE, GPIO_OUTPUT);
    ev3_gpio_write(PIN_AMP_ENABLE, 0);

    /* Freeze counter while configuring, edge-aligned PWM on chB. */
    reg16_set(EHRPWM0_BASE + TBCTL, TBCTL_CTRMODE_FREEZE, TBCTL_CTRMODE_MASK);
    reg16_set(EHRPWM0_BASE + TBCTL, 0, TBCTL_CLKDIV_MASK);   /* /1, /1 */
    reg16_set(EHRPWM0_BASE + TBCTL, 0, TBCTL_PRDLD);
    writew(AQCTL_ZRO_HIGH | AQCTL_CBU_LOW, mmio(EHRPWM0_BASE + AQCTLB));

    /* Force output low until a tone is requested. */
    reg16_set(EHRPWM0_BASE + AQSFRC,  AQSFRC_RLDCSF_IMDT, AQSFRC_RLDCSF_MASK);
    reg16_set(EHRPWM0_BASE + AQCSFRC, AQCSFRC_CSFB_FRCLOW, AQCSFRC_CSFB_MASK);

    _inited = true;
}

void ev3_sound_enable_amp(int on) {
    if (!_inited) return;
    ev3_gpio_write(PIN_AMP_ENABLE, on ? 1 : 0);
}

static int _volume = 100;

void ev3_sound_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    _volume = percent;
}

int ev3_sound_get_volume(void) {
    return _volume;
}

void ev3_sound_tone(int freq_hz) {
    if (!_inited) return;

    if (freq_hz <= 0 || _volume == 0) {
        ev3_sound_stop();
        return;
    }
    if (freq_hz < EV3_SOUND_MIN_FREQ) freq_hz = EV3_SOUND_MIN_FREQ;
    if (freq_hz > EV3_SOUND_MAX_FREQ) freq_hz = EV3_SOUND_MAX_FREQ;

    /* Pick the smallest CLKDIV (1 << n, n <= 7) that fits the period in
     * the 16-bit TBPRD. */
    uint32_t div_shift = 0;
    uint32_t period;
    while (1) {
        period = EV3_SOUND_TBCLK_HZ / ((uint32_t)freq_hz << div_shift);
        if (period <= 0xFFFF || div_shift >= 7)
            break;
        div_shift++;
    }
    if (period > 0xFFFF) period = 0xFFFF;
    if (period < 2)      period = 2;

    reg16_set(EHRPWM0_BASE + TBCTL, TBCTL_CTRMODE_FREEZE, TBCTL_CTRMODE_MASK);
    reg16_set(EHRPWM0_BASE + TBCTL,
              (uint16_t)(div_shift << TBCTL_CLKDIV_SHIFT), TBCTL_CLKDIV_MASK);
    writew((uint16_t)(period - 1), mmio(EHRPWM0_BASE + TBPRD));
    /* Loudness follows the duty cycle (50 % is max, like pwm-beeper). */
    uint32_t duty = period * (uint32_t)_volume / 200;
    if (duty < 1) duty = 1;
    writew((uint16_t)duty, mmio(EHRPWM0_BASE + CMPB));

    /* Release the force-low, then start the counter. */
    reg16_set(EHRPWM0_BASE + AQCSFRC, AQCSFRC_CSFB_FRCDIS, AQCSFRC_CSFB_MASK);
    reg16_set(EHRPWM0_BASE + AQSFRC,  AQSFRC_RLDCSF_ZRO,   AQSFRC_RLDCSF_MASK);
    reg16_set(EHRPWM0_BASE + TBCTL,   TBCTL_CTRMODE_UP,    TBCTL_CTRMODE_MASK);

    ev3_sound_enable_amp(1);
}

void ev3_sound_stop(void) {
    if (!_inited) return;
    ev3_sound_enable_amp(0);
    reg16_set(EHRPWM0_BASE + TBCTL,   TBCTL_CTRMODE_FREEZE, TBCTL_CTRMODE_MASK);
    reg16_set(EHRPWM0_BASE + AQSFRC,  AQSFRC_RLDCSF_IMDT,   AQSFRC_RLDCSF_MASK);
    reg16_set(EHRPWM0_BASE + AQCSFRC, AQCSFRC_CSFB_FRCLOW,  AQCSFRC_CSFB_MASK);
}
