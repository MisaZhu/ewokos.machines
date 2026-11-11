/* Restricted Q8B NPU (SC8280XP NSP0) platform-control ABI.
 *
 * This is the kernel-side door for the cDSP/NSP bring-up.  It is deliberately
 * narrow: the caller can ask for the ABI, boot the signed firmware the kernel
 * has staged, and stop the remote processor.  No caller-selected SMC function
 * or destination address is ever accepted - the same model as q8b_gpu_pas.h.
 */
#ifndef Q8B_NPU_H
#define Q8B_NPU_H

#include <stdint.h>

#define Q8B_NPU_ABI_BASE   0x51385100u
#define Q8B_NPU_ABI        0x51385101u
/* Name used by the userspace FastRPC header.  Keep one value for the PAS
 * capability query so tools cannot accidentally compare it with FRPC_ABI. */
#ifndef Q8B_NPU_PAS_ABI
#define Q8B_NPU_PAS_ABI    Q8B_NPU_ABI
#endif

/* op codes (match the adrenonpu user-space transport header). */
#define Q8B_NPU_PAS_QUERY  (Q8B_NPU_ABI_BASE + 0x00u)
#define Q8B_NPU_PAS_INIT   (Q8B_NPU_ABI_BASE + 0x01u) /* owned DMA PA, metadata len */
#define Q8B_NPU_PAS_MEM    (Q8B_NPU_ABI_BASE + 0x02u) /* relocated span, 0 */
#define Q8B_NPU_PAS_STAGE  (Q8B_NPU_ABI_BASE + 0x03u) /* owned DMA PA, span */
#define Q8B_NPU_PAS_AUTH   (Q8B_NPU_ABI_BASE + 0x04u) /* 0, 0 */
#define Q8B_NPU_PAS_BOOT   (Q8B_NPU_ABI_BASE + 0x05u) /* 0, 0 */
#define Q8B_NPU_PAS_STOP   (Q8B_NPU_ABI_BASE + 0x06u) /* 0, 0 */
#define Q8B_NPU_PAS_STATUS (Q8B_NPU_ABI_BASE + 0x07u) /* 0, 0 -> state bits */
#define Q8B_NPU_FRPC_QUERY (Q8B_NPU_ABI_BASE + 0x0fu) /* 0, 0 */
#define Q8B_NPU_FRPC_SESSION (Q8B_NPU_ABI_BASE + 0x10u) /* domain, 0 */
#define Q8B_NPU_FRPC_CLOSE (Q8B_NPU_ABI_BASE + 0x11u) /* session, 0 */
#define Q8B_NPU_FRPC_INVOKE (Q8B_NPU_ABI_BASE + 0x12u) /* payload PA, size */
#define Q8B_NPU_FRPC_MMAP  (Q8B_NPU_ABI_BASE + 0x13u) /* (sid<<32)|pfn, size */
#define Q8B_NPU_FRPC_MUNMAP (Q8B_NPU_ABI_BASE + 0x14u) /* sid<<32|iova, size */
#define Q8B_NPU_FRPC_PWR   (Q8B_NPU_ABI_BASE + 0x15u) /* session/level, 0 */
#define Q8B_NPU_SMMU_READ_ID (Q8B_NPU_ABI_BASE + 0x16u) /* explicit ID offset, 0 */
#define Q8B_NPU_SMMU_CHECK (Q8B_NPU_ABI_BASE + 0x17u) /* context setup/teardown, 0, 0 */
#define Q8B_NPU_SYS_RESET  (Q8B_NPU_ABI_BASE + 0x7fu) /* PSCI SYSTEM_RESET */

/* Nonblocking calls retain owned DMA until POLL consumes the response or
 * PAS_STOP has successfully quiesced the DSP.  Required for reverse RPC. */
#define Q8B_NPU_FRPC_SUBMIT (Q8B_NPU_ABI_BASE + 0x18u) /* payload PA, size */
#define Q8B_NPU_FRPC_POLL   (Q8B_NPU_ABI_BASE + 0x19u) /* same payload PA, size */
/* Same packet as SUBMIT, originating in the reverse-RPC listener worker. */
#define Q8B_NPU_FRPC_SUBMIT_LISTENER (Q8B_NPU_ABI_BASE + 0x1au)
/* Query the optional page-aligned metadata format; no DSP access. */
#define Q8B_NPU_FRPC_METADATA_QUERY (Q8B_NPU_ABI_BASE + 0x1bu)
#define Q8B_NPU_FRPC_METADATA_PAGE 4096u
#define Q8B_NPU_FRPC_PACKET_PAGE (1u << 31)
#define Q8B_NPU_FRPC_POINTER_QUERY (Q8B_NPU_ABI_BASE + 0x1cu)
#define Q8B_NPU_FRPC_DMA_QUERY (Q8B_NPU_ABI_BASE + 0x1du)
#define Q8B_NPU_FRPC_POINTER_HOST 1u
#define Q8B_NPU_FRPC_PACKET_HOST (1u << 30)
/* Read-only post-stop diagnostics, including healthy control snapshots.
 * READ takes owned DMA PA and a
 * packed (carveout byte offset << 32) | byte count; no selected MMIO/SMC. */
#define Q8B_NPU_DUMP_QUERY (Q8B_NPU_ABI_BASE + 0x1fu)
/* Before DMA_QUERY was added, dump clients used 0x1d with two zero args.
 * Keep that exact request working; a nonzero PA still selects DMA_QUERY. */
#define Q8B_NPU_DUMP_QUERY_LEGACY Q8B_NPU_FRPC_DMA_QUERY
#define Q8B_NPU_DUMP_READ  (Q8B_NPU_ABI_BASE + 0x1eu)
#define Q8B_NPU_DUMP_ABI  0x51384401u

#define Q8B_NPU_PAS_BUSY        (-1101)
#define Q8B_NPU_PAS_UNSUPPORTED (-1102)
#define Q8B_NPU_PAS_INVALID     (-1103)
#define Q8B_NPU_PAS_FAILED      (-1104)

/* PAS_STATUS state bits. A running bit only means PAS_AUTH_AND_RESET
 * returned successfully; READY and HANDOVER come from the DSP-owned SMP2P
 * item 94 and are required before a FastRPC session is useful. */
#define Q8B_NPU_STATE_BOOTED    (1u << 0)
#define Q8B_NPU_STATE_REMOTE    (1u << 1)
#define Q8B_NPU_STATE_READY     (1u << 2)
#define Q8B_NPU_STATE_HANDOVER  (1u << 3)
/* SMP2P fatal or a latched NSP0 watchdog; both invalidate the transport. */
#define Q8B_NPU_STATE_FATAL     (1u << 4)
#define Q8B_NPU_STATE_STOP_ACK  (1u << 5)
/* A prior failed DSP run requires a whole-board reset before PAS restart. */
#define Q8B_NPU_STATE_RECOVERY_REQUIRED (1u << 6)

/* A shutdown snapshot can retain READY/HANDOVER along with STOP_ACK. */
static inline int q8b_npu_state_ready(uint32_t state) {
    const uint32_t required = Q8B_NPU_STATE_REMOTE | Q8B_NPU_STATE_READY |
            Q8B_NPU_STATE_HANDOVER;
    return (state & required) == required &&
            !(state & (Q8B_NPU_STATE_FATAL | Q8B_NPU_STATE_STOP_ACK |
                Q8B_NPU_STATE_RECOVERY_REQUIRED));
}

/* The PAS door and the GLINK/FastRPC door are intentionally separate.  PAS
 * can be useful for bring-up even while the transport is unavailable. */
#define Q8B_NPU_FRPC_ABI        (Q8B_NPU_ABI_BASE + 0x03u)

/* SC8280XP NSP0 (radxa dragon-q8b DT: cdsp0-region@8a100000). */
#define Q8B_NSP0_PAS_ID     18u
#define Q8B_NSP0_CARVEOUT   0x8a100000ULL
#define Q8B_NSP0_SPAN       0x1e00000ULL

uint64_t platform_control_npu(uint64_t op, uint64_t arg, uint64_t size);

#endif
