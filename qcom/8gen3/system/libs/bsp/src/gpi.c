#include <bsp/gpi.h>
#include <string.h>
#ifdef Q8B_GPI_TEST
#include "../tests/gpi_test_io.h"
#else
#include <ewoksys/syscall.h>
#include <unistd.h>
static int gpi_map(uintptr_t va, uintptr_t pa, unsigned size) {
    return syscall3(SYS_MEM_MAP, va, pa, size) == va ? 0 : -1;
}
static uint32_t gpi_rd(uintptr_t base, unsigned off) {
    uint32_t value = *(volatile uint32_t*)(base + off);
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}
static void gpi_wr(uintptr_t base, unsigned off, uint32_t value) {
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint32_t*)(base + off) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
static void gpi_delay(unsigned us) { usleep(us); }
#endif

static unsigned state(q8b_gpi_t* gpi, unsigned context) {
    return (gpi_rd(gpi->regs, context) >> 20) & 15U;
}

static int command(q8b_gpi_t* gpi, unsigned context, unsigned opcode,
        unsigned expected) {
    unsigned event = context == 0x1000U;
    unsigned ch = event ? 0U : context / 0x80U;
    unsigned status = event ? 0x3094U : 0x3090U;
    unsigned clear = event ? 0x30a4U : 0x30a0U;
    unsigned bit = 1U << ch;
    /* Context state alone is not command completion. In particular Linux's
     * GPI driver documents that DEALLOC leaves hardware CHSTATE unchanged.
     * Never consume a stale acknowledgement from a previous command. */
    if (gpi_rd(gpi->regs, status) & bit) {
        gpi->poisoned = 1;
        return -1;
    }
    gpi_wr(gpi->regs, event ? 0x3010U : 0x3008U, opcode << 24 | ch);
    for (unsigned i = 0; i < 1000; i++) {
        if (gpi_rd(gpi->regs, status) & bit) {
            unsigned observed = state(gpi, context);
            gpi_wr(gpi->regs, clear, bit);
            if ((gpi_rd(gpi->regs, status) & bit) ||
                    (opcode != 10 && observed != expected)) {
                gpi->poisoned = 1;
                return -1;
            }
            return 0;
        }
        gpi_delay(100);
    }
    gpi->poisoned = 1;
    return -1;
}

int q8b_gpi_acquire(q8b_gpi_t* gpi, unsigned instance) {
    if (!gpi)
        return -1;
    /* Do not clear a live/uncertain handle, even when acquisition is retried. */
    if (gpi->event || gpi->channels || gpi->poisoned || gpi->retired)
        return -1;
    gpi->stage = "instance";
    if (instance >= 12)
        return -1;
    gpi->stage = "map";
    gpi->regs = 0x5b108000U;
    if (gpi_map(gpi->regs, 0x00810000U + instance * 0x4000U, 0x4000U))
        return -1;
    gpi->stage = "ownership";
    if (state(gpi, 0) || state(gpi, 0x80) || state(gpi, 0x1000) ||
            (gpi_rd(gpi->regs, 0x3090) & 3U) ||
            (gpi_rd(gpi->regs, 0x3094) & 1U))
        return -1;
    gpi->stage = "allocate-event";
    gpi->event = 1;
    if (command(gpi, 0x1000, 0, 1))
        return -1;
    for (unsigned ch = 0; ch < 2; ch++) {
        gpi->stage = ch ? "allocate-rx" : "allocate-tx";
        gpi->channels |= 1U << ch;
        if (command(gpi, ch * 0x80U, 0, 1))
            return -1;
    }
    gpi->stage = "allocated-no-dma";
    return 0;
}

int q8b_gpi_release(q8b_gpi_t* gpi) {
    if (!gpi || !gpi->regs || gpi->poisoned)
        return -1;
    /* No START is issued by this API. Reject an externally started channel;
     * deallocation is safe only for our allocated, inactive resources. */
    for (unsigned ch = 0; ch < 2; ch++)
        if ((gpi->channels & (1U << ch)) && state(gpi, ch * 0x80U) != 1) {
            gpi->poisoned = 1;
            gpi->stage = "unexpected-channel-state";
            return -1;
        }
    for (unsigned ch = 0; ch < 2; ch++) {
        if (!(gpi->channels & (1U << ch)))
            continue;
        gpi->stage = ch ? "deallocate-rx" : "deallocate-tx";
        if (command(gpi, ch * 0x80U, 10, 0))
            return -1;
        gpi->channels &= ~(1U << ch);
    }
    if (gpi->event) {
        gpi->stage = "deallocate-event";
        if (state(gpi, 0x1000) != 1) {
            gpi->poisoned = 1;
            return -1;
        }
        if (command(gpi, 0x1000, 10, 0))
            return -1;
        gpi->event = 0;
    }
    gpi->stage = "released";
    /* DEALLOC does not reset CHSTATE. A future central owner must track
     * free instances; a fresh state-only acquisition cannot prove reuse. */
    gpi->retired = 1;
    return 0;
}
