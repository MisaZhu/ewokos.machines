/*
 * adreno_a6xx.c - Adreno 690 (SC8280XP / Q8B) GPU core bring-up + selftest.
 *
 * Top-level orchestrator for the M1c bring-up chain.  adreno_a6xx_init() runs,
 * in Linux a6xx_start()/hw_init() order (drivers/gpu/drm/msm/adreno/a6xx_gpu.c
 * v6.6):
 *
 *   reg map -> gpucc clocks -> ring/shadow alloc -> SMMU CB0 -> GMU boot ->
 *   SQE ucode load -> a6xx hw_init (GBIF/SECVID/addr-mode/HWCG/
 *   QoS/UCHE/ROQ/UBWC/protect/apriv/int-mask) -> CP_RB/SQE program ->
 *   CP_ME_INIT -> secure-mode exit -> CP_MEM_WRITE datapath selftest
 *
 * A690 is ADRENO_6XX_GEN4, so in Linux terms adreno_is_a660_family() AND
 * adreno_is_a650_family() are both true and hw_apriv is set; every per-family
 * branch below takes the a660/a650 path (a6xx_gpu.c:1253-1381).
 *
 * Register offsets: GPU core from a6xx_regs.h, GMU from a6xx_gmu_regs.h.  The
 * handful of array-base registers not emitted into a6xx_regs.h (CP_PROTECT,
 * CP_PERFCTR_CP_SEL, TPL1_BICUBIC_WEIGHTS_TABLE, GBIF_QSB_SIDE) use the base +
 * stride from the Mesa a6xx.xml and are defined locally below.
 *
 * HARD DEPENDENCIES (hardware progress is recorded in test/HANDOVER.md):
 *   - Clocks depend on adreno_gpucc_init() and on GCC gpll0/gpll0_div already
 *     running (bootloader/GCC dependency).
 *   - CP_LPAC_PROG_FIFO_SIZE uses offset 0xb34.  The XML labels the register
 *     A7XX, but documents that the LPAC fields appeared on A660 and current
 *     Linux writes this offset for every A660-family GPU, including A690.
 *   - Q8B's DT enables a GPU zap shader. Secure-mode exit therefore requires
 *     its official signed image authenticated through TrustZone SCM PAS
 *     (qcom_scm_pas_auth_and_reset), via the restricted Q8B kernel service.
 *     Missing SCM support is an error, not Linux's no-zap-device -ENODEV
 *     fallback. Do not write SECVID_TRUST_CNTL on that assumption.
 *   - The GMU OOB "GPU_SET" keep-alive surrounds hw_init and CP startup, as
 *     required by the non-legacy Linux path.  GX core registers must not be
 *     accessed before that request is acknowledged.
 */
#include <adreno/adreno_drv.h>
#include <adreno/adreno_reg.h>
#include <adreno/adreno_fw.h>
#include <adreno/adreno_pm4.h>
#include <adreno/a6xx_regs.h>
#include <adreno/a6xx_gmu_regs.h>
#include <ewoksys/klog.h>
#include <ewoksys/proc.h>
#include <ewoksys/syscall.h>
#include <q8b_gpu_pas.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

/* --- array-base registers not emitted into a6xx_regs.h (Mesa a6xx.xml) --- */
#define REG_A6XX_CP_PROTECT(n)            (0x00000850u + (n))  /* stride 1 */
#define REG_A6XX_CP_PERFCTR_CP_SEL(n)     (0x000008d0u + (n))  /* stride 1 */
#define REG_A6XX_TPL1_BICUBIC_WEIGHTS(n)  (0x0000b608u + (n))  /* stride 1, len 5 */
#define REG_A6XX_GBIF_QSB_SIDE(n)         (0x00003c03u + (n))  /* stride 1 */
/* The XML labels this A7XX, but its own note and Linux's A660-family path
   confirm that LPAC_PROG_FIFO_SIZE first appeared on A660. */
#define REG_A6XX_CP_LPAC_PROG_FIFO_SIZE    0x00000b34u

/* CP_PROTECT span encoding (a6xx_gpu.h:45-55). */
#define A6XX_PROTECT_NORDWR(_reg, _len) \
	((1u << 31) | (((_len) & 0x3fffu) << 18) | ((_reg) & 0x3ffffu))
#define A6XX_PROTECT_RDONLY(_reg, _len) \
	((((_len) & 0x3fffu) << 18) | ((_reg) & 0x3ffffu))

/* CP_PROTECT_CNTL bits (a6xx.xml a6xx_cp_protect_cntl). */
#define A6XX_CP_PROTECT_CNTL_ACCESS_PROT_EN          (1u << 0)
#define A6XX_CP_PROTECT_CNTL_ACCESS_FAULT_ON_VIOL_EN (1u << 1)
#define A6XX_CP_PROTECT_CNTL_LAST_SPAN_INF_RANGE     (1u << 3)

#define PERF_CP_ALWAYS_COUNT  0u   /* a5xx.xml enum value 0 */

/* A6XX_INT_MASK (a6xx_gpu.c:1183-1193; bit positions from a6xx.xml). */
#define A6XX_INT_MASK  0x03d0e242u

#define gpu_read(w)        adreno_gpu_read(w)
#define gpu_write(w, v)    adreno_gpu_write((w), (v))
#define gmu_write(w, v)    adreno_gmu_write((w), (v))
#define gmu_rmw(w, m, v)   adreno_gmu_rmw((w), (m), (v))
static void gpu_write64(uint32_t w, uint64_t v) {
	gpu_write(w,     (uint32_t)(v & 0xffffffffu));
	gpu_write(w + 1, (uint32_t)(v >> 32));
}

/* ------------------------------------------------------------------ */
/* a690_hwcg (a6xx_gpu.c:641-696) - GPU-core clock-gating registers.    */
/* The three trailing GPU_GMU_AO_GMU_CGC_* entries are GMU-domain and    */
/* are written through the GMU aperture separately in a6xx_set_hwcg().   */
/* ------------------------------------------------------------------ */
typedef struct { uint32_t offset; uint32_t value; } regpair_t;
static const regpair_t a690_hwcg[] = {
	{REG_A6XX_RBBM_CLOCK_CNTL_SP0, 0x02222222},
	{REG_A6XX_RBBM_CLOCK_CNTL2_SP0, 0x02222220},
	{REG_A6XX_RBBM_CLOCK_DELAY_SP0, 0x00000080},
	{REG_A6XX_RBBM_CLOCK_HYST_SP0, 0x0000f3cf},
	{REG_A6XX_RBBM_CLOCK_CNTL_TP0, 0x22222222},
	{REG_A6XX_RBBM_CLOCK_CNTL2_TP0, 0x22222222},
	{REG_A6XX_RBBM_CLOCK_CNTL3_TP0, 0x22222222},
	{REG_A6XX_RBBM_CLOCK_CNTL4_TP0, 0x00022222},
	{REG_A6XX_RBBM_CLOCK_DELAY_TP0, 0x11111111},
	{REG_A6XX_RBBM_CLOCK_DELAY2_TP0, 0x11111111},
	{REG_A6XX_RBBM_CLOCK_DELAY3_TP0, 0x11111111},
	{REG_A6XX_RBBM_CLOCK_DELAY4_TP0, 0x00011111},
	{REG_A6XX_RBBM_CLOCK_HYST_TP0, 0x77777777},
	{REG_A6XX_RBBM_CLOCK_HYST2_TP0, 0x77777777},
	{REG_A6XX_RBBM_CLOCK_HYST3_TP0, 0x77777777},
	{REG_A6XX_RBBM_CLOCK_HYST4_TP0, 0x00077777},
	{REG_A6XX_RBBM_CLOCK_CNTL_RB0, 0x22222222},
	{REG_A6XX_RBBM_CLOCK_CNTL2_RB0, 0x01002222},
	{REG_A6XX_RBBM_CLOCK_CNTL_CCU0, 0x00002220},
	{REG_A6XX_RBBM_CLOCK_HYST_RB_CCU0, 0x00040f00},
	{REG_A6XX_RBBM_CLOCK_CNTL_RAC, 0x25222022},
	{REG_A6XX_RBBM_CLOCK_CNTL2_RAC, 0x00005555},
	{REG_A6XX_RBBM_CLOCK_DELAY_RAC, 0x00000011},
	{REG_A6XX_RBBM_CLOCK_HYST_RAC, 0x00445044},
	{REG_A6XX_RBBM_CLOCK_CNTL_TSE_RAS_RBBM, 0x04222222},
	{REG_A6XX_RBBM_CLOCK_MODE_VFD, 0x00002222},
	{REG_A6XX_RBBM_CLOCK_MODE_GPC, 0x00222222},
	{REG_A6XX_RBBM_CLOCK_DELAY_HLSQ_2, 0x00000002},
	{REG_A6XX_RBBM_CLOCK_MODE_HLSQ, 0x00002222},
	{REG_A6XX_RBBM_CLOCK_DELAY_TSE_RAS_RBBM, 0x00004000},
	{REG_A6XX_RBBM_CLOCK_DELAY_VFD, 0x00002222},
	{REG_A6XX_RBBM_CLOCK_DELAY_GPC, 0x00000200},
	{REG_A6XX_RBBM_CLOCK_DELAY_HLSQ, 0x00000000},
	{REG_A6XX_RBBM_CLOCK_HYST_TSE_RAS_RBBM, 0x00000000},
	{REG_A6XX_RBBM_CLOCK_HYST_VFD, 0x00000000},
	{REG_A6XX_RBBM_CLOCK_HYST_GPC, 0x04104004},
	{REG_A6XX_RBBM_CLOCK_HYST_HLSQ, 0x00000000},
	{REG_A6XX_RBBM_CLOCK_CNTL_TEX_FCHE, 0x00000222},
	{REG_A6XX_RBBM_CLOCK_DELAY_TEX_FCHE, 0x00000111},
	{REG_A6XX_RBBM_CLOCK_HYST_TEX_FCHE, 0x00000000},
	{REG_A6XX_RBBM_CLOCK_CNTL_UCHE, 0x22222222},
	{REG_A6XX_RBBM_CLOCK_HYST_UCHE, 0x00000004},
	{REG_A6XX_RBBM_CLOCK_DELAY_UCHE, 0x00000002},
	{REG_A6XX_RBBM_CLOCK_CNTL, 0x8aa8aa82},
	{REG_A6XX_RBBM_ISDB_CNT, 0x00000182},
	{REG_A6XX_RBBM_RAC_THRESHOLD_CNT, 0x00000000},
	{REG_A6XX_RBBM_SP_HYST_CNT, 0x00000000},
	{REG_A6XX_RBBM_CLOCK_CNTL_GMU_GX, 0x00000222},
	{REG_A6XX_RBBM_CLOCK_DELAY_GMU_GX, 0x00000111},
	{REG_A6XX_RBBM_CLOCK_HYST_GMU_GX, 0x00000555},
	{0, 0}
};
#define A690_HWCG_CLOCK_CNTL_ON  0x8aa8aa82u

static void a6xx_set_hwcg(adreno_dev_t *d, int state) {
	const regpair_t *reg;
	uint32_t val;
	(void)d;

	val = gpu_read(REG_A6XX_RBBM_CLOCK_CNTL);
	if ((!state && !val) || (state && val == A690_HWCG_CLOCK_CNTL_ON))
		return;

	/* Disable the SP clock before programming the HWCG registers. */
	gmu_rmw(REG_A6XX_GPU_GMU_GX_SPTPRAC_CLOCK_CONTROL, 1, 0);

	for (reg = a690_hwcg; reg->offset; reg++)
		gpu_write(reg->offset, state ? reg->value : 0);

	/* The GMU AO clock-gating controls live in the GMU aperture. */
	gmu_write(REG_A6XX_GPU_GMU_AO_GMU_CGC_MODE_CNTL,  state ? 0x20200 : 0);
	gmu_write(REG_A6XX_GPU_GMU_AO_GMU_CGC_DELAY_CNTL, state ? 0x10111 : 0);
	gmu_write(REG_A6XX_GPU_GMU_AO_GMU_CGC_HYST_CNTL,  state ? 0x5555  : 0);

	/* Re-enable the SP clock, then turn the core clock gate on. */
	gmu_rmw(REG_A6XX_GPU_GMU_GX_SPTPRAC_CLOCK_CONTROL, 0, 1);
	gpu_write(REG_A6XX_RBBM_CLOCK_CNTL, state ? A690_HWCG_CLOCK_CNTL_ON : 0);
}

/* ------------------------------------------------------------------ */
/* a690_protect (a6xx_gpu.c:862-898) + a6xx_set_cp_protect (900-945).   */
/* ------------------------------------------------------------------ */
static const uint32_t a690_protect[] = {
	A6XX_PROTECT_RDONLY(0x00000, 0x004ff),
	A6XX_PROTECT_RDONLY(0x00501, 0x00001),
	A6XX_PROTECT_RDONLY(0x0050b, 0x002f4),
	A6XX_PROTECT_NORDWR(0x0050e, 0x00000),
	A6XX_PROTECT_NORDWR(0x00510, 0x00000),
	A6XX_PROTECT_NORDWR(0x00534, 0x00000),
	A6XX_PROTECT_NORDWR(0x00800, 0x00082),
	A6XX_PROTECT_NORDWR(0x008a0, 0x00008),
	A6XX_PROTECT_NORDWR(0x008ab, 0x00024),
	A6XX_PROTECT_RDONLY(0x008de, 0x000ae),
	A6XX_PROTECT_NORDWR(0x00900, 0x0004d),
	A6XX_PROTECT_NORDWR(0x0098d, 0x00272),
	A6XX_PROTECT_NORDWR(0x00e00, 0x00001),
	A6XX_PROTECT_NORDWR(0x00e03, 0x0000c),
	A6XX_PROTECT_NORDWR(0x03c00, 0x000c3),
	A6XX_PROTECT_RDONLY(0x03cc4, 0x01fff),
	A6XX_PROTECT_NORDWR(0x08630, 0x001cf),
	A6XX_PROTECT_NORDWR(0x08e00, 0x00000),
	A6XX_PROTECT_NORDWR(0x08e08, 0x00007),
	A6XX_PROTECT_NORDWR(0x08e50, 0x0001f),
	A6XX_PROTECT_NORDWR(0x08e80, 0x0027f),
	A6XX_PROTECT_NORDWR(0x09624, 0x001db),
	A6XX_PROTECT_NORDWR(0x09e60, 0x00011),
	A6XX_PROTECT_NORDWR(0x09e78, 0x00187),
	A6XX_PROTECT_NORDWR(0x0a630, 0x001cf),
	A6XX_PROTECT_NORDWR(0x0ae02, 0x00000),
	A6XX_PROTECT_NORDWR(0x0ae50, 0x0012f),
	A6XX_PROTECT_NORDWR(0x0b604, 0x00000),
	A6XX_PROTECT_NORDWR(0x0b608, 0x00006),
	A6XX_PROTECT_NORDWR(0x0be02, 0x00001),
	A6XX_PROTECT_NORDWR(0x0be20, 0x0015f),
	A6XX_PROTECT_NORDWR(0x0d000, 0x005ff),
	A6XX_PROTECT_NORDWR(0x0f000, 0x00bff),
	A6XX_PROTECT_RDONLY(0x0fc00, 0x01fff),
	A6XX_PROTECT_NORDWR(0x11c00, 0x00000),  /* note: infinite range */
};
#define A690_PROTECT_COUNT      (sizeof(a690_protect) / sizeof(a690_protect[0]))
#define A690_PROTECT_COUNT_MAX  48u

static void a6xx_set_cp_protect(void) {
	uint32_t i;

	gpu_write(REG_A6XX_CP_PROTECT_CNTL,
	          A6XX_CP_PROTECT_CNTL_ACCESS_PROT_EN |
	          A6XX_CP_PROTECT_CNTL_ACCESS_FAULT_ON_VIOL_EN |
	          A6XX_CP_PROTECT_CNTL_LAST_SPAN_INF_RANGE);

	for (i = 0; i < A690_PROTECT_COUNT - 1; i++) {
		if (a690_protect[i])
			gpu_write(REG_A6XX_CP_PROTECT(i), a690_protect[i]);
	}
	/* Last span gets the "infinite range" entry at slot count_max-1. */
	gpu_write(REG_A6XX_CP_PROTECT(A690_PROTECT_COUNT_MAX - 1),
	          a690_protect[A690_PROTECT_COUNT - 1]);
}

/* ------------------------------------------------------------------ */
/* a6xx_set_ubwc_config (a6xx_gpu.c:947-1021), A690 branch (995-1000):  */
/*   hbb_lo=2, amsbc=1, rgb565_predicator=1, uavflagprd_inv=2,          */
/*   min_acc_len=0, ubwc_mode=0, hbb_hi=0.                              */
/* ------------------------------------------------------------------ */
static void a6xx_set_ubwc_config(void) {
	uint32_t rgb565_predicator = 1, uavflagprd_inv = 2, min_acc_len = 0;
	uint32_t ubwc_mode = 0, hbb_hi = 0, hbb_lo = 2, amsbc = 1;

	gpu_write(REG_A6XX_RB_NC_MODE_CNTL,
	          rgb565_predicator << 11 | hbb_hi << 10 | amsbc << 4 |
	          min_acc_len << 3 | hbb_lo << 1 | ubwc_mode);
	gpu_write(REG_A6XX_TPL1_NC_MODE_CNTL,
	          hbb_hi << 4 | min_acc_len << 3 | hbb_lo << 1 | ubwc_mode);
	gpu_write(REG_A6XX_SP_NC_MODE_CNTL,
	          hbb_hi << 10 | uavflagprd_inv << 4 | min_acc_len << 3 |
	          hbb_lo << 1 | ubwc_mode);
	gpu_write(REG_A6XX_UCHE_MODE_CNTL, min_acc_len << 23 | hbb_lo << 21);
}

/* ------------------------------------------------------------------ */
/* hw_init (a6xx_gpu.c:1195-1385), A690 = a660_family + a650_family +   */
/* hw_apriv, has_gbif, not a610/a619/a630/a640.                         */
/* ------------------------------------------------------------------ */
static void a6xx_hw_init(adreno_dev_t *d) {
	(void)d;

	/* Clear GBIF halt in case GX was not collapsed (a650 family, has_gbif). */
	klog("adreno_a6xx: hw_init stage 01 begin\n");
	gpu_write(REG_A6XX_GBIF_HALT, 0);
	klog("adreno_a6xx: hw_init stage 02 GBIF_HALT=%08x\n",
	     gpu_read(REG_A6XX_GBIF_HALT));
	gpu_write(REG_A6XX_RBBM_GBIF_HALT, 0);
	klog("adreno_a6xx: hw_init stage 03 RBBM_GBIF_HALT=%08x\n",
	     gpu_read(REG_A6XX_RBBM_GBIF_HALT));

	gpu_write(REG_A6XX_RBBM_SECVID_TSB_CNTL, 0);

	/* Disable the trusted memory range (no secure rendering). */
	gpu_write64(REG_A6XX_RBBM_SECVID_TSB_TRUSTED_BASE, 0x0ull);
	gpu_write(REG_A6XX_RBBM_SECVID_TSB_TRUSTED_SIZE, 0x0);
	klog("adreno_a6xx: hw_init stage 04 SECVID\n");

	/* 64-bit addressing for every block. */
	gpu_write(REG_A6XX_CP_ADDR_MODE_CNTL, 0x1);
	gpu_write(REG_A6XX_VSC_ADDR_MODE_CNTL, 0x1);
	gpu_write(REG_A6XX_GRAS_ADDR_MODE_CNTL, 0x1);
	gpu_write(REG_A6XX_RB_ADDR_MODE_CNTL, 0x1);
	gpu_write(REG_A6XX_PC_ADDR_MODE_CNTL, 0x1);
	gpu_write(REG_A6XX_HLSQ_ADDR_MODE_CNTL, 0x1);
	gpu_write(REG_A6XX_VFD_ADDR_MODE_CNTL, 0x1);
	gpu_write(REG_A6XX_VPC_ADDR_MODE_CNTL, 0x1);
	gpu_write(REG_A6XX_UCHE_ADDR_MODE_CNTL, 0x1);
	gpu_write(REG_A6XX_SP_ADDR_MODE_CNTL, 0x1);
	gpu_write(REG_A6XX_TPL1_ADDR_MODE_CNTL, 0x1);
	gpu_write(REG_A6XX_RBBM_SECVID_TSB_ADDR_MODE_CNTL, 0x1);
	klog("adreno_a6xx: hw_init stage 05 address-mode\n");

	a6xx_set_hwcg(d, 1);
	klog("adreno_a6xx: hw_init stage 06 HWCG\n");

	/* VBIF/GBIF QoS (a650 family). */
	gpu_write(REG_A6XX_GBIF_QSB_SIDE(0), 0x00071620);
	gpu_write(REG_A6XX_GBIF_QSB_SIDE(1), 0x00071620);
	gpu_write(REG_A6XX_GBIF_QSB_SIDE(2), 0x00071620);
	gpu_write(REG_A6XX_GBIF_QSB_SIDE(3), 0x00071620);
	gpu_write(REG_A6XX_RBBM_GBIF_CLIENT_QOS_CNTL, 0x3);
	klog("adreno_a6xx: hw_init stage 07 GBIF-QoS\n");

	gpu_write(REG_A6XX_RBBM_PERFCTR_GPU_BUSY_MASKED, 0xffffffff);

	/* Disable L2 bypass in the UCHE. */
	gpu_write64(REG_A6XX_UCHE_WRITE_RANGE_MAX, 0x0001ffffffffffc0ull);
	gpu_write64(REG_A6XX_UCHE_TRAP_BASE,       0x0001fffffffff000ull);
	gpu_write64(REG_A6XX_UCHE_WRITE_THRU_BASE, 0x0001fffffffff000ull);

	/* a650 family: GMEM VA range is NOT reprogrammed here (a6xx_gpu.c:1276). */

	gpu_write(REG_A6XX_UCHE_FILTER_CNTL, 0x804);
	gpu_write(REG_A6XX_UCHE_CACHE_WAYS, 0x4);
	klog("adreno_a6xx: hw_init stage 08 UCHE\n");

	/* CP ROQ thresholds (a640/a650 family). */
	gpu_write(REG_A6XX_CP_ROQ_THRESHOLDS_2, 0x02000140);
	gpu_write(REG_A6XX_CP_ROQ_THRESHOLDS_1, 0x8040362c);

	/* A660-family LPAC programming FIFO size. */
	gpu_write(REG_A6XX_CP_LPAC_PROG_FIFO_SIZE, 0x00000020);

	gpu_write(REG_A6XX_CP_MEM_POOL_SIZE, 128);

	/* primFifo thresholds + vccCacheSkipDis for a650/a660/a690. */
	gpu_write(REG_A6XX_PC_DBG_ECO_CNTL, 0x00800200);

	gpu_write(REG_A6XX_CP_AHB_CNTL, 0x1);

	gpu_write(REG_A6XX_RBBM_PERFCTR_CNTL, 0x1);
	gpu_write(REG_A6XX_CP_PERFCTR_CP_SEL(0), PERF_CP_ALWAYS_COUNT);

	a6xx_set_ubwc_config();
	klog("adreno_a6xx: hw_init stage 09 CP-UBWC\n");

	/* A690 catalog hang-detect period. */
	gpu_write(REG_A6XX_RBBM_INTERFACE_HANG_INT_CNTL, (1u << 30) | 0x4fffff);

	gpu_write(REG_A6XX_UCHE_CLIENT_PF, (1u << 7) | 1u);

	/* Bicubic filtering weights (a650 family). */
	gpu_write(REG_A6XX_TPL1_BICUBIC_WEIGHTS(0), 0);
	gpu_write(REG_A6XX_TPL1_BICUBIC_WEIGHTS(1), 0x3fe05ff4);
	gpu_write(REG_A6XX_TPL1_BICUBIC_WEIGHTS(2), 0x3fa0ebee);
	gpu_write(REG_A6XX_TPL1_BICUBIC_WEIGHTS(3), 0x3f5193ed);
	gpu_write(REG_A6XX_TPL1_BICUBIC_WEIGHTS(4), 0x3f0243f0);
	klog("adreno_a6xx: hw_init stage 10 fault-prefetch-bicubic\n");

	/* CX GMU busy counter + power counter (GMU aperture). */
	gmu_write(REG_A6XX_GPU_GMU_AO_GPU_CX_BUSY_MASK, 0xff000000);
	gmu_rmw(REG_A6XX_GMU_CX_GMU_POWER_COUNTER_SELECT_0, 0xff, (1u << 5));
	gmu_write(REG_A6XX_GMU_CX_GMU_POWER_COUNTER_ENABLE, 1);
	klog("adreno_a6xx: hw_init stage 11 GMU-counters\n");

	a6xx_set_cp_protect();
	klog("adreno_a6xx: hw_init stage 12 CP-protect\n");

	/* A690 catalog chicken bits. */
	gpu_write(REG_A6XX_CP_CHICKEN_DBG, 0x00028801);
	gpu_write(REG_A6XX_RBBM_GBIF_CLIENT_QOS_CNTL, 0x0);
	gpu_write(REG_A6XX_UCHE_CMDQ_CONFIG, 0x90);

	/* Expanded apriv (hw_apriv). */
	gpu_write(REG_A6XX_CP_APRIV_CNTL,
	          (1u << 6) | (1u << 5) | (1u << 3) | (1u << 2) | (1u << 1));

	gpu_write(REG_A6XX_RBBM_INT_0_MASK, A6XX_INT_MASK);
	klog("adreno_a6xx: hw_init stage 13 complete\n");
}

/* ------------------------------------------------------------------ */
/* SQE microcode load (a6xx_ucode_load, a6xx_gpu.c:1119-1146).         */
/* ------------------------------------------------------------------ */
static int a6xx_sqe_load(adreno_dev_t *d) {
	adreno_sqe_fw_t fw;
	int ret;

	if (d->sqe_bo.va)
		return 0;

	ret = adreno_fw_load_sqe(&fw);
	if (ret != 0) {
		klog("adreno_a6xx: SQE firmware load failed (%d)\n", ret);
		return ret;
	}
	/* The on-disk SQE image carries a 4-byte version header ahead of the
	   microcode.  adreno_fw_create_bo (adreno_gpu.c:538-554) allocates
	   fw->size - 4 and copies from &fw->data[4]; CP_SQE_INSTR_BASE must
	   point at the first real instruction, not at the version word. */
	if (fw.size <= 4) {
		adreno_fw_free_sqe(&fw);
		return -1;
	}

	ret = adreno_bo_alloc(&d->sqe_bo, (uint32_t)fw.size - 4u);
	if (ret != 0) {
		adreno_fw_free_sqe(&fw);
		return ret;
	}
	d->sqe_bo.iova = d->sqe_bo.pa;   /* CB0 identity map */
	memcpy(d->sqe_bo.va, fw.raw + 4, (size_t)fw.size - 4u);
	adreno_wmb();
	adreno_fw_free_sqe(&fw);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Q8B declares a zap shader, so Linux authenticates it before asking the
   CP to leave secure mode. Missing SCM support must not be mistaken for a
   device with no zap node: the direct TRUST_CNTL fallback can fault. */
/* ------------------------------------------------------------------ */
static int gpu_pas_call(uint32_t op, uint64_t arg, uint32_t size) {
	int ret, retry = 0;
	do {
		ret = (int)syscall3(SYS_PLATFORM_CTL, op, arg, size);
		if (ret != Q8B_GPU_PAS_BUSY)
			break;
		usleep(30000); /* Linux: 30 ms, at most 20 retries; outside kernel lock */
	} while (++retry <= 20);
	klog("adreno_a6xx: PAS op=%08x ret=%d\n", op, ret);
	return ret;
}

static int a6xx_exit_secure(adreno_dev_t *d) {
	adreno_zap_fw_t fw;
	adreno_bo_t metadata = {0}, payload = {0};
	int ret, i;
	ret = gpu_pas_call(Q8B_GPU_PAS_QUERY, 0, 0);
	if ((uint32_t)ret != Q8B_GPU_PAS_ABI) {
		klog("adreno_a6xx: compatible Q8B SCM PAS service unavailable\n");
		return -ENOSYS;
	}
	ret = adreno_fw_load_zap(&fw);
	if (ret) return ret;
	ret = adreno_bo_alloc(&metadata, fw.metadata_size);
	if (ret) goto out;
	ret = adreno_bo_alloc(&payload, fw.total_size);
	if (ret) goto out;
	memcpy(metadata.va, fw.metadata, fw.metadata_size);
	for (i = 0; i < fw.num_segs; i++) {
		adreno_zap_seg_t *s = &fw.segs[i];
		memcpy((uint8_t *)payload.va + s->paddr - fw.base_paddr,
		       s->data, s->filesz);
	}
	adreno_wmb();
	ret = adreno_rpmh_scm_vote();
	if (ret) {
		klog("adreno_a6xx: SCM interconnect vote failed\n");
		goto out;
	}
	ret = gpu_pas_call(Q8B_GPU_PAS_INIT, metadata.pa, fw.metadata_size);
	if (ret) goto out;
	ret = gpu_pas_call(Q8B_GPU_PAS_MEM, fw.total_size, 0);
	if (ret) goto out;
	ret = gpu_pas_call(Q8B_GPU_PAS_STAGE, payload.pa, fw.total_size);
	if (ret) goto out;
	ret = gpu_pas_call(Q8B_GPU_PAS_AUTH, 0, 0);
	if (ret) goto out;
	d->zap_done = 1;
	klog("adreno_a6xx: official signed zap authenticated\n");
	ret = adreno_ring_exit_secure(d);
out:
	/* Kernel owns a separate coherent metadata copy until authentication
	 * completes, including when a secure-call error requires a cold boot. */
	adreno_bo_free(&payload);
	adreno_bo_free(&metadata);
	adreno_fw_free_zap(&fw);
	return ret;
}

/* ------------------------------------------------------------------ */
/* Datapath selftest: have the CP DMA a magic pattern through SMMU CB0  */
/* into the ring scratch buffer, then read it back from the CPU.  This */
/* exercises ring emit -> WPTR doorbell -> CP IB fetch -> CP_MEM_WRITE  */
/* -> SMMU translate -> fence retire, i.e. the whole submission path,   */
/* without needing the 3D pipeline (that is M2/M3).                     */
/* ------------------------------------------------------------------ */
#define SELFTEST_MAGIC0  0xdeadbeefu
#define SELFTEST_MAGIC1  0x12345678u
#define SELFTEST_MAGIC2  0xcafebabeu
#define SELFTEST_MAGIC3  0x0badf00du
#define SELFTEST_OFF     64u   /* bytes into scratch (fence lives at 0) */

static int a6xx_selftest(adreno_dev_t *d) {
	uint32_t cmds[7];
	uint64_t dst = d->ring.scratch.iova + SELFTEST_OFF;
	volatile uint32_t *mem = (volatile uint32_t*)
		((uint8_t*)d->ring.scratch.va + SELFTEST_OFF);
	int ret;

	cmds[0] = ADRENO_PKT7(CP_MEM_WRITE, 6);
	cmds[1] = (uint32_t)(dst & 0xffffffffu);
	cmds[2] = (uint32_t)(dst >> 32);
	cmds[3] = SELFTEST_MAGIC0;
	cmds[4] = SELFTEST_MAGIC1;
	cmds[5] = SELFTEST_MAGIC2;
	cmds[6] = SELFTEST_MAGIC3;

	ret = adreno_ring_submit(d, cmds, 7);
	if (ret != 0) {
		klog("adreno_a6xx: selftest submit failed (%d)\n", ret);
		return ret;
	}
	if (mem[0] != SELFTEST_MAGIC0 || mem[1] != SELFTEST_MAGIC1 ||
	    mem[2] != SELFTEST_MAGIC2 || mem[3] != SELFTEST_MAGIC3) {
		klog("adreno_a6xx: selftest readback mismatch %08x %08x %08x %08x\n",
		     mem[0], mem[1], mem[2], mem[3]);
		return -1;
	}
	klog("adreno_a6xx: CP datapath selftest OK\n");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Top-level bring-up.                                                  */
/* ------------------------------------------------------------------ */
static adreno_dev_t g_dev;

adreno_dev_t* adreno_dev(void) {
	return &g_dev;
}

/* CP_RB_CNTL default (see adreno_ring.c): BUFSZ(12) | BLKSZ(2) = 0x20c.
   A690 is hw_apriv so NO_UPDATE stays clear. */
#define A6XX_CP_RB_CNTL_DEFAULT  0x0000020cu

int adreno_a6xx_init(adreno_dev_t *d) {
	int ret;

	if (d->init_tried)
		return d->init_rc;
	d->init_tried = 1;

	ret = adreno_reg_map();
	if (ret) { klog("adreno_a6xx: reg map failed (%d)\n", ret); d->init_rc = ret; return ret; }

	/* Power the GPUCC parent RPMh domains before touching its registers. */
	ret = adreno_rpmh_gfx_vote();
	if (ret) {
		klog("adreno_a6xx: RPMh GPU power vote failed (%d)\n", ret);
		d->init_rc = ret;
		return ret;
	}

	ret = adreno_gpucc_init();
	if (ret) { klog("adreno_a6xx: gpucc init failed (%d)\n", ret); d->init_rc = ret; return ret; }

	/* Linux allocates the hw_apriv RPTR shadow before attaching GPU BOs to
	   the IOMMU domain.  Do the same here so CB0 can be built once, with the
	   privileged page already present, before translation is enabled. */
	ret = adreno_bo_alloc(&d->rptr_shadow, 4096u);
	if (ret) {
		klog("adreno_a6xx: rptr shadow alloc failed (%d)\n", ret);
		d->init_rc = ret;
		return ret;
	}
	d->rptr_shadow.iova = d->rptr_shadow.pa;
	ret = adreno_ring_init(d);
	if (ret) {
		klog("adreno_a6xx: ring allocation failed (%d)\n", ret);
		d->init_rc = ret;
		return ret;
	}

	ret = adreno_smmu_init(d);
	if (ret) { klog("adreno_a6xx: smmu CB0 init failed (%d)\n", ret); d->init_rc = ret; return ret; }
	d->smmu_ready = 1;

	ret = adreno_gmu_init(d);
	if (ret) { klog("adreno_a6xx: gmu init failed (%d)\n", ret); d->init_rc = ret; return ret; }

	ret = a6xx_sqe_load(d);
	if (ret) { klog("adreno_a6xx: sqe load failed (%d)\n", ret); d->init_rc = ret; return ret; }

	/* Linux hw_init() begins by asking the non-legacy GMU to keep GX on for
	   all host register accesses, and clears the OOB request on exit. */
	ret = adreno_gmu_gpu_set();
	if (ret) {
		klog("adreno_a6xx: GPU_SET OOB failed (%d)\n", ret);
		d->init_rc = ret;
		return ret;
	}

	a6xx_hw_init(d);

	/* Program the CP engine onto the ring (a6xx_gpu.c:1390-1417). */
	gpu_write64(REG_A6XX_CP_SQE_INSTR_BASE, d->sqe_bo.iova);
	gpu_write64(REG_A6XX_CP_RB_BASE, d->ring.bo.iova);
	gpu_write(REG_A6XX_CP_RB_CNTL, A6XX_CP_RB_CNTL_DEFAULT);
	gpu_write64(REG_A6XX_CP_RB_RPTR_ADDR, d->rptr_shadow.iova);
	((volatile uint32_t *)d->rptr_shadow.va)[0] = 0;
	adreno_wmb();
	gpu_write(REG_A6XX_CP_SQE_CNTL, 1);

	ret = adreno_ring_cp_init(d);
	if (ret) {
		klog("adreno_a6xx: cp init failed (%d)\n", ret);
		adreno_gmu_gpu_clear();
		d->init_rc = ret;
		return ret;
	}

	ret = a6xx_exit_secure(d);
	if (ret) {
		klog("adreno_a6xx: secure-mode exit failed (%d)\n", ret);
		d->cp_ready = 0;
		adreno_gmu_gpu_clear();
		d->init_rc = ret;
		return ret;
	}

	d->gpu_ready = 1;

	ret = a6xx_selftest(d);
	if (ret) {
		/* The datapath did not come up; decline so callers fall back. */
		klog("adreno_a6xx: selftest failed (%d) - declining GPU path\n", ret);
		d->gpu_ready = 0;
		adreno_gmu_gpu_clear();
		d->init_rc = ret;
		return ret;
	}

	adreno_gmu_gpu_clear();
	d->init_rc = 0;
	klog("adreno_a6xx: Adreno 690 bring-up complete\n");
	return 0;
}

int adreno_2d_init(void) {
	return adreno_a6xx_init(adreno_dev());
}

int adreno_3d_init(void) {
	return adreno_a6xx_init(adreno_dev());
}
