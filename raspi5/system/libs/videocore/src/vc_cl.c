/*
 * vc_cl.c - VideoCore V3D 7.1 COMPUTE (CSD) back end for OpenCL on the raspi5
 * bsp_cl layer.  See vc_cl.h for the contract.
 *
 * This is a thin public layer over the private v3d_3d.c compute engine, the
 * exact sibling of vc_pgl3d.c: it brings the CSD compute path up on top of the
 * 2D engine's V3D init, parses the mesa-compiled compute blob libOpenCL ships
 * (the g2d_pgl_program_blob_t wire format, compute stage only), caches the cs
 * QPU + uniform layout in GPU dma through v3d_3d_program, and launches a grid
 * through v3d_3d_cl_dispatch (which reuses draw_compute's proven QUNIFORM
 * interpreter, uniform staging and v3d_g2d_run_csd submission).  All V3D
 * register access, cache maintenance and serialization stay in v3d_3d.c /
 * v3d_g2d.c.  A submitted dispatch is never replayed.
 */

#include <string.h>
#include <ewoksys/klog.h>
#include <videocore/vc_g2d.h>
#include <videocore/vc_cl.h>
#include <g2dclient/g2dclient.h>   /* g2d_pgl_program_blob_t wire format */
#include "v3d_g2d.h"
#include "v3d_3d.h"

static int _cl_ok;

int vc_cl_init(void)
{
    if (_cl_ok)
        return VC_CL_DONE;

    /* The compute path reuses the CSD path's register window, V3D MMU, SMS
     * power-up, L2C enable and clock; v3d_g2d_init() is idempotent (g2dd
     * already ran it for the 2D ops) and never re-issues a runtime power-domain
     * write.  v3d_3d_init() then allocates the dma staging the program cache
     * and the expanded-uniform / job-uniform buffers need; it is shared with
     * vc_pgl3d_init() and equally idempotent, so whichever runs first brings
     * the 3D/compute engine up and the other is a no-op. */
    if (v3d_g2d_init() != 0 || !v3d_g2d_ready())
        return VC_CL_UNSUPPORTED;
    if (v3d_3d_init() != V3D_3D_DONE)
        return VC_CL_UNSUPPORTED;

    _cl_ok = 1;
    return VC_CL_DONE;
}

int vc_cl_probe(void)
{
    return _cl_ok && v3d_3d_ready();
}

/* Upload a compiled compute program.  `blob` is the CPU-side serialized
 * g2d_pgl_program_blob_t (fixed header + payload sections in wire order) that
 * libOpenCL's GPU front end packed with pglc_program_serialize; `size` is the
 * attached canvas byte count.  Only the compute (cs) stage is consulted - a CL
 * blob deliberately leaves the vs/fs/coord word counts zero - so this validates
 * the header + payload bounds exactly as bsp_g3d_program does, walks the payload
 * to the cs sections, points a v3d_3d_prog_desc_t at them IN PLACE (zero copy:
 * v3d_3d_program deep-copies the cs QPU + uniform layout into GPU dma) and
 * forwards.  Returns the non-negative program handle, or a negative code
 * value-identical to G2D_ERR_FAILED / G2D_ERR_NOT_SUPPORTED so bsp_cl passes it
 * through verbatim. */
int32_t vc_cl_program(const void *blob, uint32_t size)
{
    const g2d_pgl_program_blob_t *h;
    const uint8_t *base = (const uint8_t *)blob;
    v3d_3d_prog_desc_t d;
    uint32_t off;
    uint64_t expect;

    if (!vc_cl_probe())
        return VC_CL_PROG_UNSUPPORTED;
    if (blob == NULL || size < sizeof(g2d_pgl_program_blob_t)) {
        slog("vc_cl: program blob too small %u\r\n", size);
        return VC_CL_PROG_FAILED;
    }

    h = (const g2d_pgl_program_blob_t *)blob;
    if (h->magic != G2D_PGL_PROGRAM_MAGIC ||
        h->version != G2D_PGL_PROGRAM_VERSION) {
        slog("vc_cl: program magic 0x%x version %u (want %u)\r\n",
             h->magic, h->version, (uint32_t)G2D_PGL_PROGRAM_VERSION);
        return VC_CL_PROG_FAILED;
    }
    /* a CL program MUST carry a compute stage; a blob without one is a raster
     * program that reached the wrong entry point. */
    if (h->cs_qpu_words == 0) {
        slog("vc_cl: program has no compute stage\r\n");
        return VC_CL_PROG_UNSUPPORTED;
    }

    /* the declared payload must match the section counts (accumulated in 64
     * bits so a corrupt count cannot wrap past the check) and fit inside the
     * attached canvas; only then are the section pointers below in bounds. */
    expect = ((uint64_t)h->vs_qpu_words + (uint64_t)h->fs_qpu_words +
              (uint64_t)h->coord_qpu_words +
              (uint64_t)h->vs_num_uniforms * 2u +
              (uint64_t)h->fs_num_uniforms * 2u +
              (uint64_t)h->coord_num_uniforms * 2u +
              (uint64_t)h->cs_qpu_words +
              (uint64_t)h->cs_num_uniforms * 2u +
              (uint64_t)h->cs2_qpu_words +
              (uint64_t)h->cs2_num_uniforms * 2u) * 4u;
    if (expect != (uint64_t)h->payload_bytes) {
        slog("vc_cl: program payload %u != expect %u\r\n",
             h->payload_bytes, (uint32_t)expect);
        return VC_CL_PROG_FAILED;
    }
    if ((uint64_t)h->payload_bytes > (uint64_t)size - sizeof(*h)) {
        slog("vc_cl: program payload %u > canvas %u\r\n", h->payload_bytes, size);
        return VC_CL_PROG_FAILED;
    }

    /* walk the payload sections in their fixed wire order to reach the cs
     * stage.  A CL blob zeroes every raster section, so these advances are all
     * zero, but walking the full order keeps the parser identical to bsp_g3d's
     * and robust to a blob that carries more. */
    off = (uint32_t)sizeof(*h);
    off += h->vs_qpu_words * 4u;
    off += h->fs_qpu_words * 4u;
    off += h->coord_qpu_words * 4u;
    off += h->vs_num_uniforms * 4u;     /* vs unif contents */
    off += h->vs_num_uniforms * 4u;     /* vs unif data     */
    off += h->fs_num_uniforms * 4u;     /* fs unif contents */
    off += h->fs_num_uniforms * 4u;     /* fs unif data     */
    off += h->coord_num_uniforms * 4u;  /* coord unif contents */
    off += h->coord_num_uniforms * 4u;  /* coord unif data     */

    memset(&d, 0, sizeof(d));
    d.cs_qpu           = (const uint32_t *)(base + off);
    off += h->cs_qpu_words * 4u;
    d.cs_unif_contents = (const uint32_t *)(base + off);
    off += h->cs_num_uniforms * 4u;
    d.cs_unif_data     = (const uint32_t *)(base + off);

    d.cs_qpu_words     = h->cs_qpu_words;
    d.cs_num_uniforms  = h->cs_num_uniforms;
    d.cs_threads       = h->cs_threads;
    d.cs_single_seg    = h->cs_single_seg;
    d.cs_local_size[0] = h->cs_local_size[0];
    d.cs_local_size[1] = h->cs_local_size[1];
    d.cs_local_size[2] = h->cs_local_size[2];
    d.cs_shared_size   = h->cs_shared_size;
    d.cs_spill_size    = h->cs_spill_size;
    /* cs_is_geom / cs_geom_tpw stay 0: those select the fixed two-pass geometry
     * rasterizer kernels, never a CL kernel.  vs/fs/coord stay 0 so v3d_3d
     * takes the pure-compute (cs_only) path. */

    /* V3D_3D_PROG_* and VC_CL_PROG_* are value-identical; the handle (>= 0) or
     * the negative code passes straight through. */
    return (int32_t)v3d_3d_program(&d);
}

int vc_cl_dispatch(int32_t handle,
                   const uint32_t wg_count[3], uint32_t wg_size,
                   const ewokos_addr_t *arg_phy, const uint32_t *arg_size,
                   uint32_t num_args,
                   const uint32_t *unif, uint32_t num_uniforms)
{
    uint32_t ssbo_iova[VC_CL_MAX_ARGS];
    uint32_t hull_lo = 0, hull_hi = 0;
    uint32_t i;

    if (!vc_cl_probe())
        return VC_CL_UNSUPPORTED;
    if (num_args > VC_CL_MAX_ARGS)
        return VC_CL_UNSUPPORTED;

    /* Resolve each __global buffer argument's contig physical base into a GPU
     * IOVA (the same gate the 2D and 3D paths use) and track the address hull
     * so the dispatch's POST L2 writeback covers every buffer the kernel may
     * write.  A base that will not resolve is a per-launch staging problem, not
     * a platform capability gap, so it answers the transient VC_CL_FAILED (the
     * client replays this launch on its CPU device) rather than the sticky
     * VC_CL_UNSUPPORTED.  An unused trailing slot (phy 0) binds IOVA 0, which
     * expand_uniforms declines only if the kernel actually references it. */
    memset(ssbo_iova, 0, sizeof(ssbo_iova));
    for (i = 0; i < num_args; i++) {
        uint32_t iova, span;
        if (arg_phy == NULL || arg_phy[i] == 0) {
            ssbo_iova[i] = 0;
            continue;
        }
        span = (arg_size != NULL) ? arg_size[i] : 0u;
        iova = gpu_phys(arg_phy[i], (size_t)span, 1u);
        if (iova == 0) {
            slog("vc_cl: arg %u phys %llx span %u not GPU-addressable\r\n",
                 i, (unsigned long long)arg_phy[i], span);
            return VC_CL_FAILED;
        }
        ssbo_iova[i] = iova;
        if (hull_lo == 0 || iova < hull_lo)
            hull_lo = iova;
        if (iova + span > hull_hi)
            hull_hi = iova + span;
    }

    /* V3D_3D_* and VC_CL_* tri-states are value-identical. */
    return v3d_3d_cl_dispatch(handle, wg_count, wg_size,
                              ssbo_iova, num_args, unif, num_uniforms,
                              hull_lo, hull_lo ? (hull_hi - hull_lo) : 0u);
}

/* Release a cached program.  Probe-gated like the upload so a machine whose
 * compute path never came up answers UNSUPPORTED instead of poking the table.
 * V3D_3D_PROG_* and VC_CL_PROG_* are value-identical. */
int vc_cl_program_free(int32_t handle)
{
    if (!vc_cl_probe())
        return VC_CL_PROG_UNSUPPORTED;
    return v3d_3d_program_free(handle);
}
