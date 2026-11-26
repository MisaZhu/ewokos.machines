/* Restricted QCOM8GEN3 ADSP (SC8280XP ADSP, PAS id 1) platform-control ABI.
 *
 * This is the kernel-side door for the ADSP bring-up.  It is deliberately
 * narrow: the caller can ask for the ABI, boot the signed firmware the kernel
 * has staged, and stop the remote processor.  No caller-selected SMC function
 * or destination address is ever accepted - the same model as qcom8gen3_npu.h and
 * qcom8gen3_gpu_pas.h.
 *
 * Phase 1 only authenticates and resets the ADSP.  SMP2P/GLINK/rpmsg and the
 * RPMh power-domain vote are intentionally absent and belong to a later phase.
 */
#ifndef QCOM8GEN3_ADSP_PAS_H
#define QCOM8GEN3_ADSP_PAS_H

#include <stdint.h>

#define QCOM8GEN3_ADSP_ABI_BASE   0x51384100u
#define QCOM8GEN3_ADSP_ABI        0x51384101u

/* op codes (QUERY/BOOT/STOP/STATUS only; no five-step ABI in Phase 1). */
#define QCOM8GEN3_ADSP_PAS_QUERY  (QCOM8GEN3_ADSP_ABI_BASE + 0x00u) /* 0, 0 -> ABI */
#define QCOM8GEN3_ADSP_PAS_BOOT   (QCOM8GEN3_ADSP_ABI_BASE + 0x05u) /* owned DMA PA, size */
#define QCOM8GEN3_ADSP_PAS_STOP   (QCOM8GEN3_ADSP_ABI_BASE + 0x06u) /* 0, 0 */
#define QCOM8GEN3_ADSP_PAS_STATUS (QCOM8GEN3_ADSP_ABI_BASE + 0x07u) /* 0, 0 -> state bits */

/* Battery-manager (PMIC GLINK battmgr) transport door.  Distinct from the PAS
 * door: PAS boots the ADSP, BATTMGR queries the real battery state once the
 * GLINK/GPR transport to the charger_pd service is up (Phase 2/3). */
#define QCOM8GEN3_ADSP_BATTMGR_QUERY    (QCOM8GEN3_ADSP_ABI_BASE + 0x10u) /* 0, 0 -> ABI */
#define QCOM8GEN3_ADSP_BATTMGR_CAPACITY (QCOM8GEN3_ADSP_ABI_BASE + 0x11u) /* 0, 0 -> packed */
#define QCOM8GEN3_ADSP_BATTMGR_ABI      0x51384110u

/* Packed return value of BATTMGR_CAPACITY: low byte capacity 0..100, then
 * charging/present flags.  Negative values are QCOM8GEN3_ADSP_PAS_* errors. */
#define QCOM8GEN3_ADSP_BATT_CAP_MASK    0xffu
#define QCOM8GEN3_ADSP_BATT_CHARGING    (1u << 8)
#define QCOM8GEN3_ADSP_BATT_PRESENT     (1u << 9)

#define QCOM8GEN3_ADSP_PAS_BUSY        (-1201)
#define QCOM8GEN3_ADSP_PAS_UNSUPPORTED (-1202)
#define QCOM8GEN3_ADSP_PAS_INVALID     (-1203)
#define QCOM8GEN3_ADSP_PAS_FAILED      (-1204)
#define QCOM8GEN3_ADSP_PAS_POWER       (-1205)  /* power vote failed */
#define QCOM8GEN3_ADSP_PAS_INIT        (-1206)  /* PAS_INIT_IMAGE failed */
#define QCOM8GEN3_ADSP_PAS_MEM         (-1207)  /* PAS_MEM_SETUP failed */
#define QCOM8GEN3_ADSP_PAS_AUTH        (-1208)  /* PAS_AUTH_AND_RESET failed */

/* PAS_STATUS state bits. BOOTED = PAS_AUTH_AND_RESET succeeded. */
#define QCOM8GEN3_ADSP_STATE_BOOTED       (1u << 0)

/* SC8280XP ADSP PAS id and carveout, from upstream sc8280xp.dtsi:
 *   remoteproc_adsp is "qcom,sc8280xp-adsp-pas" -> sm8250_adsp_resource
 *   (.pas_id = 1), memory-region = <&pil_adsp_mem> = adsp-region@86c00000
 *   (32 MiB). */
#define QCOM8GEN3_ADSP_PAS_ID     1u
#define QCOM8GEN3_ADSP_CARVEOUT   0x86c00000ULL
#define QCOM8GEN3_ADSP_SPAN       0x2000000ULL

uint64_t platform_control_adsp(uint64_t op, uint64_t arg, uint64_t size);

#endif
