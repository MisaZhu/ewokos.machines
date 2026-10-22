/*
 * adreno_2d.c - Adreno 690 (SC8280XP / Q8B) A2D 2D-blitter back end.
 *
 * Implements the solid-fill and copy/scale/rotate 2D operations that bsp_g2d
 * and bsp_g3d route to the GPU, using the a6xx A2D blitter.  Every blit is a
 * stream of PKT4 register writes followed by a single-dword CP_BLIT trigger,
 * submitted as one indirect buffer through adreno_ring_submit().
 *
 * ---------------------------------------------------------------------------
 * PROVENANCE (all offsets / bitfields / enums / emit order verified against)
 * ---------------------------------------------------------------------------
 *   Mesa freedreno (main branch):
 *     src/freedreno/common/fd6_blitter.cc
 *       emit_setup          -> CP_SET_MARKER(RM6_BLIT2DSCALE) + RB_CCU_CNTL
 *       emit_blit_setup     -> RB/GRAS_A2D_BLT_CNTL, SP_A2D_OUTPUT_INFO,
 *                              RB_A2D_PIXEL_CNTL
 *       emit_blit_dst       -> RB_A2D_DEST_BUFFER_INFO/BASE/PITCH
 *       emit_blit_src       -> TPL1_A2D_SRC_TEXTURE_INFO/SIZE/BASE/PITCH/FLAG_*
 *       emit_blit_texture_setup -> GRAS_A2D_SRC_XMIN..YMAX, DEST_TL/BR, rotate
 *       emit_clear_color    -> RB_A2D_CLEAR_COLOR_DW0..DW3 (R,G,B,A for UNORM8)
 *       clear_surface_setup -> DEST_TL/BR + clear color + emit_blit_setup
 *       emit_blit_fini      -> CP_BLIT(op = BLIT_OP_SCALE)
 *     src/freedreno/common/fd6_emit.{cc,h}
 *       fd6_set_render_mode -> CP_SET_MARKER,1 payload A6XX_CP_SET_MARKER_0
 *       fd6_emit_gmem_cache_cntl(false) -> CP_WAIT_FOR_IDLE + RB_CCU_CNTL
 *       fd6_set_rb_dbg_eco_mode -> quirk-gated RB_DBG_ECO_CNTL RMW (see below)
 *     registers/adreno/{a6xx,a6xx_enums,adreno_pm4,adreno_common}.xml
 *   Linux drivers/gpu/drm/msm/adreno/adreno_gpu.h  (PKT4 / PKT7 / PM4_PARITY)
 *
 * Register offsets in a6xx_regs.h are the raw PKT4 register indices (NOT the
 * *4 MMIO word indices used by adreno_gpu_read/write).  A2D blits never touch
 * MMIO: the whole operation is a PM4 dword stream the CP fetches from the IB.
 *
 * ---------------------------------------------------------------------------
 * FORMAT
 * ---------------------------------------------------------------------------
 * EwokOS surfaces are uint32 ARGB (0xAARRGGBB).  In little-endian memory that
 * is the byte order [B,G,R,A], which is exactly PIPE_FORMAT_B8G8R8A8_UNORM =
 * FMT6_8_8_8_8_UNORM (0x30), tile mode TILE6_LINEAR (0).  Copy/scale/blend use
 * color_swap WZYX on BOTH src and dst, so the channel order is a no-op for them
 * (and the blend's alpha is logical W = byte 3 = A either way).  Only that one
 * format is handled; anything else is declined so the client falls back.
 *
 * ---------------------------------------------------------------------------
 * HONEST GAPS - NOT hardware-validated on SC8280XP (QEMU is rejected as a
 * validation environment).  Items to confirm on real silicon:
 *
 *   1. Fill channel->memory byte order (RESOLVED, was a red/blue swap).
 *      emit_clear_color writes DW0..DW3 = logical R,G,B,A and the dest
 *      color_swap maps logical->memory: fd6_format_table.c gives WZYX for
 *      R8G8B8A8 (DW0 lands in byte 0) and WXYZ for B8G8R8A8.  With WZYX
 *      the four DWs are therefore simply the four bytes of the stored
 *      32-bit word, so adreno_2d_fill feeds DWi = byte i of `color`.  That
 *      one rule serves both callers: bsp_g2d (0xAARRGGBB into a BGRA
 *      canvas) and bsp_g3d_clear (0xAABBGGRR into an RGBA8 framebuffer,
 *      the same verbatim-store contract raspi5's build_clear_rcl uses).
 *      rb_swap swaps bytes 0 and 2 of the stored word.
 *
 *   2. RB_DBG_ECO_CNTL bit24 RMW is intentionally SKIPPED.  Mesa only emits it
 *      (fd6_set_rb_dbg_eco_mode) when FD_QUIRK(QCTDD04536579) is set, and that
 *      quirk is unconfirmed for A690.  If blits misbehave, the first thing to
 *      try is wrapping CP_BLIT with:
 *         CP_WAIT_FOR_IDLE; CP_REG_RMW(RB_DBG_ECO_CNTL, and=~BIT24, or=BIT24)
 *         ... CP_BLIT ...
 *         CP_WAIT_FOR_IDLE; CP_REG_RMW(RB_DBG_ECO_CNTL, and=~BIT24, or=0)
 *
 *   3. Sysmem A2D still uses the CCU, backed by GMEM.  Match Mesa's A690
 *      config_sysmem: 8 CCUs, 64 KiB depth and 64 KiB color per CCU,
 *      depth offset 0, color offset 0x80000, both full size.  Clean CCU
 *      color/depth before invalidation and explicitly clean color after
 *      CP_BLIT before the submission's UCHE fence.  Hardware validation
 *      of this corrected cache sequence is pending.
 *
 *   4. Rotate is limited to 0 and 180 degrees.  Mesa only ever derives
 *      ROTATE_0 / HFLIP / VFLIP / 180 from source-vs-dest mirroring; there is
 *      no source-demonstrated destination-rect convention for ROTATE_90/270,
 *      so 90/270 are DECLINED (-1) rather than guessed (an incorrect dst rect
 *      could drive an out-of-bounds GPU write on unvalidated hardware).
 *
 *   5. The whole blit assumes the A2D pipeline is usable right after CP
 *      bring-up.  Mesa emits a large context/magic-register state before any
 *      blit; we emit only the registers the blit itself reads.  If the A2D
 *      unit needs additional static state on A690 it must be added here.
 *
 *   6. adreno_2d_blend() programs RB_A2D_PIXEL_CNTL = PIXEL_OP_BLENDING with
 *      the documented SRC_ALPHA / ONE_MINUS_SRC_ALPHA factors (a standard
 *      source-over).  Mesa only ever uses A2D PIXEL_OP_BLENDING for D24S8
 *      depth/stencil channel-preserve (FACTOR_ONE) and its can_do_blit()
 *      rejects info->alpha_blend, so a *colour* source-over through the A2D is
 *      register-documented but NOT Mesa-demonstrated; whether the A2D colour
 *      datapath honours arithmetic SRC_ALPHA blending like the 3D RB must be
 *      confirmed on silicon.  It also blends by the per-pixel source alpha
 *      only - it cannot express blt_alpha's sa = src.a * global_alpha product
 *      (no A2D constant-colour register exists), so the bsp restricts it to
 *      global alpha 0xff and declines the rest.
 *
 *   7. adreno_2d_blur() is a bilinear scale-cascade (mip) blur, NOT a per-tap
 *      weighted gaussian.  The A2D has no convolution op (CP_BLIT is only
 *      FILL/COPY/SCALE) and no kernel-weight register, so an exact gaussian
 *      needs a compute/fragment shader. The public Gaussian API declines
 *      until that exact kernel exists. This private approximation halves the rect into a scratch
 *      mip chain with the documented bilinear FILTER on BLIT_OP_SCALE ("used
 *      for mipmap generation") and bilinear-upsamples the chain back, which
 *      converges to a gaussian by repeated 2-tap averaging (CLT).  This is a
 *      real GPU blur - the same downsample->upsample strategy raspi's own GPU
 *      blur uses - but its kernel is an approximation of arch_g2d_gaussian's,
 *      so a pixel-exact comparison against the CPU reference will differ.
 *      L is derived from radius (~2^L) and is not hardware-validated.
 *
 * The GPU path is opt-in: the bsp layer gates these calls behind ADRENO_ENABLE
 * so an unvalidated blitter is never the default.  These functions return 0 on
 * success and a negative value when the op is ineligible, the GPU is not up, or
 * the submission faulted/timed out; the caller surfaces that as -1 and the
 * client falls back to its in-process engine.  There is no CPU path here.
 */
#include <adreno/adreno_drv.h>
#include <adreno/adreno_pm4.h>
#include <adreno/a6xx_regs.h>
#include <ewoksys/klog.h>

/* ------------------------------------------------------------------ */
/* Capability latch.  adreno_2d_init() (adreno_a6xx.c) brings up the    */
/* whole GPU and is itself latched; this adds a cheap 2D-path gate so   */
/* callers do not re-probe on every op.                                */
/* ------------------------------------------------------------------ */
static int g_2d_ready = 0;   /* 0 = unprobed, 1 = ready, -1 = declined */

int adreno_2d_ready(void) {
	if (g_2d_ready == 0)
		g_2d_ready = (adreno_2d_init() == 0) ? 1 : -1;
	return g_2d_ready == 1;
}

/* ------------------------------------------------------------------ */
/* Indirect-buffer builder. Cache events include timestamp addresses; */
/* 128 leaves room for both paths. ib_w() is overflow-safe: if the      */
/* buffer were ever too small the trailing CP_BLIT would be dropped and */
/* the submit would simply do nothing rather than corrupt memory.       */
/* ------------------------------------------------------------------ */
#define IB_MAX 128u

struct ib {
	uint32_t d[IB_MAX];
	uint32_t n;
};

static void ib_w(struct ib *b, uint32_t v) {
	if (b->n < IB_MAX)
		b->d[b->n++] = v;
}

/* PKT4: write `_cnt` consecutive registers starting at raw index `reg`. */
static void ib_pkt4(struct ib *b, uint32_t reg, uint32_t cnt) {
	ib_w(b, ADRENO_PKT4(reg, cnt));
}

/* PKT7: opcode with `cnt` payload dwords. */
static void ib_pkt7(struct ib *b, uint32_t opcode, uint32_t cnt) {
	ib_w(b, ADRENO_PKT7(opcode, cnt));
}

/* ------------------------------------------------------------------ */
/* Alignment / range limits implied by the register field encodings.    */
/*   RB_A2D_DEST_BUFFER_BASE  : reg64 waddress, align 64                */
/*   TPL1_A2D_SRC_TEXTURE_BASE: reg64 address,   align 16               */
/*   *_PITCH                  : shr6 (64-byte granularity)             */
/*   GRAS_A2D_DEST_TL/BR      : a6xx_reg_xy, X/Y are 14-bit [0:13]     */
/*   GRAS_A2D_SRC_*           : signed 17-bit field [8:24]             */
/*   TPL1_A2D_SRC_TEXTURE_SIZE: WIDTH/HEIGHT 15-bit [0:14]/[15:29]     */
/* ------------------------------------------------------------------ */
#define A2D_DEST_ALIGN   64u
#define A2D_SRC_ALIGN    16u
#define A2D_PITCH_ALIGN  64u
#define A2D_XY_MAX       0x3fff   /* 14-bit dest coord (inclusive max) */
#define A2D_SRC_MAX      0x7fff   /* 15-bit src texture size           */

/* Mesa fd6_event_write(A6XX): CCU clean events require a timestamp payload.
   Use a separate word in the existing privileged scratch BO; this marker
   is not the ring completion fence. The final fence orders the whole IB. */
static void ib_ccu_clean(struct ib *b, uint32_t event) {
	uint64_t marker = adreno_dev()->ring.scratch.iova + 16;
	ib_pkt7(b, CP_EVENT_WRITE, 4);
	ib_w(b, event | (1u << 30));  /* TIMESTAMP, no interrupt */
	ib_w(b, (uint32_t)marker);
	ib_w(b, (uint32_t)(marker >> 32));
	ib_w(b, 0);
}

static void ib_event(struct ib *b, uint32_t event) {
	ib_pkt7(b, CP_EVENT_WRITE, 1);
	ib_w(b, event);
}

/* Mesa fd6_blitter.cc emit_setup() + fd6_gmem_cache.h config_sysmem.
   A2D sysmem writes still pass through CCU; CACHE_FLUSH_TS alone is an
   UCHE clean, not the required CCU color/depth clean. */
static void ib_prologue(struct ib *b) {
	ib_ccu_clean(b, PC_CCU_FLUSH_COLOR_TS);
	ib_ccu_clean(b, PC_CCU_FLUSH_DEPTH_TS);
	ib_event(b, PC_CCU_INVALIDATE_COLOR);
	ib_event(b, PC_CCU_INVALIDATE_DEPTH);
	ib_event(b, CACHE_INVALIDATE);

	ib_pkt7(b, CP_SET_MARKER, 1);
	ib_w(b, A6XX_SET_MARKER_BLIT2DSCALE);     /* RM6_BLIT2DSCALE, !uses_gmem */

	ib_pkt7(b, CP_WAIT_FOR_IDLE, 0);          /* fd6_emit_gmem_cache_cntl() */

	ib_pkt4(b, REG_A6XX_RB_CCU_CNTL, 1);
	/* A690: color follows 8 * 64 KiB depth cache. COLOR_OFFSET is
	   shr12 in bits 23..31; depth offset/cache-size fields remain zero. */
	ib_w(b, ((0x80000u >> 12) << 23));
}

/* Shared epilogue: trigger the blit.  emit_blit_fini() minus the cosmetic
   FD_LABEL event write and the quirk-gated RB_DBG_ECO_CNTL RMW (gap #2). */
static void ib_blit_trigger(struct ib *b) {
	ib_pkt7(b, CP_BLIT, 1);
	ib_w(b, A6XX_BLIT_OP_SCALE);              /* CP_BLIT_0.OP = BLIT_OP_SCALE */
	/* Make A2D's CCU writes visible to both CPU readback and a following
	   texture fetch before adreno_ring_submit emits its UCHE fence. */
	ib_ccu_clean(b, PC_CCU_FLUSH_COLOR_TS);
	ib_pkt7(b, CP_WAIT_FOR_IDLE, 0);
}

/* ------------------------------------------------------------------ */
/* Solid fill.  Follows clear_surface_setup() + emit_blit_dst() +       */
/* emit_blit_fini(): dest rect, clear color, blit control (SOLID_COLOR  */
/* set), dest buffer, trigger.  `color` is the 32-bit word to STORE in   */
/* every pixel (see FORMAT note 1); rb_swap swaps its bytes 0 and 2.    */
/* Returns 0 on success, negative on decline/failure.                  */
/* ------------------------------------------------------------------ */
int adreno_2d_fill(uint64_t dst_phy, int32_t dst_w, int32_t dst_h,
                   uint32_t dst_pitch, int32_t x, int32_t y, int32_t w,
                   int32_t h, uint32_t color, int rb_swap) {
	struct ib b;
	uint32_t blt_cntl, out_info, b0, b1, b2, b3;
	int ret;

	if (!adreno_2d_ready())
		return -1;

	/* --- eligibility --- */
	if (dst_phy == 0 || w <= 0 || h <= 0 || dst_w <= 0 || dst_h <= 0)
		return -1;
	if (x < 0 || y < 0 || x + w > dst_w || y + h > dst_h)
		return -1;
	if ((dst_phy % A2D_DEST_ALIGN) || (dst_pitch % A2D_PITCH_ALIGN))
		return -1;
	if (dst_pitch == 0)
		return -1;
	if ((uint32_t)(x + w - 1) > A2D_XY_MAX || (uint32_t)(y + h - 1) > A2D_XY_MAX)
		return -1;

	/* --- control registers (FMT6_8_8_8_8_UNORM, linear, WZYX, mask=0xf) --- */
	blt_cntl = A2D_BLT_SOLID_COLOR |
	           A2D_BLT_COLOR_FORMAT(FMT6_8_8_8_8_UNORM) |
	           A2D_BLT_MASK(0xf) |
	           A2D_BLT_IFMT(R2D_UNORM8);          /* rotate = 0 */
	out_info = A2D_OUT_IFMT_TYPE(OUTPUT_IFMT_2D_FLOAT) |
	           A2D_OUT_COLOR_FORMAT(FMT6_8_8_8_8_UNORM) |
	           A2D_OUT_MASK(0xf);

	/* --- clear color: with dest swap WZYX, CLEAR_COLOR_DWi -> memory byte i
	   (fd6_format_table.c: R8G8B8A8, byte0=R, is WZYX and emit_clear_color
	   puts R in DW0).  So feed the bytes of the word we want stored. --- */
	b0 = color & 0xffu;
	b1 = (color >> 8) & 0xffu;
	b2 = (color >> 16) & 0xffu;
	b3 = (color >> 24) & 0xffu;
	if (rb_swap) { uint32_t t = b0; b0 = b2; b2 = t; }

	b.n = 0;
	ib_prologue(&b);

	/* GRAS_A2D_DEST_TL(0x8405) + DEST_BR(0x8406): 2 consecutive. */
	ib_pkt4(&b, REG_A6XX_GRAS_A2D_DEST_TL, 2);
	ib_w(&b, A6XX_REG_XY(x, y));
	ib_w(&b, A6XX_REG_XY(x + w - 1, y + h - 1));

	/* RB_A2D_CLEAR_COLOR_DW0..DW3 (0x8c2c..0x8c2f): 4 consecutive. */
	ib_pkt4(&b, REG_A6XX_RB_A2D_CLEAR_COLOR_DW0, 4);
	ib_w(&b, b0);
	ib_w(&b, b1);
	ib_w(&b, b2);
	ib_w(&b, b3);

	/* RB_A2D_BLT_CNTL(0x8c00) + RB_A2D_PIXEL_CNTL(0x8c01, =0): 2 consecutive.
	   PIXEL_CNTL=0 is pixel_op=DISABLED with all blend factors FACTOR_ZERO. */
	ib_pkt4(&b, REG_A6XX_RB_A2D_BLT_CNTL, 2);
	ib_w(&b, blt_cntl);
	ib_w(&b, 0);

	/* GRAS_A2D_BLT_CNTL(0x8400): identical fields to the RB copy. */
	ib_pkt4(&b, REG_A6XX_GRAS_A2D_BLT_CNTL, 1);
	ib_w(&b, blt_cntl);

	/* SP_A2D_OUTPUT_INFO(0xacc0). */
	ib_pkt4(&b, REG_A6XX_SP_A2D_OUTPUT_INFO, 1);
	ib_w(&b, out_info);

	/* RB_A2D_DEST_BUFFER_INFO(0x8c17) + BASE lo/hi(0x8c18/0x8c19) +
	   PITCH(0x8c1a): 4 consecutive.  INFO = fmt | TILE6_LINEAR | WZYX. */
	ib_pkt4(&b, REG_A6XX_RB_A2D_DEST_BUFFER_INFO, 4);
	ib_w(&b, A2D_DEST_COLOR_FORMAT(FMT6_8_8_8_8_UNORM) |
	         A2D_DEST_TILE_MODE(TILE6_LINEAR) |
	         A2D_DEST_COLOR_SWAP(A3XX_COLOR_SWAP_WZYX));
	ib_w(&b, (uint32_t)(dst_phy & 0xffffffffu));
	ib_w(&b, (uint32_t)(dst_phy >> 32));
	ib_w(&b, A2D_DEST_PITCH(dst_pitch));

	ib_blit_trigger(&b);

	ret = adreno_ring_submit(adreno_dev(), b.d, b.n);
	if (ret != ADRENO_OK) {
		if (ret != ADRENO_E_WEDGED)   /* wedge itself is logged once by the ring */
			klog("adreno_2d: fill submit failed (%d)\n", ret);
		return ret;
	}
	return ADRENO_OK;
}

/* ------------------------------------------------------------------ */
/* Copy / scale / rotate / blend core.  Follows emit_blit_texture_setup()*/
/* + emit_blit_src() + emit_blit_dst() + emit_blit_fini().  Both surfaces*/
/* are FMT6_8_8_8_8_UNORM linear; the src is sampled as a 2D texture     */
/* (nearest by default, explicitly linear for private mip blur).       */
/* degree is 0 or 180                                                */
/* only (gap #4).  pixel_cntl selects the RB_A2D_PIXEL_CNTL op: 0 =      */
/* PIXEL_OP_DISABLED (straight overwrite, a copy), A2D_PIXEL_SRC_OVER =  */
/* per-pixel source-over blend.  Returns 0 on success, negative on       */
/* decline/failure.                                                     */
/* ------------------------------------------------------------------ */
static int a2d_copy_blit(uint64_t src_phy, int32_t src_w, int32_t src_h,
                   uint32_t src_pitch, int32_t sx, int32_t sy, int32_t sw,
                   int32_t sh, uint64_t dst_phy, int32_t dst_w, int32_t dst_h,
                   uint32_t dst_pitch, int32_t dx, int32_t dy, int32_t dw,
                   int32_t dh, int32_t degree, uint32_t pixel_cntl,
                   uint32_t filter) {
	struct ib b;
	uint32_t blt_cntl, out_info, src_info, rotate;
	int ret;

	if (!adreno_2d_ready())
		return -1;

	/* --- rotation: only 0 and 180 are source-demonstrated (gap #4) --- */
	if (degree == 0)        rotate = A6XX_ROTATE_0;
	else if (degree == 180) rotate = A6XX_ROTATE_180;
	else                    return -1;

	/* --- eligibility --- */
	if (src_phy == 0 || dst_phy == 0)
		return -1;
	if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
		return -1;
	if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
		return -1;
	if (sx < 0 || sy < 0 || sx + sw > src_w || sy + sh > src_h)
		return -1;
	if (dx < 0 || dy < 0 || dx + dw > dst_w || dy + dh > dst_h)
		return -1;
	if ((src_phy % A2D_SRC_ALIGN) || (dst_phy % A2D_DEST_ALIGN))
		return -1;
	if ((src_pitch % A2D_PITCH_ALIGN) || (dst_pitch % A2D_PITCH_ALIGN))
		return -1;
	if (src_pitch == 0 || dst_pitch == 0)
		return -1;
	if ((uint32_t)src_w > A2D_SRC_MAX || (uint32_t)src_h > A2D_SRC_MAX)
		return -1;
	if ((uint32_t)(dx + dw - 1) > A2D_XY_MAX ||
	    (uint32_t)(dy + dh - 1) > A2D_XY_MAX)
		return -1;

	/* The public g2d scale contract requires nearest sampling. Only the
	   private mip-blur path explicitly requests linear filtering. */

	/* --- control registers --- */
	blt_cntl = A2D_BLT_ROTATE(rotate) |               /* solid_color = 0 */
	           A2D_BLT_COLOR_FORMAT(FMT6_8_8_8_8_UNORM) |
	           A2D_BLT_MASK(0xf) |
	           A2D_BLT_IFMT(R2D_UNORM8);
	out_info = A2D_OUT_IFMT_TYPE(OUTPUT_IFMT_2D_FLOAT) |
	           A2D_OUT_COLOR_FORMAT(FMT6_8_8_8_8_UNORM) |
	           A2D_OUT_MASK(0xf);
	src_info = A2D_SRC_COLOR_FORMAT(FMT6_8_8_8_8_UNORM) |
	           A2D_SRC_TILE_MODE(TILE6_LINEAR) |
	           A2D_SRC_COLOR_SWAP(A3XX_COLOR_SWAP_WZYX) |
	           A2D_SRC_FILTER(filter) |
	           A2D_SRC_UNK20 | A2D_SRC_UNK22;

	b.n = 0;
	ib_prologue(&b);

	/* GRAS_A2D_BLT_CNTL(0x8400) + SRC_XMIN..YMAX(0x8401..0x8404) +
	   DEST_TL(0x8405) + DEST_BR(0x8406): 7 consecutive registers.
	   Src coords are MIN2 / MAX2-1 (no mirroring: sx1<sx2, dx1<dx2). */
	ib_pkt4(&b, REG_A6XX_GRAS_A2D_BLT_CNTL, 7);
	ib_w(&b, blt_cntl);
	ib_w(&b, A6XX_A2D_SRC_COORD(sx));            /* SRC_XMIN */
	ib_w(&b, A6XX_A2D_SRC_COORD(sx + sw - 1));   /* SRC_XMAX */
	ib_w(&b, A6XX_A2D_SRC_COORD(sy));            /* SRC_YMIN */
	ib_w(&b, A6XX_A2D_SRC_COORD(sy + sh - 1));   /* SRC_YMAX */
	ib_w(&b, A6XX_REG_XY(dx, dy));               /* DEST_TL  */
	ib_w(&b, A6XX_REG_XY(dx + dw - 1, dy + dh - 1)); /* DEST_BR */

	/* RB_A2D_BLT_CNTL(0x8c00) + RB_A2D_PIXEL_CNTL(0x8c01).  pixel_cntl = 0
	   (PIXEL_OP_DISABLED) overwrites; A2D_PIXEL_SRC_OVER blends per-pixel. */
	ib_pkt4(&b, REG_A6XX_RB_A2D_BLT_CNTL, 2);
	ib_w(&b, blt_cntl);
	ib_w(&b, pixel_cntl);

	/* SP_A2D_OUTPUT_INFO(0xacc0). */
	ib_pkt4(&b, REG_A6XX_SP_A2D_OUTPUT_INFO, 1);
	ib_w(&b, out_info);

	/* TPL1_A2D_SRC_TEXTURE_INFO(0xb4c0) + SIZE(0xb4c1) + BASE lo/hi
	   (0xb4c2/0xb4c3) + PITCH(0xb4c4): 5 consecutive. */
	ib_pkt4(&b, REG_A6XX_TPL1_A2D_SRC_TEXTURE_INFO, 5);
	ib_w(&b, src_info);
	ib_w(&b, A2D_SRC_SIZE(src_w, src_h));
	ib_w(&b, (uint32_t)(src_phy & 0xffffffffu));
	ib_w(&b, (uint32_t)(src_phy >> 32));
	ib_w(&b, A2D_SRC_PITCH(src_pitch));

	/* Non-UBWC source: emit_blit_src() writes FLAG_BASE(.qword=0) +
	   FLAG_PITCH(0).  FLAG_BASE lo/hi(0xb4ca/0xb4cb) + FLAG_PITCH(0xb4cc)
	   are 3 consecutive registers, all zero. */
	ib_pkt4(&b, REG_A6XX_TPL1_A2D_SRC_TEXTURE_FLAG_BASE, 3);
	ib_w(&b, 0);
	ib_w(&b, 0);
	ib_w(&b, 0);

	/* RB_A2D_DEST_BUFFER_INFO(0x8c17) + BASE lo/hi + PITCH: 4 consecutive. */
	ib_pkt4(&b, REG_A6XX_RB_A2D_DEST_BUFFER_INFO, 4);
	ib_w(&b, A2D_DEST_COLOR_FORMAT(FMT6_8_8_8_8_UNORM) |
	         A2D_DEST_TILE_MODE(TILE6_LINEAR) |
	         A2D_DEST_COLOR_SWAP(A3XX_COLOR_SWAP_WZYX));
	ib_w(&b, (uint32_t)(dst_phy & 0xffffffffu));
	ib_w(&b, (uint32_t)(dst_phy >> 32));
	ib_w(&b, A2D_DEST_PITCH(dst_pitch));

	ib_blit_trigger(&b);

	ret = adreno_ring_submit(adreno_dev(), b.d, b.n);
	if (ret != ADRENO_OK) {
		if (ret != ADRENO_E_WEDGED)
			klog("adreno_2d: copy submit failed (%d)\n", ret);
		return ret;
	}
	return ADRENO_OK;
}

/* Straight copy / scale / rotate (overwrite, no blend). */
int adreno_2d_copy(uint64_t src_phy, int32_t src_w, int32_t src_h,
                   uint32_t src_pitch, int32_t sx, int32_t sy, int32_t sw,
                   int32_t sh, uint64_t dst_phy, int32_t dst_w, int32_t dst_h,
                   uint32_t dst_pitch, int32_t dx, int32_t dy, int32_t dw,
                   int32_t dh, int32_t degree) {
	return a2d_copy_blit(src_phy, src_w, src_h, src_pitch, sx, sy, sw, sh,
	                     dst_phy, dst_w, dst_h, dst_pitch, dx, dy, dw, dh,
	                     degree, 0, 0);
}

/* ------------------------------------------------------------------ */
/* Per-pixel source-over blend (ARGB src composited onto dst).  Same    */
/* emit as adreno_2d_copy but RB_A2D_PIXEL_CNTL = A2D_PIXEL_SRC_OVER so */
/* the A2D blend unit computes out = src*src.a + dst*(1-src.a).  This    */
/* reproduces the EwokOS blt_alpha reference ONLY when the global alpha  */
/* is 0xff: the A2D factors blend by the per-pixel source alpha and      */
/* cannot express the reference's sa = src.a * global_alpha product, so  */
/* the caller must restrict this to global alpha 0xff (gap #6).          */
/* Returns 0 on success, negative on decline/failure.                    */
/* ------------------------------------------------------------------ */
int adreno_2d_blend(uint64_t src_phy, int32_t src_w, int32_t src_h,
                    uint32_t src_pitch, int32_t sx, int32_t sy, int32_t sw,
                    int32_t sh, uint64_t dst_phy, int32_t dst_w, int32_t dst_h,
                    uint32_t dst_pitch, int32_t dx, int32_t dy, int32_t dw,
                    int32_t dh) {
	return a2d_copy_blit(src_phy, src_w, src_h, src_pitch, sx, sy, sw, sh,
	                     dst_phy, dst_w, dst_h, dst_pitch, dx, dy, dw, dh,
	                     0, A2D_PIXEL_SRC_OVER, 0);
}

/* ------------------------------------------------------------------ */
/* Gaussian-style blur via a bilinear scale cascade (mip blur) - gap #7. */
/*                                                                     */
/* The A2D blitter has no convolution op and no kernel-weight register, */
/* so an exact per-tap gaussian needs a shader. This private mip helper */
/* is not exposed through the public Gaussian API. The linear FILTER  */
/* so the blur is built the way raspi's own GPU blur does it           */
/* (downsample -> upsample, vc_g2d half-res path): halve the rect L    */
/* times into a scratch mip chain, then bilinear-upsample the chain    */
/* back into the rect in place.  Repeated 2-tap averaging converges to */
/* a gaussian (CLT), giving a real GPU blur - an approximation of the  */
/* reference kernel, not a bit-exact match.  L ~ log2(radius).         */
/*                                                                     */
/* scratch: `tmp_limit` bytes at tmp_phy (64-aligned, contig).  The     */
/* chain needs sum(align64(wi*4)*hi); the caller passes the g2dd-       */
/* guaranteed tmp size ((h-1)*W*4 + w*4) so we can prove the layout     */
/* fits without knowing tmp's real allocation.  Each level is a         */
/* separate submit (adreno_ring_submit waits on its fence), so the      */
/* chain is strictly ordered.  Returns 0 on success, negative on        */
/* decline/failure.                                                    */
/* ------------------------------------------------------------------ */
#define A2D_BLUR_MAX_LEVELS 5

static uint32_t a2d_align64(uint32_t v) { return (v + 63u) & ~63u; }

int adreno_2d_blur(uint64_t canvas_phy, int32_t W, int32_t H,
                   uint32_t canvas_pitch, int32_t x, int32_t y, int32_t w,
                   int32_t h, int32_t radius, uint64_t tmp_phy,
                   uint64_t tmp_limit) {
	int32_t  lvl_w[A2D_BLUR_MAX_LEVELS + 1];
	int32_t  lvl_h[A2D_BLUR_MAX_LEVELS + 1];
	uint32_t lvl_pitch[A2D_BLUR_MAX_LEVELS + 1];
	uint64_t lvl_off[A2D_BLUR_MAX_LEVELS + 1];
	int L, i, rc;
	uint64_t off, need;

	if (!adreno_2d_ready())
		return -1;

	/* --- eligibility: canvas + scratch GPU-visible, aligned, rect in-bounds --- */
	if (canvas_phy == 0 || tmp_phy == 0)
		return -1;
	if (radius < 1 || w < 2 || h < 2 || W < 2 || H < 2)
		return -1;
	if (x < 0 || y < 0 || x + w > W || y + h > H)
		return -1;
	if ((canvas_phy % A2D_DEST_ALIGN) || (tmp_phy % A2D_DEST_ALIGN))
		return -1;
	if ((canvas_pitch % A2D_PITCH_ALIGN) || canvas_pitch == 0)
		return -1;
	if ((uint32_t)W > A2D_SRC_MAX || (uint32_t)H > A2D_SRC_MAX)
		return -1;
	if ((uint32_t)(x + w - 1) > A2D_XY_MAX || (uint32_t)(y + h - 1) > A2D_XY_MAX)
		return -1;

	/* --- level count from radius (~2^L), at least 1, capped by dims --- */
	L = 1;
	while ((1 << L) < radius && L < A2D_BLUR_MAX_LEVELS)
		L++;
	while (L > 1 && ((w >> L) < 2 || (h >> L) < 2))
		L--;
	if (L < 1)
		return -1;

	/* --- lay out the mip chain in the scratch; prove it fits in tmp_limit --- */
	off = 0;
	for (i = 1; i <= L; i++) {
		int32_t  wi = w >> i; if (wi < 1) wi = 1;
		int32_t  hi = h >> i; if (hi < 1) hi = 1;
		uint32_t p  = a2d_align64((uint32_t)wi * 4u);
		lvl_w[i] = wi; lvl_h[i] = hi; lvl_pitch[i] = p; lvl_off[i] = off;
		off += (uint64_t)p * (uint64_t)hi;
	}
	need = off;
	if (need == 0 || need > tmp_limit)
		return -1;   /* scratch too small: decline, the client CPU-blurs */

	/* --- downsample: canvas rect -> level1 -> ... -> levelL --- */
	{
		uint64_t s_phy   = canvas_phy;
		int32_t  s_w = W, s_h = H, sx = x, sy = y, sw = w, sh = h;
		uint32_t s_pitch = canvas_pitch;
		for (i = 1; i <= L; i++) {
			uint64_t d_phy = tmp_phy + lvl_off[i];
			rc = a2d_copy_blit(s_phy, s_w, s_h, s_pitch, sx, sy, sw, sh,
			                   d_phy, lvl_w[i], lvl_h[i], lvl_pitch[i],
			                   0, 0, lvl_w[i], lvl_h[i], 0, 0, 1);
			if (rc != ADRENO_OK)
				return rc;
			s_phy = d_phy; s_w = lvl_w[i]; s_h = lvl_h[i];
			s_pitch = lvl_pitch[i];
			sx = 0; sy = 0; sw = lvl_w[i]; sh = lvl_h[i];
		}
	}

	/* --- upsample: levelL -> ... -> level1 -> canvas rect (in place) --- */
	for (i = L; i >= 1; i--) {
		uint64_t s_phy = tmp_phy + lvl_off[i];
		uint64_t d_phy;
		int32_t  d_w, d_h, dx, dy, dw, dh;
		uint32_t d_pitch;
		if (i == 1) {
			d_phy = canvas_phy; d_w = W; d_h = H; d_pitch = canvas_pitch;
			dx = x; dy = y; dw = w; dh = h;
		} else {
			d_phy = tmp_phy + lvl_off[i - 1];
			d_w = lvl_w[i - 1]; d_h = lvl_h[i - 1]; d_pitch = lvl_pitch[i - 1];
			dx = 0; dy = 0; dw = lvl_w[i - 1]; dh = lvl_h[i - 1];
		}
		rc = a2d_copy_blit(s_phy, lvl_w[i], lvl_h[i], lvl_pitch[i],
		                   0, 0, lvl_w[i], lvl_h[i],
		                   d_phy, d_w, d_h, d_pitch,
		                   dx, dy, dw, dh, 0, 0, 1);
		if (rc != ADRENO_OK)
			return rc;
	}

	return ADRENO_OK;
}
