#ifndef GA10B_CTX_H
#define GA10B_CTX_H

/*
 * ctxsw header / RAMIN-GR / patch-buffer encoders -- pure logic,
 * host-unit-testable.
 *
 * VERIFIED against nvgpu rel-36:
 *   hw_ctxsw_prog_ga10b.h          byte offsets (all /4 = word index here)
 *   ctxsw_prog_gm20b.c             access map addr = RAW lo/hi32 of GPU VA;
 *                                  verif = misc_options bit3 cleared
 *   ctxsw_prog_gp10b.c + gm20b     init_ctxsw_hdr_data = zero the 4 counters
 *   ctxsw_prog_gv11b               context_buffer_ptr = RAW lo/hi32,
 *                                  ctl type_per_veid_header = 0x21
 *   common/gr/subctx.c             load_ctx_header field set (golden + chan)
 *   common/gr/ctx.c                load_golden_ctx_image per-channel patches
 *   hw_ram_ga10b.h + ramin_gv11b_fusa.c  RAMIN words 128..171
 *
 * Two header surfaces exist per context:
 *  - the MAIN image header at offset 0 of the ctx buffer itself (FECS
 *    writes the magic values during GOLDEN_IMAGE_SAVE);
 *  - the per-veid HEADER PAGE (768B) that RAMIN engine_wfi_target points
 *    at in subctx mode (ga10b has NVGPU_SUPPORT_TSG_SUBCONTEXTS).
 */

#include <stdint.h>
#include "ga10b_fifo.h"   /* ga10b_ramin_pdb_lo */

#define GA10B_CTXSW_HDR_BYTES   0x300u   /* fecs_header_size_in_bytes */
#define GA10B_CTXSW_HDR_WORDS   (GA10B_CTXSW_HDR_BYTES / 4u)

/* main-image header word indices (byte offset / 4) */
#define GA10B_CTX_W_CTL              3u    /* 0x0c */
#define GA10B_CTX_W_PATCH_COUNT      4u    /* 0x10 */
#define GA10B_CTX_W_PATCH_ADR_LO     5u    /* 0x14 */
#define GA10B_CTX_W_PATCH_ADR_HI     6u    /* 0x18 */
#define GA10B_CTX_W_PM               10u   /* 0x28 mode[2:0] smpc[5:3] */
#define GA10B_CTX_W_PM_PTR           11u   /* 0x2c */
#define GA10B_CTX_W_MISC_OPTIONS     15u   /* 0x3c verif bit3 */
#define GA10B_CTX_W_CTX_BUF_PTR_HI   30u   /* 0x78 */
#define GA10B_CTX_W_CTX_BUF_PTR      31u   /* 0x7c */
#define GA10B_CTX_W_COMPUTE_PREEMPT  33u   /* 0x84 cta=1 cilp=2 */
#define GA10B_CTX_W_PM_PTR_HI        37u   /* 0x94 */
#define GA10B_CTX_W_ACCESS_MAP_CFG   40u   /* 0xa0 allow_all=0 use_map=2 */
#define GA10B_CTX_W_ACCESS_MAP_LO    41u   /* 0xa4 */
#define GA10B_CTX_W_ACCESS_MAP_HI    42u   /* 0xa8 */
#define GA10B_CTX_W_NUM_WFI_SAVE     52u   /* 0xd0 */
#define GA10B_CTX_W_NUM_CTA_SAVE     53u   /* 0xd4 */
#define GA10B_CTX_W_NUM_SAVE         61u   /* 0xf4 */
#define GA10B_CTX_W_NUM_RESTORE      62u   /* 0xf8 */

#define GA10B_CTX_CTL_PER_VEID       0x21u
#define GA10B_CTX_ACCESS_MAP_USE_MAP 2u
#define GA10B_CTX_COMPUTE_PREEMPT_CTA 1u

/* per-veid header page (subctx.c load_ctx_header + gp10b hdr init).
 * Used for BOTH the golden context and every channel header page.
 * Caller zeroes the 768B first.  pm mode stays no_ctxsw (0), ptr 0. */
static inline void ga10b_ctxsw_veid_hdr(uint32_t* h, uint64_t ctx_va,
                                        uint64_t patch_va,
                                        uint64_t map_va) {
    h[GA10B_CTX_W_CTL]            = GA10B_CTX_CTL_PER_VEID;
    h[GA10B_CTX_W_PATCH_COUNT]    = 0;
    h[GA10B_CTX_W_PATCH_ADR_LO]   = (uint32_t)patch_va;
    h[GA10B_CTX_W_PATCH_ADR_HI]   = (uint32_t)(patch_va >> 32);
    h[GA10B_CTX_W_PM]             = 0;
    h[GA10B_CTX_W_PM_PTR]         = 0;
    h[GA10B_CTX_W_PM_PTR_HI]      = 0;
    h[GA10B_CTX_W_CTX_BUF_PTR_HI] = (uint32_t)(ctx_va >> 32);
    h[GA10B_CTX_W_CTX_BUF_PTR]    = (uint32_t)ctx_va;
    h[GA10B_CTX_W_ACCESS_MAP_LO]  = (uint32_t)map_va;
    h[GA10B_CTX_W_ACCESS_MAP_HI]  = (uint32_t)(map_va >> 32);
    h[GA10B_CTX_W_NUM_WFI_SAVE]   = 0;
    h[GA10B_CTX_W_NUM_CTA_SAVE]   = 0;
    h[GA10B_CTX_W_NUM_SAVE]       = 0;
    h[GA10B_CTX_W_NUM_RESTORE]    = 0;
}

/* per-channel patches applied to the MAIN header after memcpy of the
 * local golden image (ctx.c nvgpu_gr_ctx_load_golden_ctx_image):
 * counters re-zeroed, priv access map switched to use_map + channel VA,
 * verif disabled, patch count/addr, pm mode no_ctxsw. */
static inline void ga10b_ctxsw_main_patch(uint32_t* c, uint64_t patch_va,
                                          uint64_t map_va,
                                          uint32_t patch_count) {
    c[GA10B_CTX_W_NUM_WFI_SAVE]   = 0;
    c[GA10B_CTX_W_NUM_CTA_SAVE]   = 0;
    c[GA10B_CTX_W_NUM_SAVE]       = 0;
    c[GA10B_CTX_W_NUM_RESTORE]    = 0;
    c[GA10B_CTX_W_ACCESS_MAP_CFG] = GA10B_CTX_ACCESS_MAP_USE_MAP;
    c[GA10B_CTX_W_ACCESS_MAP_LO]  = (uint32_t)map_va;
    c[GA10B_CTX_W_ACCESS_MAP_HI]  = (uint32_t)(map_va >> 32);
    c[GA10B_CTX_W_MISC_OPTIONS]  &= ~(1u << 3);
    c[GA10B_CTX_W_PATCH_COUNT]    = patch_count;
    c[GA10B_CTX_W_PATCH_ADR_LO]   = (uint32_t)patch_va;
    c[GA10B_CTX_W_PATCH_ADR_HI]   = (uint32_t)(patch_va >> 32);
    c[GA10B_CTX_W_PM]             = 0;
    c[GA10B_CTX_W_PM_PTR]         = 0;
    c[GA10B_CTX_W_PM_PTR_HI]      = 0;
    /* default compute preemption = CTA (obj_ctx set_compute_preemption_mode) */
    c[GA10B_CTX_W_COMPUTE_PREEMPT] = GA10B_CTX_COMPUTE_PREEMPT_CTA;
}

/* RAMIN GR setup for subctx mode (golden inst block + channel inst block).
 * w128/129 main pdb (same encoding as fifo channels), w131 fw magic,
 * w132/133 engine_wfi_target -> header page GPU VA (virtual mode),
 * w134 veid 0, w166/167 subctx valid mask (only veid0), w168..171 subctx0
 * pdb map entry = same page directory as the main pdb (obj_ctx.c
 * prepare_inst_block passes vm->pdb.mem), replayable=false. */
static inline void ga10b_ramin_gr(uint32_t* w, uint64_t pdb_pa,
                                  uint64_t hdr_va) {
    uint32_t pdb_lo = ga10b_ramin_pdb_lo(pdb_pa);
    w[GA10B_RAMIN_PDB_LO]   = pdb_lo;
    w[GA10B_RAMIN_PDB_HI]   = (uint32_t)(pdb_pa >> 32);
    w[GA10B_RAMIN_FW_MAGIC] = GA10B_RAMIN_FW_MAGIC_VAL;
    w[132] = (1u << 2) |
             (uint32_t)(((hdr_va >> 12) & 0xfffffu) << 12);
    w[133] = (uint32_t)(hdr_va >> 32) & 0xffu;
    w[GA10B_RAMIN_WFI_VEID] = 0;
    w[166] = 1u;                 /* sc_pdb_valid_long_w(0), veid0 only */
    w[167] = 0;
    w[168] = pdb_lo;             /* sc_page_dir_base_vol_w(0) */
    w[169] = (uint32_t)(pdb_pa >> 32);
    w[170] = 0;
    w[171] = 0;
}

/* patch buffer: {addr, data} pairs, 512 slots per 4KB page
 * (gm20b get_patch_slots = PATCH_CTX_SLOTS_PER_PAGE = 4096/8) */
#define GA10B_PATCH_SLOTS  512u
static inline int ga10b_patch_add(uint32_t* p, uint32_t* count,
                                  uint32_t addr, uint32_t data) {
    if (*count >= GA10B_PATCH_SLOTS)
        return -1;
    p[2u * (*count)]      = addr;
    p[2u * (*count) + 1u] = data;
    (*count)++;
    return 0;
}

#endif