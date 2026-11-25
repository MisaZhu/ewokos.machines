/* Restricted QCOM8GEN3 NPU (SC8280XP NSP0) platform-control ABI.
 *
 * This is the kernel-side door for the cDSP/NSP bring-up.  It is deliberately
 * narrow: the caller can ask for the ABI, boot the signed firmware the kernel
 * has staged, and stop the remote processor.  No caller-selected SMC function
 * or destination address is ever accepted - the same model as qcom8gen3_gpu_pas.h.
 */
#ifndef QCOM8GEN3_NPU_H
#define QCOM8GEN3_NPU_H

#include <stdint.h>

#define QCOM8GEN3_NPU_ABI_BASE   0x51385100u
#define QCOM8GEN3_NPU_ABI        0x51385101u
/* Name used by the userspace FastRPC header.  Keep one value for the PAS
 * capability query so tools cannot accidentally compare it with FRPC_ABI. */
#ifndef QCOM8GEN3_NPU_PAS_ABI
#define QCOM8GEN3_NPU_PAS_ABI    QCOM8GEN3_NPU_ABI
#endif

/* op codes (match the adrenonpu user-space transport header). */
#define QCOM8GEN3_NPU_PAS_QUERY  (QCOM8GEN3_NPU_ABI_BASE + 0x00u)
#define QCOM8GEN3_NPU_PAS_INIT   (QCOM8GEN3_NPU_ABI_BASE + 0x01u) /* owned DMA PA, metadata len */
#define QCOM8GEN3_NPU_PAS_MEM    (QCOM8GEN3_NPU_ABI_BASE + 0x02u) /* relocated span, 0 */
#define QCOM8GEN3_NPU_PAS_STAGE  (QCOM8GEN3_NPU_ABI_BASE + 0x03u) /* owned DMA PA, span */
#define QCOM8GEN3_NPU_PAS_AUTH   (QCOM8GEN3_NPU_ABI_BASE + 0x04u) /* 0, 0 */
#define QCOM8GEN3_NPU_PAS_BOOT   (QCOM8GEN3_NPU_ABI_BASE + 0x05u) /* 0, 0 */
#define QCOM8GEN3_NPU_PAS_STOP   (QCOM8GEN3_NPU_ABI_BASE + 0x06u) /* 0, 0 */
#define QCOM8GEN3_NPU_PAS_STATUS (QCOM8GEN3_NPU_ABI_BASE + 0x07u) /* 0, 0 -> state bits */
#define QCOM8GEN3_NPU_FRPC_QUERY (QCOM8GEN3_NPU_ABI_BASE + 0x0fu) /* 0, 0 */
#define QCOM8GEN3_NPU_FRPC_SESSION (QCOM8GEN3_NPU_ABI_BASE + 0x10u) /* domain, 0 */
#define QCOM8GEN3_NPU_FRPC_CLOSE (QCOM8GEN3_NPU_ABI_BASE + 0x11u) /* session, 0 */
#define QCOM8GEN3_NPU_FRPC_INVOKE (QCOM8GEN3_NPU_ABI_BASE + 0x12u) /* payload PA, size */
#define QCOM8GEN3_NPU_FRPC_MMAP  (QCOM8GEN3_NPU_ABI_BASE + 0x13u) /* (sid<<32)|pfn, size */
#define QCOM8GEN3_NPU_FRPC_MUNMAP (QCOM8GEN3_NPU_ABI_BASE + 0x14u) /* sid<<32|iova, size */
#define QCOM8GEN3_NPU_FRPC_PWR   (QCOM8GEN3_NPU_ABI_BASE + 0x15u) /* session/level, 0 */
#define QCOM8GEN3_NPU_SMMU_READ_ID (QCOM8GEN3_NPU_ABI_BASE + 0x16u) /* explicit ID offset, 0 */
#define QCOM8GEN3_NPU_SMMU_CHECK (QCOM8GEN3_NPU_ABI_BASE + 0x17u) /* context setup/teardown, 0, 0 */
#define QCOM8GEN3_NPU_SYS_OFF    (QCOM8GEN3_NPU_ABI_BASE + 0x7eu) /* PSCI SYSTEM_OFF */
#define QCOM8GEN3_NPU_SYS_RESET  (QCOM8GEN3_NPU_ABI_BASE + 0x7fu) /* PSCI SYSTEM_RESET */
/* Secondary-core PSCI CPU_SUSPEND gate: arg != 0 enables, 0 disables (WFI).
 * Core 0 never suspends.  Defaults off at boot; powerd toggles it.  Returns
 * 0, or QCOM8GEN3_NPU_PAS_UNSUPPORTED when the kernel cannot resume a core. */
#define QCOM8GEN3_NPU_CPU_SLEEP  (QCOM8GEN3_NPU_ABI_BASE + 0x7du)
/* Read-only CPU_SUSPEND verification: returns a core bitmask (bit N = core
 * N) selected by arg; anything above 0xff is a QCOM8GEN3_NPU_PAS_* error.  TW220
 * has no wired kernel console, so powerd surfaces this in `status`. */
#define QCOM8GEN3_NPU_CPU_SLEEP_STATS (QCOM8GEN3_NPU_ABI_BASE + 0x7cu)
#define QCOM8GEN3_CPU_SLEEP_STATS_RESUMED 0u /* completed a power-down/resume cycle */
#define QCOM8GEN3_CPU_SLEEP_STATS_PARKED  1u /* inside CPU_SUSPEND right now */

/* Nonblocking calls retain owned DMA until POLL consumes the response or
 * PAS_STOP has successfully quiesced the DSP.  Required for reverse RPC. */
#define QCOM8GEN3_NPU_FRPC_SUBMIT (QCOM8GEN3_NPU_ABI_BASE + 0x18u) /* payload PA, size */
#define QCOM8GEN3_NPU_FRPC_POLL   (QCOM8GEN3_NPU_ABI_BASE + 0x19u) /* same payload PA, size */
/* Same packet as SUBMIT, originating in the reverse-RPC listener worker. */
#define QCOM8GEN3_NPU_FRPC_SUBMIT_LISTENER (QCOM8GEN3_NPU_ABI_BASE + 0x1au)
/* Query the optional page-aligned metadata format; no DSP access. */
#define QCOM8GEN3_NPU_FRPC_METADATA_QUERY (QCOM8GEN3_NPU_ABI_BASE + 0x1bu)
#define QCOM8GEN3_NPU_FRPC_METADATA_PAGE 4096u
#define QCOM8GEN3_NPU_FRPC_PACKET_PAGE (1u << 31)
#define QCOM8GEN3_NPU_FRPC_POINTER_QUERY (QCOM8GEN3_NPU_ABI_BASE + 0x1cu)
#define QCOM8GEN3_NPU_FRPC_DMA_QUERY (QCOM8GEN3_NPU_ABI_BASE + 0x1du)
#define QCOM8GEN3_NPU_FRPC_POINTER_HOST 1u
#define QCOM8GEN3_NPU_FRPC_PACKET_HOST (1u << 30)
/* Read-only post-stop diagnostics, including healthy control snapshots.
 * READ takes owned DMA PA and a
 * packed (carveout byte offset << 32) | byte count; no selected MMIO/SMC. */
#define QCOM8GEN3_NPU_DUMP_QUERY (QCOM8GEN3_NPU_ABI_BASE + 0x1fu)
/* Before DMA_QUERY was added, dump clients used 0x1d with two zero args.
 * Keep that exact request working; a nonzero PA still selects DMA_QUERY. */
#define QCOM8GEN3_NPU_DUMP_QUERY_LEGACY QCOM8GEN3_NPU_FRPC_DMA_QUERY
#define QCOM8GEN3_NPU_DUMP_READ  (QCOM8GEN3_NPU_ABI_BASE + 0x1eu)
#define QCOM8GEN3_NPU_DUMP_ABI  0x51384401u

#define QCOM8GEN3_NPU_PAS_BUSY        (-1101)
#define QCOM8GEN3_NPU_PAS_UNSUPPORTED (-1102)
#define QCOM8GEN3_NPU_PAS_INVALID     (-1103)
#define QCOM8GEN3_NPU_PAS_FAILED      (-1104)

/* PAS_STATUS state bits. A running bit only means PAS_AUTH_AND_RESET
 * returned successfully; READY and HANDOVER come from the DSP-owned SMP2P
 * item 94 and are required before a FastRPC session is useful. */
#define QCOM8GEN3_NPU_STATE_BOOTED    (1u << 0)
#define QCOM8GEN3_NPU_STATE_REMOTE    (1u << 1)
#define QCOM8GEN3_NPU_STATE_READY     (1u << 2)
#define QCOM8GEN3_NPU_STATE_HANDOVER  (1u << 3)
/* SMP2P fatal or a latched NSP0 watchdog; both invalidate the transport. */
#define QCOM8GEN3_NPU_STATE_FATAL     (1u << 4)
#define QCOM8GEN3_NPU_STATE_STOP_ACK  (1u << 5)
/* A prior failed DSP run requires a whole-board reset before PAS restart. */
#define QCOM8GEN3_NPU_STATE_RECOVERY_REQUIRED (1u << 6)

/* A shutdown snapshot can retain READY/HANDOVER along with STOP_ACK. */
static inline int qcom8gen3_npu_state_ready(uint32_t state) {
    const uint32_t required = QCOM8GEN3_NPU_STATE_REMOTE | QCOM8GEN3_NPU_STATE_READY |
            QCOM8GEN3_NPU_STATE_HANDOVER;
    return (state & required) == required &&
            !(state & (QCOM8GEN3_NPU_STATE_FATAL | QCOM8GEN3_NPU_STATE_STOP_ACK |
                QCOM8GEN3_NPU_STATE_RECOVERY_REQUIRED));
}

/* The PAS door and the GLINK/FastRPC door are intentionally separate.  PAS
 * can be useful for bring-up even while the transport is unavailable. */
#define QCOM8GEN3_NPU_FRPC_ABI        (QCOM8GEN3_NPU_ABI_BASE + 0x03u)

/* SC8280XP NSP0 (radxa dragon-qcom8gen3 DT: cdsp0-region@8a100000). */
#define QCOM8GEN3_NSP0_PAS_ID     18u
#define QCOM8GEN3_NSP0_CARVEOUT   0x8a100000ULL
#define QCOM8GEN3_NSP0_SPAN       0x1e00000ULL

uint64_t platform_control_npu(uint64_t op, uint64_t arg, uint64_t size);

#endif
