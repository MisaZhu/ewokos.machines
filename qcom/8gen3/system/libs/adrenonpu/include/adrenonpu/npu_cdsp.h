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
 *     The Q8B kernel implements the cDSP PAS door and returns a separate
 *     FastRPC verdict until the GLINK service is ready.  Unsupported boards
 *     still decline without touching firmware.
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

/* QAIRT 2.51 overview: SC8280X / 8cx Gen 3 has QNN socModel=37.
 * QnnHtpDeviceConfigShared.h uses decimal arch=68.  Linux socinfo's
 * hardware id 449 belongs to a different namespace. */
#define CDSP_SOC_SC8280XP   37u
#define CDSP_ARCH_V68       68u
#define CDSP_NSP_CORES      2u
#define CDSP_VTCM_KB        512u

/* Official QAIRT filenames for SDK integration.  These names do not imply
 * compatibility with npu_htp.c's private graph ABI.  The default backend
 * cannot use them until an SDK-generated adapter is implemented. */
#define CDSP_HTP_SKEL_URI          "file://libQnnHtpV68Skel.so"
#define CDSP_HTP_SYS_URI           "file://libQnnHtpV68Stub.so"

/* Firmware names + search path (rootfs layout mirrors /etc/firmware/adreno). */
#define CDSP_FW_DIR         "/etc/firmware/"
#define CDSP_FW_MDT         "qccdsp8280.mbn"
#define CDSP_FW_MDT_RADXA   "qcom/sc8280xp/qccdsp8280.mbn"
#define CDSP_FW_MDT_ALT     "qcom/sc8280xp/LENOVO/21BX/qccdsp8280.mbn"
#define CDSP_FW_SEG_FMT     "qccdsp8280.b%02u"

/* Bring-up verdicts for npu_cdsp_probe(). */
enum {
	NPU_PROBE_OK = 0,
	NPU_PROBE_NO_KERNEL_ABI,   /* SYS_PLATFORM_CTL rejects the NPU range */
	NPU_PROBE_NO_FIRMWARE,     /* signed cDSP image not staged           */
	NPU_PROBE_AUTH_FAILED,     /* TrustZone refused metadata/image       */
	NPU_PROBE_RPC_FAILED,      /* booted, but FastRPC session refused    */
	NPU_PROBE_HTP_ABI_MISSING  /* no compatible HTP graph adapter       */
};

int  npu_cdsp_probe(void);      /* idempotent; returns NPU_PROBE_* */
int  npu_cdsp_probe_unsigned(void); /* SDK unsigned DSP probes */
/* Lifecycle diagnostic: boot/validate PAS without creating USER_PD. The
 * caller owns shutdown even on failure. A subsequent session probe reuses
 * this boot; an existing/cached session probe rejects boot-only mode. */
int  npu_cdsp_boot_only(void);
/* Lifecycle diagnostic: create unsigned USER_PD without registering a
 * listener. Module loading needs the normal probe. Probe modes cannot be
 * mixed within a live session; call shutdown before changing modes. */
int  npu_cdsp_probe_session_unsigned(void);
/* Same listener-free diagnostic with attrs=0: use fastrpc_shell[_3] when
 * present, otherwise the firmware's default process image. */
int  npu_cdsp_probe_session(void);
/* Release only that diagnostic session, retaining PAS for another probe.
 * On failure, ownership remains with shutdown. No live listener is allowed. */
int  npu_cdsp_release_session_unsigned(void);
bool npu_cdsp_booted(void);
/* 0 means fully closed/stopped. Failure retains outstanding ownership for
 * retry and rejects new probes until shutdown completes. */
int  npu_cdsp_shutdown(void);

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
