/*
 * g2d_cpu.h - software (CPU) rasterizer for the bsp_g2d ARGB8888 API.
 *
 * Semantics mirror ../app/src/g2d.c (the reference implementation that is
 * bit-identical to the V3D CSD kernels): the blit/scale affine mapping
 * uses the same Q15 coefficients as the GPU kernels and clamps
 * out-of-range samples to the source edge, so CPU and GPU outputs agree
 * pixel-for-pixel whenever both paths are usable.  Rotate / rotated_size
 * follow the EwokOS aarch64 arch_g2d semantics (14-bit fixed-point trig,
 * clockwise rotation, bounding-box sizes with transparent pixels outside
 * the rotated content).
 *
 * This file is pure C (no hardware dependencies) so it can be compiled
 * and tested on the host.
 */

#ifndef G2D_CPU_H
#define G2D_CPU_H

#include <stddef.h>
#include <stdint.h>

/* Affine map coefficients (Q15 fixed point), identical to the GPU
 * kernels' uniform layout: u = (pu*X + qu*Y)>>15 + cu, v = (pv*X + qv*Y)
 * >>15 + cv for a destination pixel (X, Y). */
typedef struct {
    int32_t pu, qu, pv, qv;     /* Q15 scale factors folded with rotation */
    int32_t cu, cv;             /* constant offsets (source origin / flip) */
} g2d_cpu_map_t;

/* Rotation codes for g2d_cpu_map_params (90 = clockwise). */
enum {
    G2D_MAP_ROT_0 = 0,
    G2D_MAP_ROT_90 = 1,
    G2D_MAP_ROT_180 = 2,
    G2D_MAP_ROT_270 = 3
};

/* Build the map that samples src crop (sx,sy,sw,sh) into dst rect
 * (dx,dy,dw,dh), optionally rotated clockwise.  dx/dy only select the
 * rect; the map is origin-independent. */
void g2d_cpu_map_params(int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                    int32_t dx, int32_t dy, int32_t dw, int32_t dh,
                    int rotate, g2d_cpu_map_t *m);

/* Fill a clipped rectangle of the surface with a solid color. */
void g2d_cpu_fill(uint32_t *argb, int32_t argb_w, int32_t argb_h,
                  int32_t x, int32_t y, int32_t w, int32_t h,
                  uint32_t color);

/* Nearest-neighbour blit with crop + scale (no rotation; the bsp API has
 * no rotation parameter on blit).  dst pixels outside the surface or
 * source samples outside the crop are clipped/clamped like the app's
 * g2d_blit. */
void g2d_cpu_blt(uint32_t *argb_src, int32_t src_w, int32_t src_h,
                 int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                 uint32_t *argb_dst, int32_t dst_w, int32_t dst_h,
                 int32_t dx, int32_t dy, int32_t dw, int32_t dh);

/* Same with global alpha.  Blend formula (matches EwokOS
 * graph_blend_argb and the app's g2d_blit_alpha):
 *   sa = (src_a * alpha) >> 8
 *   out_a = dst_a + ((255 - dst_a) * sa) / 255
 *   out_c = (src_c * sa + dst_c * (255 - sa)) / 255
 */
void g2d_cpu_blt_alpha(uint32_t *argb_src, int32_t src_w, int32_t src_h,
                       int32_t sx, int32_t sy, int32_t sw, int32_t sh,
                       uint32_t *argb_dst, int32_t dst_w, int32_t dst_h,
                       int32_t dx, int32_t dy, int32_t dw, int32_t dh,
                       uint8_t alpha);

/* Scale the whole source surface into the whole destination surface
 * (nearest neighbour through the Q15 map, bit-identical to the GPU
 * blit kernel). */
void g2d_cpu_scale_to(uint32_t *argb_src, int32_t src_w, int32_t src_h,
                      uint32_t *argb_dst, int32_t dst_w, int32_t dst_h);

/* Smallest size able to hold src_w x src_h rotated clockwise by degree
 * (any angle): exact swap/keep for multiples of 90, rotated bounding box
 * otherwise (ceiling-rounded, 14-bit fixed-point trig). */
void g2d_cpu_rotated_size(int32_t src_w, int32_t src_h, int32_t degree,
                          int32_t *dst_w, int32_t *dst_h);

/* Rotate the whole source surface clockwise by degree (any angle) into
 * dst, which must be at least the size returned by
 * g2d_cpu_rotated_size().  For angles other than 0/90/180/270 pixels
 * outside the rotated content become transparent (0).  In-place
 * (argb_src == argb_dst) is only valid for 0/180. */
void g2d_cpu_rotate(uint32_t *argb_src, int32_t src_w, int32_t src_h,
                    uint32_t *argb_dst, int32_t dst_w, int32_t dst_h,
                    int32_t degree);

#endif /* G2D_CPU_H */
