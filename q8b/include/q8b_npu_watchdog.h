/* NSP0 watchdog: sc8280xp.dtsi GIC_SPI578, distinct from SMP2P fatal. */
#ifndef Q8B_NPU_WATCHDOG_H
#define Q8B_NPU_WATCHDOG_H
#include <stdint.h>

#define Q8B_NPU_WDOG_INTID (578u + 32u)
typedef struct { uint32_t group, enabled, pending, active; } q8b_npu_wdog_sample_t;
typedef struct { int armed, tripped; } q8b_npu_wdog_state_t;

/* Read-only distributor access. Group0 may be security filtered, but an
 * observed pending bit is still evidence and must not be ignored. */
static inline int q8b_npu_wdog_read(uint32_t (*read32)(void *, uint32_t),
        void *context, q8b_npu_wdog_sample_t *sample) {
    if (!read32 || !sample) return -1;
    uint32_t typer = read32(context, 4u);
    if (typer == UINT32_MAX || 32u * ((typer & 31u) + 1u) <= Q8B_NPU_WDOG_INTID)
        return -1;
    uint32_t word = Q8B_NPU_WDOG_INTID / 32u, bit = Q8B_NPU_WDOG_INTID % 32u;
    sample->group = (read32(context, 0x080u + word * 4u) >> bit) & 1u;
    sample->enabled = (read32(context, 0x100u + word * 4u) >> bit) & 1u;
    sample->pending = (read32(context, 0x200u + word * 4u) >> bit) & 1u;
    sample->active = (read32(context, 0x300u + word * 4u) >> bit) & 1u;
    return 0;
}

/* Never clear a prior watchdog to permit another boot. Only a board reset
 * resets this kernel-lifetime latch. Reject pending state before PAS_INIT. */
static inline int q8b_npu_wdog_arm(q8b_npu_wdog_state_t *state,
        const q8b_npu_wdog_sample_t *sample) {
    state->armed = 0;
    if (sample && sample->pending) state->tripped = 1;
    if (!sample || state->tripped) return -1;
    state->armed = 1;
    return 0;
}

static inline int q8b_npu_wdog_observe(q8b_npu_wdog_state_t *state,
        const q8b_npu_wdog_sample_t *sample) {
    if (state->armed && sample && sample->pending) state->tripped = 1;
    return state->tripped;
}
#endif
