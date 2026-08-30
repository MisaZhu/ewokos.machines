/*
 * v3d_g2d.h - VideoCore VII (V3D) hardware back end for the EwokOS
 * raspberry-pi5 bsp_g2d layer.
 *
 * Bring-up maps the V3D register block (0x1002000000, outside the main
 * MMIO window, whitelisted by the raspi5 kernel check_mem_map_arch) and
 * the PM power domain into the caller's address space through
 * SYS_MEM_MAP, then power-cycles the GRAFX_V3D domain and enables the
 * L2 cache.  CSD code/uniform staging lives in physically-contiguous
 * sys_dma buffers (dma_alloc), so the QPU fetches them by physical
 * address without a V3D MMU page table.
 *
 * Canvases arrive as virtual addresses (shmat() / dma window addresses)
 * with their resolved physical base supplied by the caller (the bsp_g2d
 * *_phy parameters); v3d_g2d_run() maintains the ARM/V3D caches around
 * the dispatch and the kernels write the physical addresses directly.
 *
 * ZERO COPY: the GPU never copies canvas pixels.  The kernels write and
 * read the caller's buffers in place through their physical addresses;
 * the only memory the driver touches is the (small) uniform block, which
 * is refreshed per call.  The CSD kernels are preloaded once at init, so
 * a dispatch copies nothing but the uniforms.
 */

#ifndef V3D_G2D_H
#define V3D_G2D_H

#include <stdint.h>
#include <stddef.h>
#include <ewoksys/ewokdef.h>

/* Map registers + allocate dma staging; idempotent.  Returns 0 when the
 * GPU is usable and a real failure otherwise.  Operations outside the
 * documented GPU capability set may still use the CPU compatibility path. */
int v3d_g2d_init(void);

/* Non-zero once the GPU is usable. */
int v3d_g2d_ready(void);

/* Number of QPUs available for CSD dispatch (12 on BCM2712). */
int v3d_g2d_num_qpus(void);

/* Physical address of the TMU write-scratch surface the kernels use for
 * out-of-rect writes and the flush epilogue (feed it to uniforms). */
uint32_t v3d_g2d_scratch_phys(void);

/* Address validation gate: returns non-zero when [phy, phy+bytes) lies
 * inside a legitimate physical RAM region (the allocable memory range,
 * the sys_dma window, the IPC_CONTIG shm slab, or below the total
 * physical memory size).  The GPU kernels write through 32-bit physical
 * addresses with no MMU, so a bad address would corrupt arbitrary
 * memory; every caller-supplied *_phy must pass this check before a
 * dispatch is allowed. */
int v3d_g2d_phy_valid(ewokos_addr_t phy, size_t bytes);

/*
 * Run one CSD dispatch of `code` with `unifs` against the surfaces
 * `src`/`dst` (either may be NULL/0).  src/dst are VIRTUAL addresses;
 * the caller has already substituted physical addresses into the uniform
 * fields that describe them.  This wrapper keeps the ARM/V3D caches
 * coherent around the dispatch (dc civac/ivac for cacheable canvases,
 * nothing for NOCACHE dma canvases).  Returns 0 on success.
 */
int v3d_g2d_run(const uint64_t *code, int nwords,
                const uint32_t *unifs, int nunifs,
                int num_qpus,
                const void *src, size_t src_len,
                void *dst, size_t dst_len);

/* The ARGB8888 CSD kernels (assembled from the .qpu sources). */
extern const uint64_t g2d_qpu_argb_fill[];
extern const unsigned g2d_qpu_argb_fill_n;
extern const uint64_t g2d_qpu_argb_blit[];
extern const unsigned g2d_qpu_argb_blit_n;
extern const uint64_t g2d_qpu_argb_rotate[];
extern const unsigned g2d_qpu_argb_rotate_n;
extern const uint64_t g2d_qpu_argb_rot90[];
extern const unsigned g2d_qpu_argb_rot90_n;
extern const uint64_t g2d_qpu_argb_alpha[];
extern const unsigned g2d_qpu_argb_alpha_n;
extern const uint64_t g2d_qpu_argb_scale_pow2[];
extern const unsigned g2d_qpu_argb_scale_pow2_n;

#endif /* V3D_G2D_H */
