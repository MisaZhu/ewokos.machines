/*
 * vc_g2d.h - VideoCore back end of the raspi5 (Raspberry Pi 5 / BCM2712)
 * bsp_g2d layer: V3D 7.1 CSD kernels driving offline ARGB8888 drawing.
 *
 * This header is the contract between bsp_g2d.c (public bsp_g2d API,
 * argument validation and the scalar CPU paths) and vc_g2d.c (affine map
 * construction, GPU eligibility gating, the large-surface batching policy
 * and every QPU dispatch).  Hardware bring-up, cache handling and the
 * submission mechanics live in v3d_g2d.c and stay private to this library,
 * as do the generated QPU instruction streams in g2d_qpu_kernels.h.
 *
 * GPU eligibility (on top of the width/size checks):
 *   - the *_contig flag is set and the matching *_phy carries a valid
 *     physical base (the caller resolves it: contig shm slab / sys_dma
 *     memory); the physical address must fit the kernels' 32-bit TMU
 *     addresses.
 *
 * ZERO COPY: the GPU operates directly on the caller's buffers through the
 * caller-supplied physical bases; the kernels are preloaded once at init
 * and a dispatch only refreshes the small uniform block.
 *
 * Naming: a *_op entry point owns the whole operation, including the
 * large-surface banding/tiling policy and the kernel choice, so callers
 * never see the generated kernel arrays or the cache-maintenance flags.
 * A *_surface entry point is a single dispatch of one rect.
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

/* Affine map coefficients (Q15 fixed point), identical to the GPU
 * kernels' uniform layout: u = (pu*X + qu*Y)>>15 + cu, v = (pv*X + qv*Y)
 * >>15 + cv for a destination pixel (X, Y). */
typedef struct {
    int32_t pu, qu, pv, qv;     /* Q15 scale factors folded with rotation */
    int32_t cu, cv;             /* constant offsets (source origin / flip) */
} g2d_map_t;

/* Rotation codes for g2d_map_params (90 = clockwise). */
enum {
    G2D_MAP_ROT_0 = 0,
    G2D_MAP_ROT_90 = 1,
    G2D_MAP_ROT_180 = 2,
    G2D_MAP_ROT_270 = 3
};

/* Probe the GPU, bring up power/clock and preload the QPU kernels;
 * idempotent.  Returns 0 on success. */
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

/* Non-zero when the identity 1:1 copy of src (sx0,sy0)+(x0,y0,w,h) into
 * the dst row stride dst_w can run on the vec4 copy kernel, which moves 4x
 * the bytes of argb_blit per TMU request. */
int gpu_copy_eligible(uint32_t src_phys, int32_t src_w, int32_t src_h,
                      int32_t sx0, int32_t sy0,
                      uint32_t dst_phys, int32_t dst_w,
                      int32_t x0, int32_t w, int32_t h);

/* Constant-color fill of the clipped dst rect [x0,x1) x [y0,y1) of the
 * w x h surface backed by phys/argb (argb_fill kernel). */
int gpu_fill_surface(uint32_t phys, uint32_t *argb, int32_t w, int32_t h,
                     int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                     uint32_t color);

/* Identity 1:1 copy of the src rect (csx,csy)+[rx,rx1) x [ry,ry1) into the
 * dst surface modelled surf_w pixels wide (argb_copy kernel).  dst_bytes is
 * the destination image memory used for the large-surface test; argb_dst is
 * NULL for a raw physical destination with no kernel-visible VA. */
int gpu_copy_op(uint32_t src_phys, uint32_t *argb_src,
                int32_t src_w, int32_t src_h,
                uint32_t dst_phys, uint32_t *argb_dst, int32_t surf_w,
                int32_t csx, int32_t csy,
                int32_t rx, int32_t ry, int32_t rx1, int32_t ry1,
                size_t dst_bytes);

/* Copy of the source through m into the clipped dst rect [rx,rx1) x
 * [ry,ry1) of the surf_w x surf_h destination (argb_blit kernel).
 * dst_bytes is the destination image memory used for the large-surface
 * test; argb_dst is NULL for a raw physical destination. */
int gpu_blit_op(const g2d_map_t *m,
                uint32_t src_phys, uint32_t *argb_src,
                int32_t src_w, int32_t src_h,
                uint32_t dst_phys, uint32_t *argb_dst,
                int32_t surf_w, int32_t surf_h,
                int32_t rx, int32_t ry, int32_t rx1, int32_t ry1,
                size_t dst_bytes);

/* Source-over blend of the source through m into the clipped dst rect
 * [rx,rx1) x [ry,ry1) at a constant global alpha (argb_alpha kernel).
 * Blends are not idempotent, so a failure is reported, never retried on
 * the CPU. */
int gpu_alpha_op(const g2d_map_t *m, uint8_t alpha,
                 uint32_t src_phys, uint32_t *argb_src,
                 int32_t src_w, int32_t src_h,
                 uint32_t dst_phys, uint32_t *argb_dst,
                 int32_t dst_w, int32_t dst_h,
                 int32_t rx, int32_t ry, int32_t rx1, int32_t ry1,
                 size_t dst_bytes);

/* Whole-destination scale through the corner-preserving map m: aligned
 * exact power-of-two downscales take the faster argb_scale_pow2 kernel,
 * every other shape takes argb_blit. */
int gpu_scale_op(const g2d_map_t *m,
                 uint32_t src_phys, uint32_t *argb_src,
                 int32_t src_w, int32_t src_h,
                 uint32_t dst_phys, uint32_t *argb_dst,
                 int32_t dst_w, int32_t dst_h);

/* Dedicated right-angle (rot == 90/270) whole-surface rotation: source
 * reads are contiguous and only writes stride, avoiding the generic affine
 * kernel's 16 stride-apart TMU reads.  Returns 0 when the geometry is not
 * eligible (src width % 16, exact size swap, QPU count) or the dispatch
 * failed; the caller then falls through to the generic paths. */
int gpu_rot90_surface(uint32_t src_phys, uint32_t *argb_src,
                      int32_t src_w, int32_t src_h,
                      uint32_t dst_phys, uint32_t *argb_dst,
                      int32_t dst_w, int32_t dst_h, int rot);

/* Whole-surface clockwise rotation through m into the bw x bh content box
 * of the dst_w x dst_h destination: exact right-angle maps whose content
 * fits the destination take the shorter argb_blit kernel, every other
 * angle/size takes argb_rotate (which writes every destination pixel -
 * rotated content, or transparent 0 outside the content box). */
int gpu_rotate_op(const g2d_map_t *m, int32_t rot, int32_t bw, int32_t bh,
                  uint32_t src_phys, uint32_t *argb_src,
                  int32_t src_w, int32_t src_h,
                  uint32_t dst_phys, uint32_t *argb_dst,
                  int32_t dst_w, int32_t dst_h);

#endif /* VIDEOCORE_VC_G2D_H */
