/*
 * q8b_rpmh_batch: response-required RPMh TCS command batch submitter for
 * the apps RSC (drv-2, 0x18220000), mirroring npu_pas.c's minimal voter.
 * BLUEPRINT ONLY: compiled into the kernel image but not wired to any boot
 * path yet.  A single command with fields msgid=RPMH_WRITE_RESPONSE and
 * (address, data); more commands fill the remaining 7 slots of one batch
 * (all slots must share the same trigger).
 *
 * The fresh-IRQ completion rule is enforced via q8b_rpmh_tcs.h: a retained
 * CMD_STATUS.COMPL is diagnostic only and never completes the write.
 * Any failure returns a negative stage; callers must never retry without
 * full TCS teardown.
 */
#include <stdint.h>
#include "q8b_rpmh_tcs.h"

#define Q8B_RH_RSC_PHY   0x18220000u
#define Q8B_RH_RSC_SIZE  0x10000u
#define Q8B_RH_TCS_OFF   Q8B_RSC_TCS_OFFSET
#define Q8B_RH_STRIDE    672u
#define Q8B_RH_IRQ       1u
#define Q8B_RH_AMC_ENABLE  (1u << 16)
#define Q8B_RH_AMC_TRIGGER (1u << 24)
#define Q8B_RH_WRITE_RESP 0x00010108u
#define Q8B_RH_BATCH_MAX  8u

/* Command slots, offsets relative to a TCS base. */
#define Q8B_RH_CTRL    0x14u
#define Q8B_RH_STATUS  0x18u
#define Q8B_RH_CMD_EN  0x1cu
#define Q8B_RH_CMD_STRIDE 20u
#define Q8B_RH_C_MSGID 0x30u
#define Q8B_RH_C_ADDR  0x34u
#define Q8B_RH_C_DATA  0x38u
#define Q8B_RH_C_STATUS 0x3cu

typedef struct { uint32_t address, data; } q8b_rpmh_batch_cmd_t;

/* The kernel maps the RSC window before use (arch_vm or the same fixed
 * high window npu_pas uses).  Exposed as a macro so callers can relocate. */
#ifndef Q8B_RH_RSC_WIN
#define Q8B_RH_RSC_WIN 0x5020300000ull
#endif

static inline uint32_t rh_reg(unsigned tcs, uint32_t reg) {
    return *(volatile uint32_t*)(Q8B_RH_RSC_WIN + Q8B_RH_TCS_OFF +
            tcs * Q8B_RH_STRIDE + reg);
}
static inline void rh_wr(unsigned tcs, uint32_t reg, uint32_t val) {
    *(volatile uint32_t*)(Q8B_RH_RSC_WIN + Q8B_RH_TCS_OFF +
            tcs * Q8B_RH_STRIDE + reg) = val;
}
static inline int rh_wr_sync(unsigned tcs, uint32_t reg, uint32_t val) {
    rh_wr(tcs, reg, val);
    for (unsigned i = 0; i < 100000; i++)
        if (rh_reg(tcs, reg) == val)
            return 0;
    return -1;
}
static uint32_t rh_read32(void* ctx, uint32_t off) {
    (void)ctx;
    return *(volatile uint32_t*)(Q8B_RH_RSC_WIN + off);
}
static void rh_relax(void* ctx) { (void)ctx; __asm__ volatile("yield" ::: "memory"); }

/* Submit up to Q8B_RH_BATCH_MAX commands in one TCS transaction.  Returns
 * 0 on fresh-IRQ completion, negative stage otherwise; out_stage receives
 * the failing step for diagnostics. */
int q8b_rpmh_batch(const q8b_rpmh_batch_cmd_t* cmds, unsigned n,
                   uint32_t* out_stage) {
    const unsigned tcs = 0;
    if (!cmds || n == 0 || n > Q8B_RH_BATCH_MAX)
        return -1;
    if (out_stage) *out_stage = 1;

    uint32_t control = rh_reg(tcs, Q8B_RH_CTRL) & ~Q8B_RH_AMC_TRIGGER;
    if (rh_wr_sync(tcs, Q8B_RH_CTRL, control) != 0) { if (out_stage) *out_stage = 2; return -2; }
    control &= ~Q8B_RH_AMC_ENABLE;
    if (rh_wr_sync(tcs, Q8B_RH_CTRL, control) != 0) { if (out_stage) *out_stage = 2; return -2; }
    if (rh_wr_sync(tcs, Q8B_RH_CMD_EN, 0) != 0) { if (out_stage) *out_stage = 3; return -3; }
    *(volatile uint32_t*)(Q8B_RH_RSC_WIN + Q8B_RH_TCS_OFF + Q8B_RSC_IRQ_CLEAR) = Q8B_RH_IRQ;
    __asm__ volatile("dsb sy" ::: "memory");
    for (unsigned i = 0; i < 100000; i++)
        if (!(*(volatile uint32_t*)(Q8B_RH_RSC_WIN + Q8B_RH_TCS_OFF +
                        Q8B_RSC_IRQ_STATUS) & Q8B_RH_IRQ))
            break;
    *(volatile uint32_t*)(Q8B_RH_RSC_WIN + Q8B_RH_TCS_OFF + Q8B_RSC_IRQ_ENABLE) |=
            Q8B_RH_IRQ;

    for (unsigned i = 0; i < Q8B_RH_BATCH_MAX; i++) {
        uintptr_t b = Q8B_RH_RSC_WIN + Q8B_RH_TCS_OFF + tcs * Q8B_RH_STRIDE +
                      i * Q8B_RH_CMD_STRIDE;
        if (i < n) {
            *(volatile uint32_t*)(b + Q8B_RH_C_MSGID) = Q8B_RH_WRITE_RESP;
            *(volatile uint32_t*)(b + Q8B_RH_C_ADDR) = cmds[i].address;
            *(volatile uint32_t*)(b + Q8B_RH_C_DATA) = cmds[i].data;
        } else {
            *(volatile uint32_t*)(b + Q8B_RH_C_MSGID) = 0;
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
    if (rh_wr_sync(tcs, Q8B_RH_CMD_EN, 1) != 0) { if (out_stage) *out_stage = 4; return -4; }
    if (rh_wr_sync(tcs, Q8B_RH_CTRL, Q8B_RH_AMC_ENABLE) != 0) { if (out_stage) *out_stage = 5; return -5; }
    rh_wr(tcs, Q8B_RH_CTRL, Q8B_RH_AMC_ENABLE | Q8B_RH_AMC_TRIGGER);
    const q8b_rpmh_io_t io = { 0, rh_read32, rh_relax };
    uint32_t compl_without_irq = 0;
    uint32_t cmd_status = Q8B_RH_TCS_OFF + tcs * Q8B_RH_STRIDE +
                          Q8B_RH_C_STATUS;
    if (q8b_rpmh_wait_irq(&io, cmd_status, Q8B_RH_IRQ, 1000000,
                          &compl_without_irq) != 0) {
        if (out_stage) *out_stage = 6;
        return -6;
    }
    control = rh_reg(tcs, Q8B_RH_CTRL) & ~Q8B_RH_AMC_TRIGGER;
    if (rh_wr_sync(tcs, Q8B_RH_CTRL, control) != 0 ||
            rh_wr_sync(tcs, Q8B_RH_CTRL, 0) != 0 ||
            rh_wr_sync(tcs, Q8B_RH_CMD_EN, 0) != 0) {
        if (out_stage) *out_stage = 7;
        return -7;
    }
    *(volatile uint32_t*)(Q8B_RH_RSC_WIN + Q8B_RH_TCS_OFF + Q8B_RSC_IRQ_CLEAR) =
            Q8B_RH_IRQ;
    if (out_stage) *out_stage = 0;
    return 0;
}