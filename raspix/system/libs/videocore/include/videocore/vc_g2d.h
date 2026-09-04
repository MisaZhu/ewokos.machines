/*
 * vc_g2d.h - VideoCore back end of the raspix (Raspberry Pi 3 / BCM2837,
 * Pi 4 / BCM2711) bsp_g2d layer: V3D 4.2 CSD kernels on BCM2711 and VC4
 * (V3D 2.1) SRQ kernels on BCM2837.
 *
 * This header is the contract between bsp_g2d.c (public bsp_g2d API,
 * argument validation and the scalar CPU paths) and vc_g2d.c (affine map
 * construction, GPU eligibility gating and every QPU dispatch).  Hardware
 * bring-up, cache handling and the dispatch mechanics live in v3d_g2d.c
 * and stay private to this library, as do the generated QPU instruction
 * streams in g2d_qpu_kernels.h / g2d_qpu_kernels_v42.h.
 *
 * GPU eligibility (on top of the width/size checks):
 *   - the *_contig flag is set and the matching *_phy carries a valid
 *     physical base (the caller resolves it: contig shm slab / sys_dma
 *     memory); the physical address must fit the kernels' 32-bit TMU
 *     addresses.
 *
 * The GPU operates on caller buffers through caller-supplied physical
 * bases.  Alpha's intermediate staging is also GPU-produced; the ARM only
 * builds address vectors and uniforms (see v3d_g2d.c).
 *
 * Every operation here is GPU-only: there is no implicit CPU fallback, and
 * a submitted dispatch is never replayed - a timed-out dispatch may still
 * own or have partially written the destination.
 */

#ifndef VIDEOCORE_VC_G2D_H
#define VIDEOCORE_VC_G2D_H

#include <stdint.h>
#include <stddef.h>
#include <ewoksys/ewokdef.h>

/* GPU operations use a three-state result: success, unsupported before any
 * submission, or failure after submission.  Both non-success states surface
 * as -1 through the public API; keeping them distinct prevents a failed
 * dispatch being mistaken for a capability rejection. */
enum {
    GPU_FAILED = -1,
    GPU_UNSUPPORTED = 0,
    GPU_DONE = 1
};

/* Rotation codes for g2d_map_params (90 = clockwise). */
enum {
    G2D_BSP_MAP_ROT_0 = 0,
    G2D_BSP_MAP_ROT_90 = 1,
    G2D_BSP_MAP_ROT_180 = 2,
    G2D_BSP_MAP_ROT_270 = 3
};

/* Affine map coefficients (Q15 fixed point), identical to the QPU kernels'
 * uniform layout: u = (pu*X + qu*Y)>>15 + cu, v = (pv*X + qv*Y)>>15 + cv
 * for a destination pixel (X, Y). */
typedef struct {
    int32_t pu, qu, pv, qv;     /* Q15 scale factors folded with rotation */
    int32_t cu, cv;             /* constant offsets (source origin / flip) */
} g2d_map_t;

/* Probe the GPU, bring up power/clock and preload the QPU kernels;
 * idempotent.  Returns 0 when the GPU is usable (hardware present AND at
 * least one kernel loaded). */
int vc_g2d_init(void);

/* V3D clock rate in Hz confirmed during vc_g2d_init(), or 0 when unknown. */
uint32_t vc_g2d_clock_hz(void);

/* Build the map that samples src crop (sx,sy,sw,sh) into dst rect
 * (dx,dy,dw,dh), optionally rotated clockwise.  dx/dy only select the
 * rect; the map is origin-independent. */
void g2d_map_params(int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                    int32_t dx, int32_t dy, int32_t dw, int32_t dh,
                    int rotate, g2d_map_t *m);

/* Angle normalised into [0, 360). */
int32_t g2d_norm_degree(int32_t degree);

/* Smallest size able to hold src_w x src_h rotated clockwise by degree
 * (any angle): exact swap/keep for multiples of 90, rotated bounding box
 * otherwise (ceiling-rounded, 14-bit fixed-point trig). */
void g2d_rotated_size(int32_t src_w, int32_t src_h, int32_t degree,
                      int32_t *dst_w, int32_t *dst_h);

/* Whole-surface clockwise rotation map (canonical rotate semantics shared
 * with the argb_rotate QPU kernel), written into the top-left bw x bh box
 * of the destination. */
void g2d_map_rotate(int32_t src_w, int32_t src_h, int32_t degree,
                    int32_t bw, int32_t bh, g2d_map_t *m);

/* Non-zero when the GPU is usable and the surface size is valid. */
int gpu_ok(int32_t w, int32_t h);

/* Validate a caller-provided physical base for the QPU's 32-bit TMU
 * addresses.  Returns the physical address usable by the kernels, or 0
 * when the canvas cannot run on the GPU (not physically contiguous, no phy
 * supplied, > 4 GB physical, or the address fails the RAM-range validation
 * gate). */
uint32_t gpu_phys(ewokos_addr_t phys, size_t bytes, uint8_t contig);

/* Clip the rect (x, y, w, h) against the surf_w x surf_h bounds; returns 0
 * when the clipped rect is empty. */
int gpu_clip_rect(int32_t *x, int32_t *y, int32_t *w, int32_t *h,
                  int32_t surf_w, int32_t surf_h);

/* Non-zero when the map's coefficients fit the kernel's two signed 24x24
 * multiplies over a w x h destination without wrapping 32 bits. */
int gpu_map_fits(const g2d_map_t *m, int64_t w, int64_t h);

/* Constant-color fill of the clipped dst rect [x0,x1) x [y0,y1) of the
 * w x h surface backed by phys/argb.  Picks the back end by hardware
 * version: VC4 tries the self-looping group-fill kernel and falls back to
 * the exact span batches, V3D >= 4 runs the CSD kernel. */
int gpu_fill(uint32_t phys, uint32_t *argb, int32_t w, int32_t h,
             int32_t x0, int32_t y0, int32_t x1, int32_t y1,
             uint32_t color);

/* Copy of the source through m into the clipped dst rect [x0,x1) x
 * [y0,y1); no_clamp != 0 declares a whole-surface destination (scaling),
 * which lets the kernel skip the dst in-rect clamp. */
int gpu_blit_surface(const g2d_map_t *m,
                     uint32_t src_phys, uint32_t *argb_src,
                     int32_t src_w, int32_t src_h,
                     uint32_t dst_phys, uint32_t *argb_dst,
                     int32_t dst_w, int32_t dst_h,
                     int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                     int no_clamp);

/* Source-over blend of the source through m into the clipped dst rect
 * [x0,x1) x [y0,y1) at a constant global alpha.  Deliberately has no
 * fallback path: blends are not idempotent, so a decline or a failure is
 * reported to the caller. */
int gpu_alpha_surface(const g2d_map_t *m, uint8_t alpha,
                      uint32_t src_phys, uint32_t *argb_src,
                      int32_t src_w, int32_t src_h,
                      uint32_t dst_phys, uint32_t *argb_dst,
                      int32_t dst_w, int32_t dst_h,
                      int32_t x0, int32_t y0, int32_t x1, int32_t y1);

/* Whole-surface clockwise rotation through m (any angle), writing every
 * destination pixel: rotated content, or transparent 0 for pixels whose
 * pre-clamp source coordinate is out of range. */
int gpu_rotate_surface(const g2d_map_t *m,
                       uint32_t src_phys, uint32_t *argb_src,
                       int32_t src_w, int32_t src_h,
                       uint32_t dst_phys, uint32_t *argb_dst,
                       int32_t dst_w, int32_t dst_h);

/* Dedicated right-angle (rot == 90/270) whole-surface rotation: source
 * reads are contiguous and only writes stride, avoiding the generic affine
 * kernel's 16 stride-apart TMU reads.  Returns GPU_UNSUPPORTED on VC4, for
 * any other angle, or when the destination is not the exact size swap. */
int gpu_rot90_surface(uint32_t src_phys, uint32_t *argb_src,
                      int32_t src_w, int32_t src_h,
                      uint32_t dst_phys, uint32_t *argb_dst,
                      int32_t dst_w, int32_t dst_h, int rot);

#endif /* VIDEOCORE_VC_G2D_H */
