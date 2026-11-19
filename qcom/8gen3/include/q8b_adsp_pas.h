/* Restricted Q8B ADSP (SC8280XP ADSP, PAS id 1) platform-control ABI.
 *
 * This is the kernel-side door for the ADSP bring-up.  It is deliberately
 * narrow: the caller can ask for the ABI, boot the signed firmware the kernel
 * has staged, and stop the remote processor.  No caller-selected SMC function
 * or destination address is ever accepted - the same model as q8b_npu.h and
 * q8b_gpu_pas.h.
 *
 * Phase 1 only authenticates and resets the ADSP.  SMP2P/GLINK/rpmsg and the
 * RPMh power-domain vote are intentionally absent and belong to a later phase.
 */
#ifndef Q8B_ADSP_PAS_H
#define Q8B_ADSP_PAS_H

#include <stdint.h>

#define Q8B_ADSP_ABI_BASE   0x51384100u
#define Q8B_ADSP_ABI        0x51384101u

/* op codes (QUERY/BOOT/STOP/STATUS only; no five-step ABI in Phase 1). */
#define Q8B_ADSP_PAS_QUERY  (Q8B_ADSP_ABI_BASE + 0x00u) /* 0, 0 -> ABI */
#define Q8B_ADSP_PAS_BOOT   (Q8B_ADSP_ABI_BASE + 0x05u) /* owned DMA PA, size */
#define Q8B_ADSP_PAS_STOP   (Q8B_ADSP_ABI_BASE + 0x06u) /* 0, 0 */
#define Q8B_ADSP_PAS_STATUS (Q8B_ADSP_ABI_BASE + 0x07u) /* 0, 0 -> state bits */

/* Battery-manager (PMIC GLINK battmgr) transport door.  Distinct from the PAS
 * door: PAS boots the ADSP, BATTMGR queries the real battery state once the
 * GLINK/GPR transport to the charger_pd service is up (Phase 2/3). */
#define Q8B_ADSP_BATTMGR_QUERY    (Q8B_ADSP_ABI_BASE + 0x10u) /* 0, 0 -> ABI */
#define Q8B_ADSP_BATTMGR_CAPACITY (Q8B_ADSP_ABI_BASE + 0x11u) /* 0, 0 -> packed */
#define Q8B_ADSP_BATTMGR_ABI      0x51384110u

/* Packed return value of BATTMGR_CAPACITY: low byte capacity 0..100, then
 * charging/present flags.  Negative values are Q8B_ADSP_PAS_* errors. */
#define Q8B_ADSP_BATT_CAP_MASK    0xffu
#define Q8B_ADSP_BATT_CHARGING    (1u << 8)
#define Q8B_ADSP_BATT_PRESENT     (1u << 9)

#define Q8B_ADSP_PAS_BUSY        (-1201)
#define Q8B_ADSP_PAS_UNSUPPORTED (-1202)
#define Q8B_ADSP_PAS_INVALID     (-1203)
#define Q8B_ADSP_PAS_FAILED      (-1204)
#define Q8B_ADSP_PAS_POWER       (-1205)  /* power vote failed */
#define Q8B_ADSP_PAS_INIT        (-1206)  /* PAS_INIT_IMAGE failed */
#define Q8B_ADSP_PAS_MEM         (-1207)  /* PAS_MEM_SETUP failed */
#define Q8B_ADSP_PAS_AUTH        (-1208)  /* PAS_AUTH_AND_RESET failed */

/* PAS_STATUS state bits. BOOTED = PAS_AUTH_AND_RESET succeeded. */
#define Q8B_ADSP_STATE_BOOTED       (1u << 0)

/* SC8280XP ADSP PAS id and carveout, from upstream sc8280xp.dtsi:
 *   remoteproc_adsp is "qcom,sc8280xp-adsp-pas" -> sm8250_adsp_resource
 *   (.pas_id = 1), memory-region = <&pil_adsp_mem> = adsp-region@86c00000
 *   (32 MiB). */
#define Q8B_ADSP_PAS_ID     1u
#define Q8B_ADSP_CARVEOUT   0x86c00000ULL
#define Q8B_ADSP_SPAN       0x2000000ULL

uint64_t platform_control_adsp(uint64_t op, uint64_t arg, uint64_t size);

#endif
