/*
 * ge_g2d.h - SigmaStar SSD202D GE (2D graphics engine) hardware back end
 * for the EwokOS miyoo bsp_g2d layer.
 *
 * The register interface is reverse engineered from the vendor
 * libmi_gfx.so / mi_gfx.ko pair (see the MI_GFX notes that shipped with
 * the retired custom g2dd): the GE lives on the RIU bus at 0x1f281200
 * and takes 16-bit word accesses; buffer addresses are programmed in the
 * MIU bus view (ARM physical address minus the 0x20000000 DRAM window
 * base); a job is a synchronous register sequence - program, kick CMD,
 * poll STATUS.BUSY.
 *
 * Three operations are implemented natively (ported from the retired
 * miyoo g2dd's register path): constant-color rectangle fill, same-size
 * BitBlt, and same-size BitBlt with a global constant alpha through the
 * DFB blend stage (CTRL ABL|DFB, ABL_COEF = 1 selects constant-alpha
 * mode, ABL_CONST carries the alpha replicated in both bytes).
 * Everything else the MI_GFX API hints at has no recovered programming
 * sequence - the vendor library itself exposes neither stretch nor
 * rotation on this chip (MI_GFX_Rotate_e knows only ROTATE_0 and no
 * StretchBlit ioctl was recovered) - and those operations return -1
 * (the client side renders on the cpu when the device path fails).
 *
 * Canvases arrive as MIU bus addresses translated from the caller
 * supplied physical bases (bsp_g2d *_phy).  GE-eligible canvases are
 * physically contiguous shm slabs or dma memory, both mapped NOCACHE in
 * every process, so no ARM cache maintenance is needed around a
 * dispatch.  ZERO COPY: the engine reads and writes the caller's buffers
 * in place.
 */

#ifndef GE_G2D_H
#define GE_G2D_H

#include <stdint.h>
#include <stddef.h>
#include <ewoksys/ewokdef.h>

/* map the GE register block and probe the engine.  safe to call
 * multiple times.  returns 0 when the GE is present and idle. */
int ge_g2d_init(void);

/* != 0 after a successful ge_g2d_init(). */
int ge_g2d_ready(void);

/* address validation gate: is [phy, phy+bytes) a legitimate physical
 * RAM range the GE may access?  The engine has no MMU, so a caller that
 * passes a bad *_phy would let it scribble over arbitrary memory. */
int ge_g2d_phy_valid(ewokos_addr_t phy, size_t bytes);

/* translate an ARM physical address to the GE-visible MIU bus address.
 * returns 0 when the address is outside the DRAM window. */
uint32_t ge_g2d_miu(ewokos_addr_t phy);

/* constant-color fill of the rect (x0,y0)-(x1,y1) (exclusive) on the
 * dst surface (miu = dst MIU base, w = dst pitch in pixels).  synchronous:
 * returns 0 after the engine went idle, -1 on timeout. */
int ge_g2d_fill(uint32_t miu, int32_t w, int32_t h,
                int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                uint32_t argb);

/* same-size BitBlt of the w x h block at (sx,sy) on the src surface to
 * (dx,dy) on the dst surface (MIU bases, pitches in pixels).
 * synchronous: returns 0 after the engine went idle, -1 on timeout. */
int ge_g2d_blit(uint32_t src_miu, int32_t src_w, int32_t src_h,
                int32_t sx, int32_t sy,
                uint32_t dst_miu, int32_t dst_w, int32_t dst_h,
                int32_t dx, int32_t dy, int32_t w, int32_t h);

/* same-size BitBlt of the w x h block at (sx,sy) on the src surface to
 * (dx,dy) on the dst surface with a global constant alpha blended
 * through the DFB stage (out = src*alpha + dst*(255-alpha)).
 * synchronous: returns 0 after the engine went idle, -1 on timeout. */
int ge_g2d_blit_alpha(uint32_t src_miu, int32_t src_w, int32_t src_h,
                int32_t sx, int32_t sy,
                uint32_t dst_miu, int32_t dst_w, int32_t dst_h,
                int32_t dx, int32_t dy, int32_t w, int32_t h,
                uint8_t alpha);

#endif /* GE_G2D_H */
