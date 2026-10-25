/*
 * vc_cl.h - VideoCore V3D 7.1 COMPUTE (CSD) back end for OpenCL on the raspi5
 * (Raspberry Pi 5 / BCM2712) bsp_cl layer.
 *
 * This is the public contract between bsp_cl.c (the /dev/g2d daemon side,
 * argument validation and physical-base resolution) and v3d_3d.c (the CSD
 * compute submission mechanics, kept private to libvideocore).  It is the
 * compute sibling of vc_pgl3d.h: where vc_pgl3d drives the 3D bin/render
 * pipeline for portablegl, vc_cl drives the SAME V3D CSD compute path the
 * shadertoy "compute route" already proved, for libOpenCL's GPU device.
 *
 * A kernel reaches the GPU as a mesa-compiled compute blob in the exact
 * g2d_pgl_program_blob_t wire format portablegl uses, with ONLY the compute
 * (cs) stage populated.  vc_cl_program parses that blob, deep-copies the cs
 * QPU + uniform layout into GPU dma through the private v3d_3d program cache
 * and returns a handle; vc_cl_dispatch resolves each __global buffer argument
 * to a GPU IOVA (zero copy, the same gate the 2D and 3D paths use), binds them
 * as SSBOs and launches the grid through v3d_g2d_run_csd.
 *
 * ZERO COPY: like every other V3D route here, the GPU operates directly on the
 * caller's contig-shm / dma buffers through their resolved physical bases
 * (IOVA = low 32 bits of phys); nothing is copied but the small uniform block.
 *
 * Every entry point returns a DONE/FAILED/UNSUPPORTED tri-state (plus the
 * TIMEOUT/MMU_FAULT refinements of a submitted-but-failed launch).  A submitted
 * dispatch is NEVER replayed, so FAILED and UNSUPPORTED both mean "the caller
 * must run this kernel on its CPU device instead".  Neither result disables
 * later GPU launches; unsupported shader constructs remain a per-kernel fallback.
 *
 * HARDWARE STATUS: the V3D 7.1 compute bring-up is shared with the proven
 * shadertoy CSD route, but the CL front end (GLSL-ES-3.10 compute -> mesa ->
 * this layer) cannot be exercised under QEMU (no V3D model) and requires
 * validation on a physical Pi 5.  Kernels needing shared memory / barriers
 * beyond CSD support are declined (UNSUPPORTED) rather than run incorrectly.
 */

#ifndef VIDEOCORE_VC_CL_H
#define VIDEOCORE_VC_CL_H

#include <stdint.h>
#include <stddef.h>
#include <ewoksys/ewokdef.h>

/* Tri-state result, value-identical to the V3D_3D_* set and to vc_pgl3d's, so
 * bsp_cl maps it onto the g2d result codes with the same table.  DONE = the
 * launch completed; FAILED / UNSUPPORTED = run it on the CPU device instead;
 * TIMEOUT / MMU_FAULT refine a launch that WAS submitted but overran its wait
 * budget / faulted the GPU MMU. */
#define VC_CL_DONE        0
#define VC_CL_FAILED      1
#define VC_CL_UNSUPPORTED 2
#define VC_CL_TIMEOUT     3
#define VC_CL_MMU_FAULT   4

/* Program-upload result codes.  A non-negative return is the program handle
 * forwarded to the client; the two negatives are value-identical to the
 * G2D_ERR_FAILED / G2D_ERR_NOT_SUPPORTED codes so bsp_cl forwards them
 * verbatim (mirrors VC_PGL3D_PROG_*). */
#define VC_CL_PROG_FAILED      (-1)
#define VC_CL_PROG_UNSUPPORTED (-2)

/* Maximum __global buffer SSBO arguments one dispatch binds.  Mirrors
 * BSP_CL_MAX_ARGS / G2D_CL_MAX_ARGS across the layers. */
#define VC_CL_MAX_ARGS 16u

/* Bring up the compute path on top of the CSD path's V3D init (idempotent):
 * ensures v3d_g2d is ready, then brings up the v3d_3d dma staging the program
 * cache and uniform expansion need.  Returns VC_CL_DONE on success,
 * VC_CL_UNSUPPORTED when the GPU cannot be used for compute (the caller then
 * keeps libOpenCL's CPU device).  Shares the underlying v3d_g2d / v3d_3d
 * bring-up with vc_pgl3d_init; both are idempotent and may be called in any
 * order. */
int vc_cl_init(void);

/* Non-zero once vc_cl_init() succeeded and the compute path is usable. */
int vc_cl_probe(void);

/* Upload a compiled compute program.  `blob` points to a serialized
 * g2d_pgl_program_blob_t header immediately followed by its payload sections,
 * `size` bytes total (a CPU-side buffer - no GPU visibility needed; the cs QPU
 * + uniform layout are deep-copied into GPU dma).  Only the compute (cs) stage
 * is consulted; a blob whose cs_qpu_words == 0 (no compute stage) is declined.
 * Returns the non-negative program handle (referenced by a later
 * vc_cl_dispatch), VC_CL_PROG_UNSUPPORTED when the compute path is not ready /
 * the blob is malformed or out of range, or VC_CL_PROG_FAILED on a dma
 * allocation failure. */
int32_t vc_cl_program(const void *blob, uint32_t size);

/* Run one NDRange launch of the program `handle`.  wg_count[3] is the grid in
 * workgroups and wg_size the local_size product (1..256); both come from the
 * client's clEnqueueNDRangeKernel.  arg_phy[i] / arg_size[i] (i < num_args)
 * are the resolved contig physical bases and byte lengths of the __global
 * buffer arguments, in SSBO binding order (a 0 phy marks an unused trailing
 * slot); each is re-validated into a GPU IOVA here.  `unif` points to
 * num_uniforms u32 packed scalar / __constant argument values the kernel's
 * QUNIFORM_UNIFORM / UBO_ADDR slots read (NULL when num_uniforms == 0).
 * Returns a tri-state; the caller runs the kernel on its CPU device on
 * anything but VC_CL_DONE. */
int vc_cl_dispatch(int32_t handle,
                   const uint32_t wg_count[3], uint32_t wg_size,
                   const ewokos_addr_t *arg_phy, const uint32_t *arg_size,
                   uint32_t num_args,
                   const uint32_t *unif, uint32_t num_uniforms);

/* Drop a program uploaded by vc_cl_program (its cache slot + GPU dma are
 * released for reuse).  g2dd calls this for every program whose owning client
 * has exited.  Returns 0, or VC_CL_PROG_UNSUPPORTED for an unknown / already-
 * free handle or when the compute path never came up. */
int vc_cl_program_free(int32_t handle);

#endif /* VIDEOCORE_VC_CL_H */
