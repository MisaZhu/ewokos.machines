#ifndef Q8B_BSP_GPI_H
#define Q8B_BSP_GPI_H
#include <stdint.h>

/* Experimental QUP2 control-plane candidate; host tests only. It is not linked
 * into libbsp or spitest: its hardware command path has not been verified.
 * No rings or DMA are started by this API.
 * Initialize the handle to zero before first use. Caller must prepare QUP2
 * clocks and serialize ownership across processes.
 * A command timeout poisons this handle; do not retry it or reuse the instance
 * until the outstanding hardware state has been recovered. Command completion
 * requires a fresh raw control IRQ. DEALLOC retains hardware CHSTATE; after a
 * clean release this handle is retired. Reuse needs a persistent central owner
 * rather than inferring allocation solely from CHSTATE. */
typedef struct {
    uintptr_t regs;
    unsigned channels, event, poisoned, retired;
    const char* stage;
} q8b_gpi_t;

int q8b_gpi_acquire(q8b_gpi_t* gpi, unsigned instance);
int q8b_gpi_release(q8b_gpi_t* gpi);
#endif
