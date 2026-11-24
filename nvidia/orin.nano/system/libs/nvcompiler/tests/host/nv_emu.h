/*
 * nv_emu.h - host-only sm_87 SASS warp emulator for the nvcompiler tests.
 *
 * Executes the subset of SASS the NIR backend emits, one warp (32 lanes) at a
 * time, with the divergence model the backend relies on: BSSY arms a barrier
 * with the executing lanes, BSYNC parks a lane until every armed lane that has
 * not exited arrives, barriers hold garbage until cleared.  It also enforces
 * what the hardware faults on or silently corrupts: registers outside the
 * declared count (the top two belong to the hardware), reads of registers that
 * were never written, arming an uncleared barrier, and deadlocks.
 * SPDX-License-Identifier: MIT
 */
#ifndef NV_EMU_H
#define NV_EMU_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NV_EMU_MAX_MEM 8

struct nv_emu_mem {
    uint64_t va;
    uint8_t* p;
    uint64_t size;
};

struct nv_emu_env {
    const uint32_t* code;      /* 4 words per instruction */
    uint32_t ninstr;
    uint32_t num_gprs;         /* declared register count (SHI_REGISTERS) */
    const uint8_t* cb0;
    uint32_t cb0_size;
    struct nv_emu_mem mem[NV_EMU_MAX_MEM];
    unsigned nmem;
    uint32_t block[3];         /* workgroup shape */
};

struct nv_emu;

/* Decode + statically check the program.  NULL on error (message in err). */
struct nv_emu* nv_emu_create(const struct nv_emu_env* env, char* err, size_t errsz);
void nv_emu_destroy(struct nv_emu* e);

/* Run every thread of one workgroup.  0 on success, -1 on fault (err). */
int nv_emu_run_cta(struct nv_emu* e, const uint32_t ctaid[3], char* err,
                   size_t errsz);

/* Executed instruction count (lane-steps) since creation. */
uint64_t nv_emu_steps(const struct nv_emu* e);

/* nvdisasm-style text for instruction `idx` (branch targets as @0xADDR,
 * immediates as #XXXXXXXX). */
void nv_emu_disasm(const struct nv_emu* e, uint32_t idx, char* out, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* NV_EMU_H */
