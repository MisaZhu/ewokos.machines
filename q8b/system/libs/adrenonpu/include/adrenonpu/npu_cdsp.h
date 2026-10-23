/*
 * npu_cdsp.h - SC8280XP cDSP (Hexagon V68 NSP) bring-up for the QNN HTP
 * backend.
 *
 * Reconstruction sources:
 *   - drivers/remoteproc/qcom_q6v5_pas.c (v6.6): the PAS boot sequence
 *     metadata -> qcom_scm_pas_mem_setup -> image stage -> auth_and_reset,
 *     with the "cdsp" firmware name from sc8280xp.dtsi
 *     (qcom/sc8280xp/LENOVO/21BX/cdsp.mbn on this board, same vendor path
 *     convention as the GPU zap blob).
 *   - drivers/remoteproc/qcom_q6v5.c: the Q6 handover / stop handshake.
 *   - The SCM calls themselves ride the restricted Q8B kernel ABI
 *     (Q8B_NPU_PAS_* in npu_fastrpc.h), mirroring how machines/q8b/
 *     kernel/bsp/gpu_pas.c wraps qcom_scm_pas_* for the GPU zap today.
 *     The current kernel only implements GPU PAS id 13, so every op here
 *     declines with Q8B_NPU_PAS_UNSUPPORTED until a cDSP PAS service
 *     (PAS id from the DT "qcom,pas-id" of the cdsp remoteproc node) is
 *     added kernel-side.
 *
 * Power/clock voting for execution (HAP_power DCVS v3 in the vendor stack)
 * rides the FastRPC PWR_VOTE op; see npu_htp.c.
 */
#ifndef NPU_CDSP_H
#define NPU_CDSP_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SC8280XP identity (JTAG socinfo id 530; Hexagon V68 NSP, 2 cores,
   512 KB VTCM per core - HTP v68 capability set). */
#define CDSP_SOC_SC8280XP   530u
#define CDSP_ARCH_V68       0x68u
#define CDSP_NSP_CORES      2u
#define CDSP_VTCM_KB        512u

/* HTP skel URI resolved through remote_handle64_open.  v68 matches the
   SC8280XP NSP; the vendor backend picks the same suffix from dsp_arch. */
#define CDSP_HTP_SKEL_URI   "file://libqnn_htp_v68_skel.so"
#define CDSP_HTP_SYS_URI    "file://libqnn_htp_v68_stubs.so"

/* Firmware names + search path (rootfs layout mirrors /etc/firmware/adreno). */
#define CDSP_FW_DIR         "/etc/firmware/npu/"
#define CDSP_FW_MDT         "qcom/sc8280xp/LENOVO/21BX/cdsp.mbn"
#define CDSP_FW_MDT_ALT     "cdsp.mbn"
#define CDSP_FW_SEG_FMT     "cdsp.b%02u"

/* Bring-up verdicts for npu_cdsp_probe(). */
enum {
	NPU_PROBE_OK = 0,
	NPU_PROBE_NO_KERNEL_ABI,   /* SYS_PLATFORM_CTL rejects the NPU range */
	NPU_PROBE_NO_FIRMWARE,     /* signed cDSP image not staged           */
	NPU_PROBE_AUTH_FAILED,     /* TrustZone refused metadata/image       */
	NPU_PROBE_RPC_FAILED       /* booted, but FastRPC session refused    */
};

int  npu_cdsp_probe(void);      /* idempotent; returns NPU_PROBE_* */
bool npu_cdsp_booted(void);
void npu_cdsp_shutdown(void);   /* best effort PAS stop */

/* dma-backed uncached buffer helpers (kernel sys_dma pool, block 0 - the
   same pool libadreno uses; see adreno_bo_alloc). */
typedef struct {
	void    *va;
	uint64_t pa;
	uint32_t size;
} npu_buf_t;

int  npu_buf_alloc(npu_buf_t *b, uint32_t size);
void npu_buf_free(npu_buf_t *b);

/* Cache maintenance around cDSP-visible writes.  The dma pool is mapped
   NOCACHE on EwokOS (sys_dma invariant), so both are barriers only - kept
   as named hooks because the Linux path needs real flushes and the HTP
   skip-flush tuning knob (qnn_htp_ext.h) toggles exactly this step. */
void npu_dcache_flush(const void *va, uint32_t size);
void npu_dcache_inval(const void *va, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif /* NPU_CDSP_H */
