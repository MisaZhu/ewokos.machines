/* Response-required RPMh completion, following Linux rpmh-rsc.c.
 * IRQ registers are relative to tcs_base, not the RSC configuration base. */
#ifndef Q8B_RPMH_TCS_H
#define Q8B_RPMH_TCS_H
#include <stdint.h>

#define Q8B_RSC_TCS_OFFSET 0x0d00u
#define Q8B_RSC_IRQ_ENABLE 0x00u
#define Q8B_RSC_IRQ_STATUS 0x04u
#define Q8B_RSC_IRQ_CLEAR  0x08u

typedef struct {
    void *context;
    uint32_t (*read32)(void *context, uint32_t rsc_offset);
    void (*relax)(void *context);
} q8b_rpmh_io_t;

/* The caller must clear and observe the previous IRQ before triggering.
 * COMPL can precede the IRQ and is diagnostic only, never completion. */
static inline int q8b_rpmh_wait_irq(const q8b_rpmh_io_t *io,
        uint32_t command_status_offset, uint32_t irq, unsigned attempts,
        uint32_t *compl_without_irq) {
    for(unsigned i = 0; i < attempts; i++) {
        uint32_t status = io->read32(io->context,
                Q8B_RSC_TCS_OFFSET + Q8B_RSC_IRQ_STATUS);
        uint32_t command = io->read32(io->context, command_status_offset);
        if(status & irq) return 0;
        if(command & (1u << 16)) (*compl_without_irq)++;
        io->relax(io->context);
    }
    return -1;
}
#endif
