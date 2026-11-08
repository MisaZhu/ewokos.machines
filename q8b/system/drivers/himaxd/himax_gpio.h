#ifndef HIMAX_GPIO_H
#define HIMAX_GPIO_H

#include <stdbool.h>

#include "himax_protocol.h"

/*
 * GPIO bit-bang transport for the gaokun3 HX83121A.
 *
 * Only TLMM is mapped.  The QUP0 SE6 GENI block needs GPI DMA and SMMU
 * routing that is not validated on this platform, and unsafe MMIO there has
 * hung the machine before; a bit-banged master is slower but self-contained.
 *
 * himax_gpio_open() maps TLMM, drives the host-interface select pin low and
 * leaves CS/CLK high with the IC in reset.  It must run before any probe.
 */
bool himax_gpio_open(void);
himax_dev_t* himax_gpio_dev(void);

/* True when the level-low interrupt line is asserted. */
bool himax_gpio_irq_asserted(void);

/* TLMM pad inspection for bring-up diagnostics (read-only). */
typedef struct {
    uint32_t ctl;
    uint32_t io;
} himax_pad_t;

unsigned himax_gpio_pin_count(void);
const char* himax_gpio_pin_name(unsigned index);
unsigned himax_gpio_pin_number(unsigned index);
bool himax_gpio_pad_read(unsigned index, himax_pad_t* out);

/* Drive one pad as an input with the given pull (0 none, 1 down, 3 up) and
 * return the sampled level.  Used to tell a driven line from a floating one. */
bool himax_gpio_sample_input(unsigned gpio, unsigned pull, bool* level);

/* Bring-up probes: force the host-interface select or hold the IC in reset. */
void himax_gpio_set_mode(bool high);
void himax_gpio_hold_reset(bool assert);
bool himax_gpio_probe_device(void);

/* Optional clock stretching for bring-up; 0 keeps the fastest timing. */
void himax_gpio_set_half_period_us(unsigned half_us);
unsigned himax_gpio_half_period_us(void);

#endif /* HIMAX_GPIO_H */
