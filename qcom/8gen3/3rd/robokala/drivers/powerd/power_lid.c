/*
 * power_lid.c - keyboard/lid hall sensor access for the Robo & Kala
 * TW220 (Qualcomm SC8280XP).
 *
 * The hall sensor sits on TLMM GPIO 107, active-low (closed = low) -
 * the same pad the mainline Ntmer TW220 device tree wires to gpio-keys
 * as SW_LID.  TLMM is the only non-SPMI block this driver touches; the
 * window and the pad-config pattern are the ones goodixd proved safe on
 * this board (GPIO mode-0 SPI runs on the same TLMM every boot).
 *
 * The pad is configured as a plain GPIO input with a weak pull-up: the
 * DTS uses bias-disable because the sensor drives the line, and the
 * pull-up only matters when the sensor is unpowered or the pad floats -
 * it then reads HIGH = open, so a dead sensor can never force the
 * display asleep.  No interrupt, debounce or wakeup configuration is
 * written; the daemon polls the level like it polls the power key.
 */

#include <stdbool.h>
#include <stdint.h>

#include <ewoksys/klog.h>
#include <ewoksys/syscall.h>

#include "power_lid.h"

/* SC8280XP TLMM (physical address proven by goodixd).  powerd's
 * own VA slot, past its SPMI windows at 0x5a080000..0x5a110000. */
#define TLMM_PHY           0x0f100000ULL
#define TLMM_VA            0x5a200000ULL
#define TLMM_SIZE          0x00300000U
#define GPIO_STRIDE        0x1000U
#define GPIO_CTL(g)        ((uint32_t)(g) * GPIO_STRIDE)
#define GPIO_IO(g)         (GPIO_CTL(g) + 4U)
#define GPIO_IN            (1U << 0)
#define GPIO_OE            (1U << 9)
#define GPIO_PULL_MASK     0x3U
#define GPIO_PULL_UP       0x3U
#define GPIO_FUNC_MASK     (0xfU << 2)
#define GPIO_DRV_MASK      (0x7U << 6)

static volatile uint8_t* _tlmm;

static inline uint32_t rd(volatile uint8_t* base, uint32_t off) {
    return *(volatile uint32_t*)(base + off);
}

static inline void wr(volatile uint8_t* base, uint32_t off, uint32_t value) {
    *(volatile uint32_t*)(base + off) = value;
}

int power_lid_init(void) {
    uint32_t ctl;

    if (_tlmm != NULL)
        return 0;
    if (syscall3(SYS_MEM_MAP, (ewokos_addr_t)TLMM_VA,
            (ewokos_addr_t)TLMM_PHY, (ewokos_addr_t)TLMM_SIZE) != TLMM_VA) {
        slog("qcom8gen3 powerd: lid TLMM mapping failed\n");
        return -1;
    }
    _tlmm = (volatile uint8_t*)(uintptr_t)TLMM_VA;
    /* GPIO function 0, input, weak pull-up (fail-open, see header). */
    ctl = rd(_tlmm, GPIO_CTL(POWER_LID_GPIO));
    ctl &= ~(GPIO_FUNC_MASK | GPIO_DRV_MASK | GPIO_PULL_MASK | GPIO_OE);
    ctl |= GPIO_PULL_UP;
    wr(_tlmm, GPIO_CTL(POWER_LID_GPIO), ctl);
    __asm__ volatile("dsb sy" ::: "memory");
    (void)rd(_tlmm, GPIO_CTL(POWER_LID_GPIO));
    return 0;
}

int power_lid_state(void) {
    if (_tlmm == NULL)
        return -1;
    /* Active-low hall output: low = keyboard closed over the panel. */
    return (rd(_tlmm, GPIO_IO(POWER_LID_GPIO)) & GPIO_IN) ? 0 : 1;
}