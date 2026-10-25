/*
 * bsp_cl.c - raspberry-pi5 OpenCL compute public API: the V3D 7.1 CSD compute
 * offload, the sibling of bsp_g3d.c.  Every operation is delegated to the
 * VideoCore compute back end in libvideocore (<videocore/vc_cl.h>), which owns
 * the CSD submission and works zero-copy on the caller's contig-shm / dma
 * buffers through their resolved physical bases.
 *
 * The compute path is brought up lazily on the first op (idempotent, on top of
 * the CSD path's V3D init that bsp_g2d_init already ran) and latched: once
 * vc_cl_init() declines, we never retry, so a libOpenCL GPU device sees one
 * sticky G2D_ERR_NOT_SUPPORTED and keeps the kernel on its always-present CPU
 * device.  These flags are only touched from g2dd's serialized dev_cntl path
 * for a given requester; the vc_cl / v3d_3d layer owns the real GPU-dispatch
 * lock.  A submitted dispatch is never replayed.
 */

#include <string.h>

#include <bsp/bsp_cl.h>
#include <g2dclient/g2dclient.h>

#include <videocore/vc_cl.h>
#include <ewoksys/klog.h>

static int _cl_ready;
static int _cl_declined;

static int cl_ensure(void)
{
    if (_cl_ready)
        return 1;
    if (_cl_declined)
        return 0;
    if (vc_cl_init() == VC_CL_DONE) {
        _cl_ready = 1;
        return 1;
    }
    _cl_declined = 1;
    return 0;
}

/* Map the vc_cl tri-state onto the g2d result codes g2dd passes back verbatim:
 * DONE -> G2D_OK, FAILED -> G2D_ERR_FAILED (transient, replay on the CPU
 * device), TIMEOUT / MMU_FAULT -> their terminal g2d codes (the launch was
 * submitted, so it is never replayed), and UNSUPPORTED -> G2D_ERR_NOT_SUPPORTED
 * (sticky, libOpenCL latches the GPU device off).  Mirrors bsp_g3d's pgl3d_rc. */
static int32_t cl_rc(int r)
{
    switch (r) {
    case VC_CL_DONE:
        return G2D_OK;
    case VC_CL_FAILED:
        return G2D_ERR_FAILED;
    case VC_CL_TIMEOUT:
        return G2D_ERR_TIMEOUT;
    case VC_CL_MMU_FAULT:
        return G2D_ERR_MMU_FAULT;
    default: /* VC_CL_UNSUPPORTED */
        return G2D_ERR_NOT_SUPPORTED;
    }
}

int32_t bsp_cl_init(void)
{
    return cl_ensure() ? 0 : -1;
}

/* Upload a compiled compute program.  `blob` is the CPU-side serialized
 * g2d_pgl_program_blob_t (compute stage only) libOpenCL packed; `size` is the
 * attached canvas byte count.  vc_cl_program validates the header + payload and
 * deep-copies the cs QPU into GPU dma, returning the non-negative program handle
 * or a negative code.  VC_CL_PROG_FAILED / VC_CL_PROG_UNSUPPORTED are value-
 * identical to G2D_ERR_FAILED / G2D_ERR_NOT_SUPPORTED, so the result passes
 * through verbatim (a handle >= 0 rides the g2dd reply's second int). */
int32_t bsp_cl_program(const void* blob, uint32_t size)
{
    if (!cl_ensure())
        return G2D_ERR_NOT_SUPPORTED;
    return vc_cl_program(blob, size);
}

/* Run one NDRange launch.  g2dd has already resolved every __global buffer
 * argument canvas to a contig physical base (rejecting a non-contig arg with the
 * transient G2D_ERR_FAILED before it reaches here) and attached the packed
 * scalar-arg uniform canvas for CPU read, so this only forwards the resolved
 * bsp_cl_dispatch_t to vc_cl_dispatch and maps the tri-state back. */
int32_t bsp_cl_dispatch(int32_t handle, const bsp_cl_dispatch_t* d)
{
    if (!cl_ensure())
        return G2D_ERR_NOT_SUPPORTED;
    if (d == NULL)
        return G2D_ERR_FAILED;
    if (d->num_args > BSP_CL_MAX_ARGS)
        return G2D_ERR_FAILED;
    if (d->wg_size == 0 || d->wg_size > 256)
        return G2D_ERR_FAILED;
    return cl_rc(vc_cl_dispatch(handle, d->wg_count, d->wg_size,
                                d->arg_phy, d->arg_size, d->num_args,
                                (const uint32_t*)d->unif, d->num_uniforms));
}

/* Release a program handle returned by bsp_cl_program (its cache slot and GPU
 * dma go back for reuse).  g2dd calls this for clients that have exited.
 * vc_cl_program_free is probe-gated, so an unknown / already-free handle or a
 * compute path that never came up answers VC_CL_PROG_UNSUPPORTED (-2). */
int32_t bsp_cl_program_free(int32_t handle)
{
    return vc_cl_program_free(handle);
}
