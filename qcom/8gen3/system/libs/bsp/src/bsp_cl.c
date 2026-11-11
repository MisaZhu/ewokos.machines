/*
 * bsp_cl.c - Q8B (SC8280XP / Adreno 690) OpenCL compute public API: the a6xx
 * CS compute offload, the sibling of bsp_g3d.c.  Every operation is delegated
 * to libadreno (<adreno/adreno_drv.h>): the program blob the app-side
 * adrenocompiler serialized (adreno_prog_blob_t, pure compute kern[0]) loads
 * through adreno_3d_program_load - the SAME table the pgl programs use - and
 * a launch dispatches through adreno_3d_cl_dispatch, which binds the
 * __global buffer arguments as UAV descriptors at their resolved contig
 * physical bases (CB0 is identity-mapped, so pa == iova: zero copy) and
 * submits one CP_EXEC_CS on the shared ring.
 *
 * The compute path is gated exactly like bsp_g3d (ADRENO_ENABLE opt-in +
 * adreno_3d_init bring-up, idempotent and latched): once the probe declines
 * we never retry, so a libOpenCL / libvulkan GPU device sees one sticky
 * G2D_ERR_NOT_SUPPORTED and keeps its kernels on the always-present CPU
 * device.  A submitted dispatch is never replayed (adreno_ring_submit's
 * fail-fast wedge latch owns that policy).
 *
 * Signatures MUST stay byte-identical to the other machines: g2dd.c is
 * shared code and a prototype divergence surfaces only as a link error.
 */

#include <string.h>

#include <bsp/bsp_cl.h>
#include <g2dclient/g2dclient.h>   /* G2D_ERR_* */
#include <adreno/adreno_drv.h>
#include <adreno/adreno_prog.h>    /* ADRENO_PROG_MAGIC, adreno_prog_blob_t */
#include <ewoksys/klog.h>

/* capability latch: 0 = not probed, 1 = datapath ready, -1 = declined */
static int g_cl_ready = 0;

/* Probe the Adreno 690 compute path once (idempotent + latched), the same
   gate bsp_g3d.c uses: ADRENO_ENABLE opt-in, then the shared a6xx bring-up
   (adreno_3d_init is what bsp_g3d's probe already runs; whichever comes
   first pays for it).  A wedged device demotes the latch for the session. */
static int cl_ensure(void) {
	if (g_cl_ready == 1 && adreno_wedged()) {
		g_cl_ready = -1;
		klog("q8b cl: Adreno wedged - CL offload disabled for this session\n");
	}
	if (g_cl_ready != 0)
		return g_cl_ready == 1;
	if (!adreno_flag("ADRENO_ENABLE")) {
		g_cl_ready = -1;
		return 0;
	}
	g_cl_ready = (adreno_3d_init() == 0) ? 1 : -1;
	klog("q8b cl: adreno compute bring-up %s\n",
	     (g_cl_ready == 1) ? "OK" : "DECLINED");
	return g_cl_ready == 1;
}

/* libadreno result -> g2d code, the same mapping bsp_g3d.c's a3d_result
 * uses and mirroring raspi5's bsp_cl: DECLINED (nothing submitted) is the
 * replayable -1 so libOpenCL replays the launch on its CPU device; TIMEOUT
 * (submitted, fence never retired) and WEDGED are terminal -3, FAULT
 * (submitted, RBBM fault latched) is the terminal -4 MMU-fault code.  The
 * terminal codes keep the failed job's staging buffers retained before the
 * client's CPU replay on its own host buffers (never a double GPU run). */
static int32_t cl_result(int rc) {
	if (rc == ADRENO_OK)
		return G2D_OK;
	if (rc == ADRENO_E_DECLINED)
		return G2D_ERR_FAILED;
	if (rc == ADRENO_E_FAULT)
		return G2D_ERR_MMU_FAULT;
	return G2D_ERR_TIMEOUT;   /* ADRENO_E_TIMEOUT / ADRENO_E_WEDGED */
}

int32_t bsp_cl_init(void) {
	return cl_ensure() ? 0 : -1;
}

/* Upload a compiled compute program.  `blob` is the CPU-side serialized
 * adreno_prog_blob_t (pure compute kern[0]) the app-side adrenocompiler
 * packed via pglc_program_serialize; `size` is the attached canvas byte
 * count.  A non-Adreno blob (e.g. a V3D-QPU program that reached the wrong
 * machine's daemon) is a capability mismatch, not a transient staging
 * failure, so it answers the sticky -2 and libOpenCL keeps the kernel on
 * its CPU device.  The handle (>= 0) or the negative code rides the g2dd
 * reply verbatim. */
int32_t bsp_cl_program(const void* blob, uint32_t size) {
	const adreno_prog_blob_t* h;
	int rc;

	if (!cl_ensure())
		return G2D_ERR_NOT_SUPPORTED;
	if (blob == NULL || size < sizeof(adreno_prog_blob_t))
		return G2D_ERR_FAILED;
	h = (const adreno_prog_blob_t*)blob;
	if (h->magic != ADRENO_PROG_MAGIC)
		return G2D_ERR_NOT_SUPPORTED;   /* not an Adreno program blob */

	rc = adreno_3d_program_load(blob, size);
	if (rc >= 0)
		return (int32_t)rc;
	if (rc == ADRENO_E_WEDGED)
		return G2D_ERR_TIMEOUT;
	return G2D_ERR_FAILED;
}

/* Run one NDRange launch.  g2dd has already resolved every __global buffer
 * argument canvas to a contig physical base (rejecting a non-contig arg with
 * the transient G2D_ERR_FAILED before it reaches here) and attached the
 * packed scalar-arg uniform canvas for CPU read, so this only forwards the
 * resolved bsp_cl_dispatch_t to adreno_3d_cl_dispatch and maps the result
 * back.  arg_phy[] is ewokos_addr_t (u64 on this aarch64 platform); copy it
 * into the u64 GPU-address table libadreno takes. */
int32_t bsp_cl_dispatch(int32_t handle, const bsp_cl_dispatch_t* d) {
	uint64_t phy[BSP_CL_MAX_ARGS];
	uint32_t i;

	if (!cl_ensure())
		return G2D_ERR_NOT_SUPPORTED;
	if (d == NULL)
		return G2D_ERR_FAILED;
	if (d->num_args > BSP_CL_MAX_ARGS)
		return G2D_ERR_FAILED;
	if (d->wg_size == 0 || d->wg_size > 256)
		return G2D_ERR_FAILED;
	if (d->wg_count[0] == 0 || d->wg_count[1] == 0 || d->wg_count[2] == 0)
		return G2D_ERR_FAILED;

	memset(phy, 0, sizeof(phy));
	for (i = 0; i < d->num_args; i++)
		phy[i] = (uint64_t)d->arg_phy[i];

	return cl_result(adreno_3d_cl_dispatch(handle, d->wg_count, d->wg_size,
	                                       phy, d->arg_size, d->num_args,
	                                       (const uint32_t*)d->unif,
	                                       d->num_uniforms));
}

/* Release a program handle returned by bsp_cl_program (its table slot and
 * GPU BOs go back for reuse).  g2dd calls this for clients that have
 * exited.  Unknown / already-free handles answer the sticky -2, matching
 * the canonical header's contract. */
int32_t bsp_cl_program_free(int32_t handle) {
	int rc;

	if (!cl_ensure())
		return G2D_ERR_NOT_SUPPORTED;
	rc = adreno_3d_program_free(handle);
	if (rc == 0)
		return G2D_OK;
	if (rc == ADRENO_E_WEDGED)
		return G2D_ERR_TIMEOUT;
	return G2D_ERR_NOT_SUPPORTED;
}
