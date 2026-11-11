/* Copyright (C) 2020 Google, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <util/bitset.h>
#include <util/log.h>



#define BITMASK_WORDS BITSET_WORDS(64)

typedef struct {
    BITSET_WORD bitset[BITMASK_WORDS];
} bitmask_t;

static inline uint64_t
bitmask_to_uint64_t(bitmask_t mask)
{
    return ((uint64_t)mask.bitset[1] << 32) | mask.bitset[0];
}

static inline bitmask_t
uint64_t_to_bitmask(uint64_t val)
{
    bitmask_t mask = {
        .bitset[0] = val & 0xffffffff,
        .bitset[1] = (val >> 32) & 0xffffffff,
    };

    return mask;
}

static inline void
store_instruction(BITSET_WORD *dst, bitmask_t instr)
{
    *(dst + 0) = instr.bitset[0];
    *(dst + 1) = instr.bitset[1];
}

/**
 * Opaque type from the PoV of generated code, but allows state to be passed
 * thru to the hand written helpers used by the generated code.
 */
struct encode_state;

/**
 * Allows to use gpu_id in expr functions
 */
#define ISA_GPU_ID() s->gen

struct bitset_params;

static bitmask_t
pack_field(unsigned low, unsigned high, int64_t val, bool is_signed)
{
   bitmask_t field, mask;

   if (is_signed) {
      /* NOTE: Don't assume val is already sign-extended to 64b,
       * just check that the bits above the valid range are either
       * all zero or all one:
       */
      assert(!(( val & ~BITFIELD64_MASK(1 + high - low)) &&
               (~val & ~BITFIELD64_MASK(1 + high - low))));
   } else {
      assert(!(val & ~BITFIELD64_MASK(1 + high - low)));
   }

   BITSET_ZERO(field.bitset);

   if (!val)
      return field;

   BITSET_ZERO(mask.bitset);
   BITSET_SET_RANGE(mask.bitset, 0, high - low);

   field = uint64_t_to_bitmask(val);
   BITSET_AND(field.bitset, field.bitset, mask.bitset);
   BITSET_SHL(field.bitset, low);

   return field;
}

/*
 * Forward-declarations (so we don't have to figure out which order to
 * emit various encoders when they have reference each other)
 */

static bitmask_t encode__reg_gpr(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static bitmask_t encode__reg_const(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static bitmask_t encode__reg_relative_gpr(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static bitmask_t encode__reg_relative_const(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static bitmask_t encode__multisrc(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static bitmask_t encode__cat1_dst(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static bitmask_t encode__cat1_immed_src(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static bitmask_t encode__cat1_const_src(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static bitmask_t encode__cat1_gpr_src(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static bitmask_t encode__cat1_relative_gpr_src(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static bitmask_t encode__cat1_relative_const_src(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static bitmask_t encode__cat1_multi_src(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static bitmask_t encode__cat1_multi_dst(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static bitmask_t encode__dst_gpr_cat2(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static bitmask_t encode__cat3_src(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static bitmask_t encode__cat5_s2en_bindless_base(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static bitmask_t encode__cat5_src1(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static bitmask_t encode__cat5_src2(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static bitmask_t encode__cat5_samp(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static bitmask_t encode__cat5_samp_s2en_bindless_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static bitmask_t encode__cat5_tex_s2en_bindless_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static bitmask_t encode__cat5_tex(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static bitmask_t encode__cat5_tex_s2en_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static bitmask_t encode__cat5_type(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static bitmask_t encode__cat5_src3(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static bitmask_t encode__const_dst(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static bitmask_t encode__cat6_typed(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static bitmask_t encode__cat6_base(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static bitmask_t encode__cat6_src(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static bitmask_t encode__cat6_src_sp(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static bitmask_t encode__cat6_src_load_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static bitmask_t encode__cat6_src_const_or_gpr(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static bitmask_t encode__alias_immed_src(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static bitmask_t encode__alias_const_src(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static bitmask_t encode__alias_gpr_src(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static bitmask_t encode__dst_rt(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static bitmask_t encode__instruction(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);


/*
 * Expression evaluators:
 */

struct bitset_params {
   int64_t HAS_SAMP;
   int64_t HAS_TEX;
   int64_t LAST;
   int64_t SRC_R;
   int64_t HALF;
   int64_t SRC_TYPE;
   int64_t DST_REL;
   int64_t DST_HALF;
   int64_t FULL;
   int64_t IMMED_ENCODING;
   int64_t BINDLESS;
   int64_t BASE_HI;
   int64_t HAS_TYPE;
   int64_t NUM_SRC;
   int64_t O;
   int64_t CLP;
   int64_t SRC2_IMM_OFFSET;
   int64_t DESC_MODE;
   int64_t SRC_CONST;
   int64_t LOAD_SIZE_IM;
   int64_t SRC_IM;
   int64_t SH;
   int64_t TYPE_SIZE;
   int64_t TYPE;
};






static inline int64_t __reg_gpr_expr___reg_gpr_a0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __reg_gpr_expr___reg_gpr_p0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __reg_relative_gpr_expr___offset_zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __reg_relative_const_expr___offset_zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __multisrc_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __multisrc_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __multisrc_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __multisrc_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __multisrc_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __cat1_dst_expr_anon_1(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __cat1_dst_expr_anon_2(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __cat1_immed_src_expr_anon_10(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __cat1_immed_src_expr_anon_11(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __cat1_immed_src_expr_anon_12(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __cat1_immed_src_expr_anon_13(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __cat1_immed_src_expr_anon_14(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __dst_gpr_cat2_expr___reg_gpr_a0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __dst_gpr_cat2_expr___reg_gpr_p0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __cat3_src_expr_anon_16(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __cat5_s2en_bindless_base_expr_anon_18(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __cat5_s2en_bindless_base_expr_anon_17(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __cat5_src1_expr_anon_21(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __cat5_src2_expr_anon_22(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __cat5_src2_expr_anon_23(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __cat5_samp_expr_anon_24(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __cat5_samp_s2en_bindless_a1_expr_anon_25(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __cat5_tex_s2en_bindless_a1_expr_anon_26(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __cat5_tex_expr_anon_27(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __cat5_tex_s2en_bindless_expr_anon_28(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __cat5_type_expr_anon_29(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __cat5_src3_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __cat5_src3_expr_anon_30(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __cat5_src3_expr___cat5_s2enb_uses_a1_gen6(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __cat5_src3_expr___cat5_s2enb_uses_a1_gen7(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __cat6_typed_expr_anon_43(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __cat6_base_expr_anon_44(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __cat6_src_expr_anon_45(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __cat6_src_sp_expr_anon_46(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __cat6_src_load_size_expr_anon_47(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __cat6_src_load_size_expr___cat6_load_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __cat6_src_const_or_gpr_expr_anon_48(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __alias_immed_src_expr_anon_49(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __alias_immed_src_expr_anon_50(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __alias_immed_src_expr_anon_51(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __alias_const_src_expr_anon_52(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __alias_gpr_src_expr_anon_53(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src);
static inline int64_t __instruction_expr_anon_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_9(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_5(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_3(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_4(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_9(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_5(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_3(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_4(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_6(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_7(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_8(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_5(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_3(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_4(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_6(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_7(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_8(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_5(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_3(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_4(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_6(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_7(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_8(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_5(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_3(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_4(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_6(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_7(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_8(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_5(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_3(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_4(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_6(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_7(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_8(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_5(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_3(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_4(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_6(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_7(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_8(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_5(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_3(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_4(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_6(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_7(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_8(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_5(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_3(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_4(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_3(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_4(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_3(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_4(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_3(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_4(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_15(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___wmm_dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___wmm_dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___one(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___one(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___two(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___two(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___two(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___one(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___one(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___two(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___two(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___one(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___one(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___two(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___two(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___one(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___one(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___one(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___one(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___one(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___one(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___one(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___one(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___one(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___one(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___one(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___one(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___one(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___one(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___one(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_20(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___one(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___two(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___one(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___one(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___one(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_31(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_32(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_33(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_shift(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_full_shift(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_34(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_35(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_36(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_37(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_shift(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_full_shift(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_38(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_38(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_38(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_39(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_load_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_40(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_direct(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_40(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_direct(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_direct(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_direct(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_42(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_41(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_direct(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_42(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_41(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_direct(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_direct(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_direct(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_direct(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_direct(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_direct(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_direct(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_direct(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_direct(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_direct(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_56(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_57(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_58(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_54(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);
static inline int64_t __instruction_expr_anon_55(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src);



          
static inline int64_t
__reg_gpr_expr___reg_gpr_a0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t GPR;

          GPR = src->num >> 2;
    return GPR == 61 /* a0.* */;
}


          
static inline int64_t
__reg_gpr_expr___reg_gpr_p0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t GPR;

          GPR = src->num >> 2;
    return GPR == 62 /* p0.x */;
}




          
static inline int64_t
__reg_relative_gpr_expr___offset_zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t OFFSET;

          OFFSET = src->array.offset;
    return OFFSET == 0;
}



          
static inline int64_t
__reg_relative_const_expr___offset_zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t OFFSET;

          OFFSET = src->array.offset;
    return OFFSET == 0;
}



          
static inline int64_t
__multisrc_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t FULL;

          FULL = p->FULL;
    return !FULL;
}



          
static inline int64_t
__cat1_dst_expr_anon_1(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t OFFSET;
    int64_t DST_REL;

          OFFSET = src->array.offset;

          DST_REL = p->DST_REL;
    return (OFFSET == 0) && DST_REL;
}


          
static inline int64_t
__cat1_dst_expr_anon_2(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t DST_REL;

          DST_REL = p->DST_REL;
    return DST_REL;
}



          
static inline int64_t
__cat1_immed_src_expr_anon_10(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t SRC_TYPE;

          SRC_TYPE = p->SRC_TYPE;
    return SRC_TYPE == 0 /* f16 */;
}


          
static inline int64_t
__cat1_immed_src_expr_anon_11(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t SRC_TYPE;

          SRC_TYPE = p->SRC_TYPE;
    return SRC_TYPE == 1 /* f32 */;
}


          
static inline int64_t
__cat1_immed_src_expr_anon_12(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t SRC_TYPE;
    int64_t IMMED;

          SRC_TYPE = p->SRC_TYPE;

          IMMED = extract_reg_uim(src);
    return (SRC_TYPE == 3 /* u32 */) && (IMMED > 0x1000);
}


          
static inline int64_t
__cat1_immed_src_expr_anon_13(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t SRC_TYPE;

          SRC_TYPE = p->SRC_TYPE;
    return SRC_TYPE == 4 /* s16 */;
}


          
static inline int64_t
__cat1_immed_src_expr_anon_14(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t SRC_TYPE;

          SRC_TYPE = p->SRC_TYPE;
    return SRC_TYPE == 5 /* s32 */;
}









          
static inline int64_t
__dst_gpr_cat2_expr___reg_gpr_a0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t GPR;

          GPR = extract_dst_num(src);
    return GPR == 61 /* a0.* */;
}


          
static inline int64_t
__dst_gpr_cat2_expr___reg_gpr_p0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t GPR;

          GPR = extract_dst_num(src);
    return GPR == 62 /* p0.x */;
}



          
static inline int64_t
__cat3_src_expr_anon_16(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t IMMED_ENCODING;

          IMMED_ENCODING = p->IMMED_ENCODING;
    return IMMED_ENCODING;
}



          
static inline int64_t
__cat5_s2en_bindless_base_expr_anon_18(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t BINDLESS;

          BINDLESS = p->BINDLESS;
    return BINDLESS;
}


          
static inline int64_t
__cat5_s2en_bindless_base_expr_anon_17(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t BASE_LO;
    int64_t BASE_HI;

          BASE_LO = src->cat5.tex_base & 0x1;

          BASE_HI = p->BASE_HI;
    return (BASE_HI * 2) | BASE_LO;
}



          
static inline int64_t
__cat5_src1_expr_anon_21(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t NUM_SRC;

          NUM_SRC = p->NUM_SRC;
    return NUM_SRC > 0;
}



          
static inline int64_t
__cat5_src2_expr_anon_22(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t NUM_SRC;
    int64_t O;
    int64_t CLP;

          NUM_SRC = p->NUM_SRC;

          O = p->O;

          CLP = p->CLP;
    return O || CLP || (NUM_SRC > 1);
}


          
static inline int64_t
__cat5_src2_expr_anon_23(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t SRC2_IMM_OFFSET;

          SRC2_IMM_OFFSET = p->SRC2_IMM_OFFSET;
    return SRC2_IMM_OFFSET;
}



          
static inline int64_t
__cat5_samp_expr_anon_24(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t HAS_SAMP;

          HAS_SAMP = p->HAS_SAMP;
    return HAS_SAMP;
}



          
static inline int64_t
__cat5_samp_s2en_bindless_a1_expr_anon_25(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t HAS_SAMP;

          HAS_SAMP = p->HAS_SAMP;
    return HAS_SAMP;
}



          
static inline int64_t
__cat5_tex_s2en_bindless_a1_expr_anon_26(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t HAS_TEX;

          HAS_TEX = p->HAS_TEX;
    return HAS_TEX;
}



          
static inline int64_t
__cat5_tex_expr_anon_27(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t HAS_TEX;

          HAS_TEX = p->HAS_TEX;
    return HAS_TEX;
}



          
static inline int64_t
__cat5_tex_s2en_bindless_expr_anon_28(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t HAS_TEX;

          HAS_TEX = p->HAS_TEX;
    return HAS_TEX;
}



          
static inline int64_t
__cat5_type_expr_anon_29(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t HAS_TYPE;
    int64_t TYPE;

          HAS_TYPE = p->HAS_TYPE;

          TYPE = (src->flags & IR3_INSTR_RCK) ? 7 : src->cat5.type;
    return HAS_TYPE && TYPE != 7;
}



          
static inline int64_t
__cat5_src3_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t DESC_MODE;

          DESC_MODE = p->DESC_MODE;
    return DESC_MODE < 6  /* CAT5_BINDLESS_IMM */;
}


          
static inline int64_t
__cat5_src3_expr_anon_30(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t BINDLESS;

          BINDLESS = p->BINDLESS;
    return !BINDLESS;
}


          
static inline int64_t
__cat5_src3_expr___cat5_s2enb_uses_a1_gen6(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t DESC_MODE;

          DESC_MODE = p->DESC_MODE;
    return ISA_GPU_ID() >= 600 && ISA_GPU_ID() < 700 &&
	((DESC_MODE == 1) /* CAT5_BINDLESS_A1_UNIFORM */ ||
	 (DESC_MODE == 3) /* CAT5_BINDLESS_A1_NONUNIFORM */ ||
	 (DESC_MODE == 7))/* CAT5_BINDLESS_A1_IMM */;
}


          
static inline int64_t
__cat5_src3_expr___cat5_s2enb_uses_a1_gen7(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t DESC_MODE;

          DESC_MODE = p->DESC_MODE;
    return ISA_GPU_ID() >= 700 &&
	((DESC_MODE == 1) /* CAT5_BINDLESS_A1_UNIFORM */ ||
	 (DESC_MODE == 3) /* CAT5_BINDLESS_A1_NONUNIFORM */ ||
	 (DESC_MODE == 7))/* CAT5_BINDLESS_A1_IMM */;
}




          
static inline int64_t
__cat6_typed_expr_anon_43(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t TYPED;

          TYPED = src->cat6.typed;
    return TYPED;
}



          
static inline int64_t
__cat6_base_expr_anon_44(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t BINDLESS;

          BINDLESS = p->BINDLESS;
    return BINDLESS;
}



          
static inline int64_t
__cat6_src_expr_anon_45(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t SRC_IM;

          SRC_IM = p->SRC_IM;
    return SRC_IM;
}



          
static inline int64_t
__cat6_src_sp_expr_anon_46(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t SRC_IM;

          SRC_IM = p->SRC_IM;
    return SRC_IM;
}



          
static inline int64_t
__cat6_src_load_size_expr_anon_47(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t LOAD_SIZE_IM;

          LOAD_SIZE_IM = p->LOAD_SIZE_IM;
    return LOAD_SIZE_IM;
}


          
static inline int64_t
__cat6_src_load_size_expr___cat6_load_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t LOAD_SIZE_MINUS_ONE;

          LOAD_SIZE_MINUS_ONE = extract_reg_uim(src) - 1;
    return LOAD_SIZE_MINUS_ONE + 1;
}



          
static inline int64_t
__cat6_src_const_or_gpr_expr_anon_48(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t SRC_CONST;

          SRC_CONST = p->SRC_CONST;
    return SRC_CONST;
}



          
static inline int64_t
__alias_immed_src_expr_anon_49(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t TYPE;
    int64_t TYPE_SIZE;

          TYPE = p->TYPE;

          TYPE_SIZE = p->TYPE_SIZE;
    return TYPE == 0 && TYPE_SIZE == 0 /* f16 */;
}


          
static inline int64_t
__alias_immed_src_expr_anon_50(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t TYPE;
    int64_t TYPE_SIZE;

          TYPE = p->TYPE;

          TYPE_SIZE = p->TYPE_SIZE;
    return TYPE == 0 && TYPE_SIZE == 1 /* f32 */;
}


          
static inline int64_t
__alias_immed_src_expr_anon_51(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t TYPE_SIZE;

          TYPE_SIZE = p->TYPE_SIZE;
    return TYPE_SIZE == 0 /* b16 */;
}



          
static inline int64_t
__alias_const_src_expr_anon_52(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t TYPE_SIZE;

          TYPE_SIZE = p->TYPE_SIZE;
    return (TYPE_SIZE == 0) /* b16 */;
}



          
static inline int64_t
__alias_gpr_src_expr_anon_53(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
    int64_t TYPE_SIZE;

          TYPE_SIZE = p->TYPE_SIZE;
    return (TYPE_SIZE == 0) /* b16 */;
}




          
static inline int64_t
__instruction_expr_anon_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t W;

          W = util_logbase2(8) - 1;
    return 2ULL << W;
}


          
static inline int64_t
__instruction_expr_anon_9(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t SRC_TYPE;
    int64_t DST_TYPE;

          SRC_TYPE = src->cat1.src_type;

          DST_TYPE = src->cat1.dst_type;
    return (SRC_TYPE == 4 /* s16 */) && (DST_TYPE == 4);
}


          
static inline int64_t
__instruction_expr_anon_5(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t SRC_R;

          SRC_R = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U);
    return SRC_R;
}


          
static inline int64_t
__instruction_expr_anon_3(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t SRC_TYPE;

          SRC_TYPE = src->cat1.src_type;
    return (SRC_TYPE == 0) /* f16 */ ||
			(SRC_TYPE == 2) /* u16 */ ||
			(SRC_TYPE == 4) /* s16 */ ||
			(SRC_TYPE == 6) /* u8 */  ||
			(SRC_TYPE == 7) /* s8 */;
}


          
static inline int64_t
__instruction_expr_anon_4(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t DST_TYPE;

          DST_TYPE = src->cat1.dst_type;
    return (DST_TYPE == 0) /* f16 */ ||
			(DST_TYPE == 2) /* u16 */ ||
			(DST_TYPE == 4) /* s16 */ ||
			(DST_TYPE == 6) /* u8 */  ||
			(DST_TYPE == 7) /* s8 */;
}


          
static inline int64_t
__instruction_expr___false(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    return 0;
}


          
static inline int64_t
__instruction_expr_anon_6(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t SRC_TYPE;
    int64_t DST;
    int64_t DST_TYPE;

          SRC_TYPE = src->cat1.src_type;

          { 
 struct bitset_params bp = {
    .DST_REL = !!(src->dsts[0]->flags & IR3_REG_RELATIV),  /* DST_REL */
 };

          const bitmask_t tmp = encode__cat1_dst(s, &bp, src->dsts[0]);
          DST = bitmask_to_uint64_t(tmp);
          }

          DST_TYPE = src->cat1.dst_type;
    return (DST == 0xf4 /* a0.x */) && (SRC_TYPE == 4 /* s16 */) && (DST_TYPE == 4);
}


          
static inline int64_t
__instruction_expr_anon_7(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t SRC_TYPE;
    int64_t DST;
    int64_t DST_TYPE;

          SRC_TYPE = src->cat1.src_type;

          { 
 struct bitset_params bp = {
    .DST_REL = !!(src->dsts[0]->flags & IR3_REG_RELATIV),  /* DST_REL */
 };

          const bitmask_t tmp = encode__cat1_dst(s, &bp, src->dsts[0]);
          DST = bitmask_to_uint64_t(tmp);
          }

          DST_TYPE = src->cat1.dst_type;
    return (DST == 0xf5 /* a0.y */) && (SRC_TYPE == 2 /* u16 */) && (DST_TYPE == 2);
}


          
static inline int64_t
__instruction_expr_anon_8(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t SRC_TYPE;
    int64_t DST_TYPE;

          SRC_TYPE = src->cat1.src_type;

          DST_TYPE = src->cat1.dst_type;
    return SRC_TYPE != DST_TYPE;
}


          
static inline int64_t
__instruction_expr_anon_15(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t REPEAT;

          REPEAT = src->repeat;
    return (REPEAT + 1) * 32;
}


          
static inline int64_t
__instruction_expr___cat2_cat3_nop_encoding(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t SRC1_R;
    int64_t SRC2_R;
    int64_t REPEAT;

          SRC1_R = extract_SRC1_R(src);

          SRC2_R = extract_SRC2_R(src);

          REPEAT = src->repeat;
    return ((SRC1_R != 0) || (SRC2_R != 0)) && (REPEAT == 0);
}


          
static inline int64_t
__instruction_expr___cat2_cat3_nop_value(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t SRC1_R;
    int64_t SRC2_R;

          SRC1_R = extract_SRC1_R(src);

          SRC2_R = extract_SRC2_R(src);
    return SRC1_R | (SRC2_R << 1);
}


          
static inline int64_t
__instruction_expr___dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t FULL;
    int64_t DST;
    int64_t DST_CONV;

          FULL = !(src->srcs[0]->flags & IR3_REG_HALF);

          { 
 struct bitset_params bp = {
 };

          const bitmask_t tmp = encode__dst_gpr_cat2(s, &bp, src->dsts[0]);
          DST = bitmask_to_uint64_t(tmp);
          }

          DST_CONV = ((src->dsts[0]->num >> 2) == 62) ? 0 :
			!!((src->srcs[0]->flags ^ src->dsts[0]->flags) & IR3_REG_HALF);
    return (FULL == DST_CONV) && (DST <= 0xf7 /* p0.x */);
}


          
static inline int64_t
__instruction_expr___zero(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    return 0;
}


          
static inline int64_t
__instruction_expr___multisrc_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t FULL;

          FULL = __instruction_expr___false(s, p, src);
    return !FULL;
}


          
static inline int64_t
__instruction_expr___true(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    return 1;
}


          
static inline int64_t
__instruction_expr___wmm_dest_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t DST_FULL;

          DST_FULL = ((src->dsts[0]->num >> 2) == 62) ? 1 :
			!(src->dsts[0]->flags & IR3_REG_HALF);
    return (!DST_FULL);
}


          
static inline int64_t
__instruction_expr___one(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    return 1;
}


          
static inline int64_t
__instruction_expr_anon_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t S2EN_BINDLESS;

          S2EN_BINDLESS = !!(src->flags & (IR3_INSTR_S2EN | IR3_INSTR_B));
    return S2EN_BINDLESS;
}


          
static inline int64_t
__instruction_expr___cat5_s2enb_is_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t DESC_MODE;

          DESC_MODE = extract_cat5_DESC_MODE(src);
    return (DESC_MODE == 1) /* CAT5_BINDLESS_A1_UNIFORM */ ||
	(DESC_MODE == 2) /* CAT5_BINDLESS_NONUNIFORM */ ||
	(DESC_MODE == 3) /* CAT5_BINDLESS_A1_NONUNIFORM */ ||
	(DESC_MODE == 5) /* CAT5_BINDLESS_UNIFORM */ ||
	(DESC_MODE == 6) /* CAT5_BINDLESS_IMM */ ||
	(DESC_MODE == 7) /* CAT5_BINDLESS_A1_IMM */;
}


          
static inline int64_t
__instruction_expr___cat5_s2enb_is_indirect(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t DESC_MODE;

          DESC_MODE = extract_cat5_DESC_MODE(src);
    return DESC_MODE < 6  /* CAT5_BINDLESS_IMM */;
}


          
static inline int64_t
__instruction_expr___cat5_s2enb_is_uniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t DESC_MODE;

          DESC_MODE = extract_cat5_DESC_MODE(src);
    return (DESC_MODE == 0) /* CAT5_UNIFORM */ ||
	(DESC_MODE == 1) /* CAT5_BINDLESS_A1_UNIFORM */ ||
	(DESC_MODE == 5) /* CAT5_BINDLESS_UNIFORM */;
}


          
static inline int64_t
__instruction_expr___cat5_s2enb_is_nonuniform(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t DESC_MODE;

          DESC_MODE = extract_cat5_DESC_MODE(src);
    return (DESC_MODE == 2) /* CAT5_BINDLESS_NONUNIFORM */ ||
	(DESC_MODE == 3) /* CAT5_BINDLESS_A1_NONUNIFORM */ ||
	(DESC_MODE == 4) /* CAT5_NONUNIFORM */;
}


          
static inline int64_t
__instruction_expr___cat5_s2enb_uses_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t DESC_MODE;

          DESC_MODE = extract_cat5_DESC_MODE(src);
    return (DESC_MODE == 1) /* CAT5_BINDLESS_A1_UNIFORM */ ||
	(DESC_MODE == 3) /* CAT5_BINDLESS_A1_NONUNIFORM */ ||
	(DESC_MODE == 7) /* CAT5_BINDLESS_A1_IMM */;
}


          
static inline int64_t
__instruction_expr___type_half(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t TYPE;

          { 
 struct bitset_params bp = {
    .HAS_TYPE = __instruction_expr___true(s, p, src),  /* HAS_TYPE */
 };

          const bitmask_t tmp = encode__cat5_type(s, &bp, src);
          TYPE = bitmask_to_uint64_t(tmp);
          }
    return (TYPE == 0) /* f16 */ ||
	(TYPE == 2) /* u16 */ ||
	(TYPE == 4) /* s16 */ ||
	(TYPE == 6) /* u8 */;
}


          
static inline int64_t
__instruction_expr___cat5_is_rck(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t TYPE;

          { 
 struct bitset_params bp = {
    .HAS_TYPE = __instruction_expr___true(s, p, src),  /* HAS_TYPE */
 };

          const bitmask_t tmp = encode__cat5_type(s, &bp, src);
          TYPE = bitmask_to_uint64_t(tmp);
          }
    return TYPE == 7;
}


          
static inline int64_t
__instruction_expr___two(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    return 2;
}


          
static inline int64_t
__instruction_expr_anon_20(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t W;

          W = util_logbase2(src->cat5.cluster_size) - 1;
    return 2 << W;
}


          
static inline int64_t
__instruction_expr_anon_31(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t TYPE_SHIFT;

          TYPE_SHIFT = __instruction_expr___cat6_type_shift(s, p, src);
    return TYPE_SHIFT == 0;
}


          
static inline int64_t
__instruction_expr_anon_32(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t OFF;

          OFF = extract_reg_uim(src->srcs[3]);
    return OFF == 0;
}


          
static inline int64_t
__instruction_expr_anon_33(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t SRC2_SHIFT;

          SRC2_SHIFT = extract_reg_uim(src->srcs[2]);
    return SRC2_SHIFT == 0;
}


          
static inline int64_t
__instruction_expr___cat6_type_shift(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t TYPE;
    int64_t TYPE_HALF;

          TYPE = src->cat6.type;

          TYPE_HALF = __instruction_expr___type_half(s, p, src);
    return TYPE >= 6 ? 0 /* u8 */ : (TYPE_HALF ? 1 : 2);;
}


          
static inline int64_t
__instruction_expr___cat6_full_shift(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t TYPE_SHIFT;
    int64_t SRC2_SHIFT;

          TYPE_SHIFT = __instruction_expr___cat6_type_shift(s, p, src);

          SRC2_SHIFT = extract_reg_uim(src->srcs[2]);
    return TYPE_SHIFT + SRC2_SHIFT;
}


          
static inline int64_t
__instruction_expr_anon_34(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t OFF_LO;
    int64_t OFF_HI;

          OFF_LO = extract_reg_iim(src->srcs[1]) & 0xff;

          OFF_HI = extract_reg_iim(src->srcs[1]) >> 8;
    return (OFF_HI << 8) | OFF_LO;
}


          
static inline int64_t
__instruction_expr_anon_35(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t TYPE_SHIFT;

          TYPE_SHIFT = __instruction_expr___cat6_type_shift(s, p, src);
    return TYPE_SHIFT == 0;
}


          
static inline int64_t
__instruction_expr_anon_36(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t OFF;

          OFF = extract_reg_uim(src->srcs[3]);
    return OFF == 0;
}


          
static inline int64_t
__instruction_expr_anon_37(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t SRC2_SHIFT;

          SRC2_SHIFT = extract_reg_uim(src->srcs[2]);
    return SRC2_SHIFT == 0;
}


          
static inline int64_t
__instruction_expr_anon_38(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t OFF_LO;
    int64_t OFF_HI;

          OFF_LO = src->cat6.dst_offset & 0xff;

          OFF_HI = src->cat6.dst_offset >> 8;
    return (OFF_HI << 8) | OFF_LO;
}


          
static inline int64_t
__instruction_expr_anon_39(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t DST_HI;
    int64_t DST_LO;

          DST_HI = extract_reg_uim(src->srcs[0]) >> 8;

          DST_LO = extract_reg_uim(src->srcs[0]) & 0xff;
    return (DST_HI << 8) | DST_LO;
}


          
static inline int64_t
__instruction_expr___cat6_d(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t D_MINUS_ONE;

          D_MINUS_ONE = src->cat6.d - 1;
    return D_MINUS_ONE + 1;
}


          
static inline int64_t
__instruction_expr___cat6_type_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t TYPE_SIZE_MINUS_ONE;

          TYPE_SIZE_MINUS_ONE = src->cat6.iim_val - 1;
    return TYPE_SIZE_MINUS_ONE + 1;
}


          
static inline int64_t
__instruction_expr___cat6_load_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t LOAD_SIZE_MINUS_ONE;

          LOAD_SIZE_MINUS_ONE = src->cat6.iim_val - 1;
    return LOAD_SIZE_MINUS_ONE + 1;
}


          
static inline int64_t
__instruction_expr_anon_40(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t SP;

          SP = ISA_GPU_ID() >= 800;
    return !SP && (ISA_GPU_ID() >= 800);
}


          
static inline int64_t
__instruction_expr___cat6_direct(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t MODE;

          MODE = extract_cat6_DESC_MODE(src);
    return MODE == 0;
}


          
static inline int64_t
__instruction_expr_anon_42(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t HAS_OFFSET;

          HAS_OFFSET = !!(src->flags & IR3_INSTR_IMM_OFFSET);
    return !HAS_OFFSET;
}


          
static inline int64_t
__instruction_expr_anon_41(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t OFFSET_HI;
    int64_t OFFSET_LO;

          OFFSET_HI = extract_reg_uim(src->srcs[2]) >> 5;

          OFFSET_LO = extract_reg_uim(src->srcs[2]) & 0x1f;
    return (OFFSET_HI << 5) | OFFSET_LO;
}


          
static inline int64_t
__instruction_expr_anon_56(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t SRC_REG_TYPE;

          SRC_REG_TYPE = (src->srcs[0]->flags & IR3_REG_CONST) ? 1 : ((src->srcs[0]->flags & IR3_REG_IMMED) ? 2 : 0);
    return SRC_REG_TYPE == 0;
}


          
static inline int64_t
__instruction_expr_anon_57(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t SRC_REG_TYPE;

          SRC_REG_TYPE = (src->srcs[0]->flags & IR3_REG_CONST) ? 1 : ((src->srcs[0]->flags & IR3_REG_IMMED) ? 2 : 0);
    return SRC_REG_TYPE == 1;
}


          
static inline int64_t
__instruction_expr_anon_58(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t SCOPE;

          SCOPE = __instruction_expr_anon_54(s, p, src);
    return SCOPE == 1;
}


          
static inline int64_t
__instruction_expr_anon_54(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t SCOPE_HI;
    int64_t SCOPE_LO;

          SCOPE_HI = src->cat7.alias_scope >> 1;

          SCOPE_LO = src->cat7.alias_scope & 0x1;
    return (SCOPE_HI << 1) | SCOPE_LO;
}


          
static inline int64_t
__instruction_expr_anon_55(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
    int64_t TYPE_SIZE;

          TYPE_SIZE = (src->srcs[0]->flags & IR3_REG_HALF) ? 0 : 1;
    return TYPE_SIZE == 0;
}



/*
 * The actual encoder definitions
 */



static bitmask_t
snippet__reg_gpr_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__reg_gpr_expr___reg_gpr_a0(s, p, src)) {


           
    {

             fld = src->num & 0x3;
             const bitmask_t packed = pack_field(0, 1, fld, false);  /* SWIZ */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->num >> 2;
             const bitmask_t packed = pack_field(2, 7, fld, false);  /* GPR */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(2, 7, 61, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    if (__reg_gpr_expr___reg_gpr_p0(s, p, src)) {

       const bitmask_t packed = pack_field(2, 7, 62, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    if (__reg_gpr_expr___reg_gpr_p0(s, p, src)) {


           
    {

             fld = src->num & 0x3;
             const bitmask_t packed = pack_field(0, 1, fld, false);  /* SWIZ */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->num >> 2;
             const bitmask_t packed = pack_field(2, 7, fld, false);  /* GPR */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(2, 7, 62, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = src->num >> 2;
             const bitmask_t packed = pack_field(2, 7, fld, false);  /* GPR */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->num & 0x3;
             const bitmask_t packed = pack_field(0, 1, fld, false);  /* SWIZ */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__reg_gpr(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__reg_gpr_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__reg_const_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = src->num >> 2;
             const bitmask_t packed = pack_field(2, 10, fld, false);  /* CONST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->num & 0x3;
             const bitmask_t packed = pack_field(0, 1, fld, false);  /* SWIZ */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__reg_const(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__reg_const_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__reg_relative_gpr_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__reg_relative_gpr_expr___offset_zero(s, p, src)) {


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = src->array.offset;
             const bitmask_t packed = pack_field(0, 9, fld, true);  /* OFFSET */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__reg_relative_gpr(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__reg_relative_gpr_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__reg_relative_const_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__reg_relative_const_expr___offset_zero(s, p, src)) {


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = src->array.offset;
             const bitmask_t packed = pack_field(0, 9, fld, true);  /* OFFSET */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__reg_relative_const(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__reg_relative_const_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__multisrc_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__multisrc_expr___multisrc_half(s, p, src)) {


           
    {

             fld = extract_ABSNEG(src);
             const bitmask_t packed = pack_field(14, 15, fld, false);  /* ABSNEG */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_iim(src);
             const bitmask_t packed = pack_field(0, 10, fld, true);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = extract_ABSNEG(src);
             const bitmask_t packed = pack_field(14, 15, fld, false);  /* ABSNEG */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_iim(src);
             const bitmask_t packed = pack_field(0, 10, fld, true);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__multisrc_1(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = extract_ABSNEG(src);
             const bitmask_t packed = pack_field(14, 15, fld, false);  /* ABSNEG */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src);
             const bitmask_t packed = pack_field(0, 9, fld, false);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}




static bitmask_t
snippet__multisrc_2(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_REG_LAST_USE);
             const bitmask_t packed = pack_field(10, 10, fld, false);  /* LAST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_ABSNEG(src);
             const bitmask_t packed = pack_field(14, 15, fld, false);  /* ABSNEG */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__multisrc_3(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = extract_ABSNEG(src);
             const bitmask_t packed = pack_field(14, 15, fld, false);  /* ABSNEG */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_const(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 10, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__multisrc_4(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = extract_ABSNEG(src);
             const bitmask_t packed = pack_field(14, 15, fld, false);  /* ABSNEG */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_relative_gpr(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 9, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__multisrc_5(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = extract_ABSNEG(src);
             const bitmask_t packed = pack_field(14, 15, fld, false);  /* ABSNEG */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_relative_const(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 9, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__multisrc(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   switch (__multisrc_case(s, src)) {
   case REG_MULITSRC_IMMED: {


      bitmask_t val = uint64_t_to_bitmask(0x2000);




      BITSET_OR(val.bitset, val.bitset, snippet__multisrc_0(s, p, src).bitset);
      return val;
    }
   case REG_MULTISRC_IMMED_FLUT_FULL: {


      bitmask_t val = uint64_t_to_bitmask(0x2800);




      BITSET_OR(val.bitset, val.bitset, snippet__multisrc_1(s, p, src).bitset);
      return val;
    }
   case REG_MULTISRC_IMMED_FLUT_HALF: {


      bitmask_t val = uint64_t_to_bitmask(0x2c00);




      BITSET_OR(val.bitset, val.bitset, snippet__multisrc_1(s, p, src).bitset);
      return val;
    }
   case REG_MULTISRC_GPR: {


      bitmask_t val = uint64_t_to_bitmask(0x0);




      BITSET_OR(val.bitset, val.bitset, snippet__multisrc_2(s, p, src).bitset);
      return val;
    }
   case REG_MULTISRC_CONST: {


      bitmask_t val = uint64_t_to_bitmask(0x1000);




      BITSET_OR(val.bitset, val.bitset, snippet__multisrc_3(s, p, src).bitset);
      return val;
    }
   case REG_MULTISRC_RELATIVE_GPR: {


      bitmask_t val = uint64_t_to_bitmask(0x800);




      BITSET_OR(val.bitset, val.bitset, snippet__multisrc_4(s, p, src).bitset);
      return val;
    }
   case REG_MULTISRC_RELATIVE_CONST: {


      bitmask_t val = uint64_t_to_bitmask(0xc00);




      BITSET_OR(val.bitset, val.bitset, snippet__multisrc_5(s, p, src).bitset);
      return val;
    }
   default:
      /* Note that we need the default case, because there are
       * instructions which we never expect to be encoded, (ie.
       * meta/macro instructions) as they are removed/replace
       * in earlier stages of the compiler.
       */
      break;
   }
   mesa_loge("Unhandled #multisrc encode case: 0x%x\n", __multisrc_case(s, src));
   return uint64_t_to_bitmask(0);
}


static bitmask_t
snippet__cat1_dst_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__cat1_dst_expr_anon_1(s, p, src)) {


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    if (__cat1_dst_expr_anon_2(s, p, src)) {


           
    {

             fld = src->array.offset;
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* OFFSET */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__cat1_dst(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__cat1_dst_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__cat1_immed_src_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__cat1_immed_src_expr_anon_10(s, p, src)) {


           
    {

             fld = extract_reg_uim(src);
             const bitmask_t packed = pack_field(0, 15, fld, false);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    if (__cat1_immed_src_expr_anon_11(s, p, src)) {

             fld = extract_reg_uim(src);
             const bitmask_t packed = pack_field(0, 31, fld, false);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    } else


           
    if (__cat1_immed_src_expr_anon_12(s, p, src)) {

             fld = extract_reg_uim(src);
             const bitmask_t packed = pack_field(0, 31, fld, false);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    } else


           
    if (__cat1_immed_src_expr_anon_13(s, p, src)) {

             fld = extract_reg_uim(src);
             const bitmask_t packed = pack_field(0, 15, fld, true);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    } else


           
    if (__cat1_immed_src_expr_anon_14(s, p, src)) {

             fld = extract_reg_uim(src);
             const bitmask_t packed = pack_field(0, 31, fld, true);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    } else


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    if (__cat1_immed_src_expr_anon_11(s, p, src)) {


           
    {

             fld = extract_reg_uim(src);
             const bitmask_t packed = pack_field(0, 31, fld, false);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    if (__cat1_immed_src_expr_anon_12(s, p, src)) {

             fld = extract_reg_uim(src);
             const bitmask_t packed = pack_field(0, 31, fld, false);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    } else


           
    if (__cat1_immed_src_expr_anon_13(s, p, src)) {

             fld = extract_reg_uim(src);
             const bitmask_t packed = pack_field(0, 15, fld, true);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    } else


           
    if (__cat1_immed_src_expr_anon_14(s, p, src)) {

             fld = extract_reg_uim(src);
             const bitmask_t packed = pack_field(0, 31, fld, true);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    } else


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    if (__cat1_immed_src_expr_anon_12(s, p, src)) {


           
    {

             fld = extract_reg_uim(src);
             const bitmask_t packed = pack_field(0, 31, fld, false);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    if (__cat1_immed_src_expr_anon_13(s, p, src)) {

             fld = extract_reg_uim(src);
             const bitmask_t packed = pack_field(0, 15, fld, true);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    } else


           
    if (__cat1_immed_src_expr_anon_14(s, p, src)) {

             fld = extract_reg_uim(src);
             const bitmask_t packed = pack_field(0, 31, fld, true);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    } else


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    if (__cat1_immed_src_expr_anon_13(s, p, src)) {

             fld = extract_reg_uim(src);
             const bitmask_t packed = pack_field(0, 15, fld, true);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    } else


           
    if (__cat1_immed_src_expr_anon_14(s, p, src)) {

             fld = extract_reg_uim(src);
             const bitmask_t packed = pack_field(0, 31, fld, true);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    } else


           
    {

             fld = extract_reg_uim(src);
             const bitmask_t packed = pack_field(0, 31, fld, false);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__cat1_immed_src(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__cat1_immed_src_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__cat1_const_src_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_const(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 10, fld, false);  /* CONST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__cat1_const_src(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__cat1_const_src_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__cat1_gpr_src_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__cat1_gpr_src(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__cat1_gpr_src_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__cat1_relative_gpr_src_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_relative_gpr(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 9, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__cat1_relative_gpr_src(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__cat1_relative_gpr_src_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__cat1_relative_const_src_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_relative_const(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 9, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__cat1_relative_const_src(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__cat1_relative_const_src_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__cat1_multi_src_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* REG */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__cat1_multi_src(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__cat1_multi_src_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__cat1_multi_dst_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* REG */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__cat1_multi_dst(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__cat1_multi_dst_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__dst_gpr_cat2_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__dst_gpr_cat2_expr___reg_gpr_a0(s, p, src)) {


           
    {

             fld = src->num & 0x3;
             const bitmask_t packed = pack_field(0, 1, fld, false);  /* SWIZ */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_dst_num(src);
             const bitmask_t packed = pack_field(2, 7, fld, false);  /* GPR */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(2, 7, 61, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    if (__dst_gpr_cat2_expr___reg_gpr_p0(s, p, src)) {

       const bitmask_t packed = pack_field(2, 7, 62, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    if (__dst_gpr_cat2_expr___reg_gpr_p0(s, p, src)) {


           
    {

             fld = src->num & 0x3;
             const bitmask_t packed = pack_field(0, 1, fld, false);  /* SWIZ */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_dst_num(src);
             const bitmask_t packed = pack_field(2, 7, fld, false);  /* GPR */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(2, 7, 62, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = extract_dst_num(src);
             const bitmask_t packed = pack_field(2, 7, fld, false);  /* GPR */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->num & 0x3;
             const bitmask_t packed = pack_field(0, 1, fld, false);  /* SWIZ */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__dst_gpr_cat2(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__dst_gpr_cat2_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__cat3_src_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_REG_LAST_USE);
             const bitmask_t packed = pack_field(10, 10, fld, false);  /* LAST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__cat3_src_1(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_REG_HALF);
             const bitmask_t packed = pack_field(7, 7, fld, false);  /* HALF */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src);
             const bitmask_t packed = pack_field(0, 3, fld, false);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(4, 6, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__cat3_src_2(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = extract_reg_uim(src);
             const bitmask_t packed = pack_field(0, 7, fld, true);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__cat3_src_3(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__cat3_src_expr_anon_16(s, p, src)) {


           
    {

             fld = extract_reg_uim(src);
             const bitmask_t packed = pack_field(0, 11, fld, false);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(11, 11, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = src->num >> 2;
             const bitmask_t packed = pack_field(2, 10, fld, false);  /* CONST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->num & 0x3;
             const bitmask_t packed = pack_field(0, 1, fld, false);  /* SWIZ */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(11, 11, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__cat3_src_4(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = src->array.offset;
             const bitmask_t packed = pack_field(0, 9, fld, true);  /* OFFSET */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
encode__cat3_src(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   switch (__cat3_src_case(s, src)) {
   case REG_CAT3_SRC_GPR: {


      bitmask_t val = uint64_t_to_bitmask(0x0);




      BITSET_OR(val.bitset, val.bitset, snippet__cat3_src_0(s, p, src).bitset);
      return val;
    }
   case REG_CAT3_SRC_FLUT: {
      if (s->gen >= 800 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0x700);




      BITSET_OR(val.bitset, val.bitset, snippet__cat3_src_1(s, p, src).bitset);
      return val;
      }
      break;
    }
   case REG_CAT3_SRC_ALT_IMMED: {
      if (s->gen >= 800 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0x600);




      BITSET_OR(val.bitset, val.bitset, snippet__cat3_src_2(s, p, src).bitset);
      return val;
      }
      break;
    }
   case REG_CAT3_SRC_CONST_OR_IMMED: {


      bitmask_t val = uint64_t_to_bitmask(0x1000);




      BITSET_OR(val.bitset, val.bitset, snippet__cat3_src_3(s, p, src).bitset);
      return val;
    }
   case REG_CAT3_SRC_RELATIVE_GPR: {


      bitmask_t val = uint64_t_to_bitmask(0x800);




      BITSET_OR(val.bitset, val.bitset, snippet__cat3_src_4(s, p, src).bitset);
      return val;
    }
   case REG_CAT3_SRC_RELATIVE_CONST: {


      bitmask_t val = uint64_t_to_bitmask(0xc00);




      BITSET_OR(val.bitset, val.bitset, snippet__cat3_src_4(s, p, src).bitset);
      return val;
    }
   default:
      /* Note that we need the default case, because there are
       * instructions which we never expect to be encoded, (ie.
       * meta/macro instructions) as they are removed/replace
       * in earlier stages of the compiler.
       */
      break;
   }
   mesa_loge("Unhandled #cat3-src encode case: 0x%x\n", __cat3_src_case(s, src));
   return uint64_t_to_bitmask(0);
}


static bitmask_t
snippet__cat5_s2en_bindless_base_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__cat5_s2en_bindless_base_expr_anon_18(s, p, src)) {


           
    {

             fld = src->cat5.tex_base & 0x1;
             const bitmask_t packed = pack_field(0, 0, fld, false);  /* BASE_LO */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__cat5_s2en_bindless_base(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__cat5_s2en_bindless_base_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__cat5_src1_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__cat5_src1_expr_anon_21(s, p, src)) {


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(0, 7, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {



       
    {

       const bitmask_t packed = pack_field(0, 7, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__cat5_src1(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__cat5_src1_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__cat5_src2_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__cat5_src2_expr_anon_22(s, p, src)) {


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(0, 7, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    if (__cat5_src2_expr_anon_23(s, p, src)) {


           
    {

             fld = extract_reg_uim(src);
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* OFF */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(0, 7, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {



       
    {

       const bitmask_t packed = pack_field(0, 7, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__cat5_src2(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__cat5_src2_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__cat5_samp_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__cat5_samp_expr_anon_24(s, p, src)) {


           
    {

             fld = src->cat5.samp;
             const bitmask_t packed = pack_field(0, 3, fld, false);  /* SAMP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(0, 3, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {



       
    {

       const bitmask_t packed = pack_field(0, 3, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__cat5_samp(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__cat5_samp_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__cat5_samp_s2en_bindless_a1_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__cat5_samp_s2en_bindless_a1_expr_anon_25(s, p, src)) {


           
    {

             fld = src->cat5.samp;
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* SAMP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(0, 7, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {



       
    {

       const bitmask_t packed = pack_field(0, 7, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__cat5_samp_s2en_bindless_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__cat5_samp_s2en_bindless_a1_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__cat5_tex_s2en_bindless_a1_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__cat5_tex_s2en_bindless_a1_expr_anon_26(s, p, src)) {


           
    {

             fld = src->cat5.tex;
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* TEX */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(0, 7, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {



       
    {

       const bitmask_t packed = pack_field(0, 7, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__cat5_tex_s2en_bindless_a1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__cat5_tex_s2en_bindless_a1_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__cat5_tex_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__cat5_tex_expr_anon_27(s, p, src)) {


           
    {

             fld = src->cat5.tex;
             const bitmask_t packed = pack_field(0, 6, fld, false);  /* TEX */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(0, 6, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {



       
    {

       const bitmask_t packed = pack_field(0, 6, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__cat5_tex(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__cat5_tex_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__cat5_tex_s2en_bindless_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__cat5_tex_s2en_bindless_expr_anon_28(s, p, src)) {


           
    {

             fld = src->cat5.tex;
             const bitmask_t packed = pack_field(0, 3, fld, false);  /* TEX */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(0, 3, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {



       
    {

       const bitmask_t packed = pack_field(0, 3, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__cat5_tex_s2en_bindless(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__cat5_tex_s2en_bindless_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__cat5_type_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__cat5_type_expr_anon_29(s, p, src)) {


           
    {

             fld = (src->flags & IR3_INSTR_RCK) ? 7 : src->cat5.type;
             const bitmask_t packed = pack_field(0, 2, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = (src->flags & IR3_INSTR_RCK) ? 7 : src->cat5.type;
             const bitmask_t packed = pack_field(0, 2, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__cat5_type(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__cat5_type_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__cat5_src3_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__cat5_src3_expr___cat5_s2enb_is_indirect(s, p, src)) {


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    if (__cat5_src3_expr___cat5_s2enb_uses_a1_gen6(s, p, src)) {


           
    {

             { 
 struct bitset_params bp = {
    .HAS_SAMP = p->HAS_SAMP,  /* HAS_SAMP */
 };

               bitmask_t tmp = encode__cat5_samp_s2en_bindless_a1(s, &bp, s->instr);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* SAMP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    if (__cat5_src3_expr___cat5_s2enb_uses_a1_gen7(s, p, src)) {


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TEX = p->HAS_TEX,  /* HAS_TEX */
 };

               bitmask_t tmp = encode__cat5_tex_s2en_bindless_a1(s, &bp, s->instr);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* TEX */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             { 
 struct bitset_params bp = {
    .HAS_SAMP = p->HAS_SAMP,  /* HAS_SAMP */
 };

               bitmask_t tmp = encode__cat5_samp(s, &bp, s->instr);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 3, fld, false);  /* SAMP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TEX = p->HAS_TEX,  /* HAS_TEX */
 };

               bitmask_t tmp = encode__cat5_tex_s2en_bindless(s, &bp, s->instr);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(4, 7, fld, false);  /* TEX */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__cat5_src3(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__cat5_src3_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__const_dst_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = extract_reg_uim(src->srcs[0]);
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* OFFSET */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
encode__const_dst(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   switch (__const_dst_case(s, src)) {
   case CONST_DST_IMM: {


      bitmask_t val = uint64_t_to_bitmask(0x0);




      BITSET_OR(val.bitset, val.bitset, snippet__const_dst_0(s, p, src).bitset);
      return val;
    }
   case CONST_DST_A1: {


      bitmask_t val = uint64_t_to_bitmask(0x100);




      BITSET_OR(val.bitset, val.bitset, snippet__const_dst_0(s, p, src).bitset);
      return val;
    }
   default:
      /* Note that we need the default case, because there are
       * instructions which we never expect to be encoded, (ie.
       * meta/macro instructions) as they are removed/replace
       * in earlier stages of the compiler.
       */
      break;
   }
   mesa_loge("Unhandled #const-dst encode case: 0x%x\n", __const_dst_case(s, src));
   return uint64_t_to_bitmask(0);
}


static bitmask_t
snippet__cat6_typed_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__cat6_typed_expr_anon_43(s, p, src)) {


           
    {

             fld = src->cat6.typed;
             const bitmask_t packed = pack_field(0, 0, fld, false);  /* TYPED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = src->cat6.typed;
             const bitmask_t packed = pack_field(0, 0, fld, false);  /* TYPED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__cat6_typed(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__cat6_typed_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__cat6_base_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__cat6_base_expr_anon_44(s, p, src)) {


           
    {

             fld = src->cat6.base;
             const bitmask_t packed = pack_field(0, 2, fld, false);  /* BASE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__cat6_base(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__cat6_base_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__cat6_src_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__cat6_src_expr_anon_45(s, p, src)) {


           
    {

             fld = extract_reg_iim(src);
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = src->num >> 2;
             const bitmask_t packed = pack_field(2, 7, fld, false);  /* GPR */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->num & 0x3;
             const bitmask_t packed = pack_field(0, 1, fld, false);  /* SWIZ */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__cat6_src(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__cat6_src_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__cat6_src_sp_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__cat6_src_sp_expr_anon_46(s, p, src)) {


           
    {

             fld = extract_reg_iim(src);
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = src->num >> 2;
             const bitmask_t packed = pack_field(2, 7, fld, false);  /* GPR */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->num & 0x3;
             const bitmask_t packed = pack_field(0, 1, fld, false);  /* SWIZ */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__cat6_src_sp(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__cat6_src_sp_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__cat6_src_load_size_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__cat6_src_load_size_expr_anon_47(s, p, src)) {


           
    {

             fld = extract_reg_uim(src) - 1;
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* LOAD_SIZE_MINUS_ONE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = src->num >> 2;
             const bitmask_t packed = pack_field(2, 7, fld, false);  /* GPR */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->num & 0x3;
             const bitmask_t packed = pack_field(0, 1, fld, false);  /* SWIZ */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__cat6_src_load_size(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__cat6_src_load_size_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__cat6_src_const_or_gpr_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__cat6_src_const_or_gpr_expr_anon_48(s, p, src)) {


           
    {

             fld = src->num >> 2;
             const bitmask_t packed = pack_field(2, 7, fld, false);  /* GPR */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->num & 0x3;
             const bitmask_t packed = pack_field(0, 1, fld, false);  /* SWIZ */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = src->num >> 2;
             const bitmask_t packed = pack_field(2, 7, fld, false);  /* GPR */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->num & 0x3;
             const bitmask_t packed = pack_field(0, 1, fld, false);  /* SWIZ */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__cat6_src_const_or_gpr(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__cat6_src_const_or_gpr_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__alias_immed_src_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__alias_immed_src_expr_anon_49(s, p, src)) {


           
    {

             fld = extract_reg_uim(src);
             const bitmask_t packed = pack_field(0, 15, fld, false);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    if (__alias_immed_src_expr_anon_50(s, p, src)) {

             fld = extract_reg_uim(src);
             const bitmask_t packed = pack_field(0, 31, fld, false);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    } else


           
    if (__alias_immed_src_expr_anon_51(s, p, src)) {

             fld = extract_reg_uim(src);
             const bitmask_t packed = pack_field(0, 15, fld, false);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    } else


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    if (__alias_immed_src_expr_anon_50(s, p, src)) {


           
    {

             fld = extract_reg_uim(src);
             const bitmask_t packed = pack_field(0, 31, fld, false);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    if (__alias_immed_src_expr_anon_51(s, p, src)) {

             fld = extract_reg_uim(src);
             const bitmask_t packed = pack_field(0, 15, fld, false);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    } else


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    if (__alias_immed_src_expr_anon_51(s, p, src)) {


           
    {

             fld = extract_reg_uim(src);
             const bitmask_t packed = pack_field(0, 15, fld, false);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = extract_reg_uim(src);
             const bitmask_t packed = pack_field(0, 31, fld, false);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__alias_immed_src(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__alias_immed_src_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__alias_const_src_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_const(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 10, fld, false);  /* CONST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__alias_const_src(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__alias_const_src_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__alias_gpr_src_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__alias_gpr_src(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__alias_gpr_src_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__dst_rt_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = src->num >> 2;
             const bitmask_t packed = pack_field(2, 4, fld, false);  /* RT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->num & 0x3;
             const bitmask_t packed = pack_field(0, 1, fld, false);  /* SWIZ */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__dst_rt(struct encode_state *s, const struct bitset_params *p, const struct ir3_register * src)
{

      bitmask_t val = uint64_t_to_bitmask(0x0);
      BITSET_OR(val.bitset, val.bitset, snippet__dst_rt_0(s, p, src).bitset);
      return val;
}


static bitmask_t
snippet__instruction_0(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_EQ);
             const bitmask_t packed = pack_field(48, 48, fld, false);  /* EQ */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_EOLM);
             const bitmask_t packed = pack_field(32, 32, fld, false);  /* EOLM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_EOGM);
             const bitmask_t packed = pack_field(33, 33, fld, false);  /* EOGM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_EOSTSC);
             const bitmask_t packed = pack_field(34, 34, fld, false);  /* EOSTSC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 42, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}














static bitmask_t
snippet__instruction_1(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_EQ);
             const bitmask_t packed = pack_field(48, 48, fld, false);  /* EQ */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat0.inv1;
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* INV1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = reg_comp(src->srcs[0]);
             const bitmask_t packed = pack_field(53, 54, fld, false);  /* COMP1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_2(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat0.immed;
             const bitmask_t packed = pack_field(0, 31, fld, true);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}





static bitmask_t
snippet__instruction_3(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat0.immed;
             const bitmask_t packed = pack_field(0, 31, fld, true);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = util_logbase2(8) - 1;
             const bitmask_t packed = pack_field(52, 54, fld, false);  /* W */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}





static bitmask_t
snippet__instruction_4(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_EQ);
             const bitmask_t packed = pack_field(48, 48, fld, false);  /* EQ */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat0.idx;
             const bitmask_t packed = pack_field(32, 36, fld, false);  /* INDEX */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat0.immed;
             const bitmask_t packed = pack_field(0, 31, fld, true);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_5(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


   return val;
}



static bitmask_t
snippet__instruction_6(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_EQ);
             const bitmask_t packed = pack_field(48, 48, fld, false);  /* EQ */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat0.inv1;
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* INV1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = reg_comp(src->srcs[0]);
             const bitmask_t packed = pack_field(53, 54, fld, false);  /* COMP1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat0.immed;
             const bitmask_t packed = pack_field(0, 31, fld, true);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}





static bitmask_t
snippet__instruction_7(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_EQ);
             const bitmask_t packed = pack_field(48, 48, fld, false);  /* EQ */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat0.inv1;
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* INV1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = reg_comp(src->srcs[0]);
             const bitmask_t packed = pack_field(53, 54, fld, false);  /* COMP1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat0.inv2;
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* INV2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = reg_comp(src->srcs[1]);
             const bitmask_t packed = pack_field(46, 47, fld, false);  /* COMP2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat0.immed;
             const bitmask_t packed = pack_field(0, 31, fld, true);  /* IMMED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}




static bitmask_t
snippet__instruction_8(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__instruction_expr_anon_9(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.round;
             const bitmask_t packed = pack_field(55, 56, fld, false);  /* ROUND */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* DM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[0]);
             const bitmask_t packed = pack_field(0, 13, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.r[0];
             const bitmask_t packed = pack_field(14, 22, fld, false);  /* R0 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.r[1];
             const bitmask_t packed = pack_field(23, 31, fld, false);  /* R1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.src_type;
             const bitmask_t packed = pack_field(50, 52, fld, false);  /* SRC_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.dst_type;
             const bitmask_t packed = pack_field(46, 48, fld, false);  /* DST_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_9(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__instruction_expr_anon_9(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.round;
             const bitmask_t packed = pack_field(55, 56, fld, false);  /* ROUND */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* DM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .LAST = __instruction_expr___false(s, p, src),  /* FALSE */
    .SRC_R = __instruction_expr___false(s, p, src),  /* FALSE */
    .HALF = __instruction_expr_anon_3(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat1_gpr_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.r[0];
             const bitmask_t packed = pack_field(14, 22, fld, false);  /* R0 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.r[1];
             const bitmask_t packed = pack_field(23, 31, fld, false);  /* R1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.src_type;
             const bitmask_t packed = pack_field(50, 52, fld, false);  /* SRC_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.dst_type;
             const bitmask_t packed = pack_field(46, 48, fld, false);  /* DST_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(8, 13, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {



       
    {

       const bitmask_t packed = pack_field(8, 13, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_10(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__instruction_expr_anon_6(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.round;
             const bitmask_t packed = pack_field(55, 56, fld, false);  /* ROUND */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* DM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_TYPE = src->cat1.src_type,  /* SRC_TYPE */
 };

               bitmask_t tmp = encode__cat1_immed_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 31, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.src_type;
             const bitmask_t packed = pack_field(50, 52, fld, false);  /* SRC_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .DST_REL = !!(src->dsts[0]->flags & IR3_REG_RELATIV),  /* DST_REL */
 };

               bitmask_t tmp = encode__cat1_dst(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.dst_type;
             const bitmask_t packed = pack_field(46, 48, fld, false);  /* DST_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_RELATIV);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* DST_REL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(32, 39, 244, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(46, 48, 4, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(50, 52, 4, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    if (__instruction_expr_anon_7(s, p, src)) {

       const bitmask_t packed = pack_field(32, 39, 245, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    if (__instruction_expr_anon_7(s, p, src)) {

       const bitmask_t packed = pack_field(46, 48, 2, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    if (__instruction_expr_anon_7(s, p, src)) {

       const bitmask_t packed = pack_field(50, 52, 2, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    if (__instruction_expr_anon_7(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.round;
             const bitmask_t packed = pack_field(55, 56, fld, false);  /* ROUND */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* DM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_TYPE = src->cat1.src_type,  /* SRC_TYPE */
 };

               bitmask_t tmp = encode__cat1_immed_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 31, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.src_type;
             const bitmask_t packed = pack_field(50, 52, fld, false);  /* SRC_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .DST_REL = !!(src->dsts[0]->flags & IR3_REG_RELATIV),  /* DST_REL */
 };

               bitmask_t tmp = encode__cat1_dst(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.dst_type;
             const bitmask_t packed = pack_field(46, 48, fld, false);  /* DST_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_RELATIV);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* DST_REL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(32, 39, 245, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(46, 48, 2, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(50, 52, 2, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    if (__instruction_expr_anon_8(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.src_type;
             const bitmask_t packed = pack_field(50, 52, fld, false);  /* SRC_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.dst_type;
             const bitmask_t packed = pack_field(46, 48, fld, false);  /* DST_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.round;
             const bitmask_t packed = pack_field(55, 56, fld, false);  /* ROUND */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* DM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .DST_REL = !!(src->dsts[0]->flags & IR3_REG_RELATIV),  /* DST_REL */
 };

               bitmask_t tmp = encode__cat1_dst(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_TYPE = src->cat1.src_type,  /* SRC_TYPE */
 };

               bitmask_t tmp = encode__cat1_immed_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 31, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_RELATIV);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* DST_REL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.src_type;
             const bitmask_t packed = pack_field(50, 52, fld, false);  /* SRC_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.dst_type;
             const bitmask_t packed = pack_field(46, 48, fld, false);  /* DST_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.round;
             const bitmask_t packed = pack_field(55, 56, fld, false);  /* ROUND */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* DM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .DST_REL = !!(src->dsts[0]->flags & IR3_REG_RELATIV),  /* DST_REL */
 };

               bitmask_t tmp = encode__cat1_dst(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_TYPE = src->cat1.src_type,  /* SRC_TYPE */
 };

               bitmask_t tmp = encode__cat1_immed_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 31, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_RELATIV);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* DST_REL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_11(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__instruction_expr_anon_6(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.round;
             const bitmask_t packed = pack_field(55, 56, fld, false);  /* ROUND */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* DM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_R = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U),  /* SRC_R */
    .HALF = __instruction_expr_anon_3(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat1_const_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 10, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.src_type;
             const bitmask_t packed = pack_field(50, 52, fld, false);  /* SRC_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .DST_REL = !!(src->dsts[0]->flags & IR3_REG_RELATIV),  /* DST_REL */
 };

               bitmask_t tmp = encode__cat1_dst(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.dst_type;
             const bitmask_t packed = pack_field(46, 48, fld, false);  /* DST_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_RELATIV);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* DST_REL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(32, 39, 244, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(46, 48, 4, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(50, 52, 4, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    if (__instruction_expr_anon_7(s, p, src)) {

       const bitmask_t packed = pack_field(32, 39, 245, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    if (__instruction_expr_anon_7(s, p, src)) {

       const bitmask_t packed = pack_field(46, 48, 2, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    if (__instruction_expr_anon_7(s, p, src)) {

       const bitmask_t packed = pack_field(50, 52, 2, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    if (__instruction_expr_anon_7(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.round;
             const bitmask_t packed = pack_field(55, 56, fld, false);  /* ROUND */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* DM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_R = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U),  /* SRC_R */
    .HALF = __instruction_expr_anon_3(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat1_const_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 10, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.src_type;
             const bitmask_t packed = pack_field(50, 52, fld, false);  /* SRC_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .DST_REL = !!(src->dsts[0]->flags & IR3_REG_RELATIV),  /* DST_REL */
 };

               bitmask_t tmp = encode__cat1_dst(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.dst_type;
             const bitmask_t packed = pack_field(46, 48, fld, false);  /* DST_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_RELATIV);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* DST_REL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(32, 39, 245, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(46, 48, 2, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(50, 52, 2, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    if (__instruction_expr_anon_8(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.src_type;
             const bitmask_t packed = pack_field(50, 52, fld, false);  /* SRC_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.dst_type;
             const bitmask_t packed = pack_field(46, 48, fld, false);  /* DST_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.round;
             const bitmask_t packed = pack_field(55, 56, fld, false);  /* ROUND */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* DM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .DST_REL = !!(src->dsts[0]->flags & IR3_REG_RELATIV),  /* DST_REL */
 };

               bitmask_t tmp = encode__cat1_dst(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_R = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U),  /* SRC_R */
    .HALF = __instruction_expr_anon_3(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat1_const_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 10, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_RELATIV);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* DST_REL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.src_type;
             const bitmask_t packed = pack_field(50, 52, fld, false);  /* SRC_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.dst_type;
             const bitmask_t packed = pack_field(46, 48, fld, false);  /* DST_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.round;
             const bitmask_t packed = pack_field(55, 56, fld, false);  /* ROUND */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* DM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .DST_REL = !!(src->dsts[0]->flags & IR3_REG_RELATIV),  /* DST_REL */
 };

               bitmask_t tmp = encode__cat1_dst(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_R = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U),  /* SRC_R */
    .HALF = __instruction_expr_anon_3(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat1_const_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 10, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_RELATIV);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* DST_REL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_12(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__instruction_expr_anon_6(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.round;
             const bitmask_t packed = pack_field(55, 56, fld, false);  /* ROUND */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* DM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .LAST = !!(src->srcs[0]->flags & IR3_REG_LAST_USE),  /* LAST */
    .SRC_R = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U),  /* SRC_R */
    .HALF = __instruction_expr_anon_3(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat1_gpr_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.src_type;
             const bitmask_t packed = pack_field(50, 52, fld, false);  /* SRC_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .DST_REL = !!(src->dsts[0]->flags & IR3_REG_RELATIV),  /* DST_REL */
 };

               bitmask_t tmp = encode__cat1_dst(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.dst_type;
             const bitmask_t packed = pack_field(46, 48, fld, false);  /* DST_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_LAST_USE);
             const bitmask_t packed = pack_field(10, 10, fld, false);  /* LAST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_RELATIV);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* DST_REL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(32, 39, 244, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(46, 48, 4, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(50, 52, 4, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    if (__instruction_expr_anon_7(s, p, src)) {

       const bitmask_t packed = pack_field(32, 39, 245, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    if (__instruction_expr_anon_7(s, p, src)) {

       const bitmask_t packed = pack_field(46, 48, 2, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    if (__instruction_expr_anon_7(s, p, src)) {

       const bitmask_t packed = pack_field(50, 52, 2, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    if (__instruction_expr_anon_7(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.round;
             const bitmask_t packed = pack_field(55, 56, fld, false);  /* ROUND */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* DM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .LAST = !!(src->srcs[0]->flags & IR3_REG_LAST_USE),  /* LAST */
    .SRC_R = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U),  /* SRC_R */
    .HALF = __instruction_expr_anon_3(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat1_gpr_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.src_type;
             const bitmask_t packed = pack_field(50, 52, fld, false);  /* SRC_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .DST_REL = !!(src->dsts[0]->flags & IR3_REG_RELATIV),  /* DST_REL */
 };

               bitmask_t tmp = encode__cat1_dst(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.dst_type;
             const bitmask_t packed = pack_field(46, 48, fld, false);  /* DST_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_LAST_USE);
             const bitmask_t packed = pack_field(10, 10, fld, false);  /* LAST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_RELATIV);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* DST_REL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(32, 39, 245, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(46, 48, 2, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(50, 52, 2, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    if (__instruction_expr_anon_8(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.src_type;
             const bitmask_t packed = pack_field(50, 52, fld, false);  /* SRC_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.dst_type;
             const bitmask_t packed = pack_field(46, 48, fld, false);  /* DST_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.round;
             const bitmask_t packed = pack_field(55, 56, fld, false);  /* ROUND */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* DM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .DST_REL = !!(src->dsts[0]->flags & IR3_REG_RELATIV),  /* DST_REL */
 };

               bitmask_t tmp = encode__cat1_dst(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .LAST = !!(src->srcs[0]->flags & IR3_REG_LAST_USE),  /* LAST */
    .SRC_R = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U),  /* SRC_R */
    .HALF = __instruction_expr_anon_3(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat1_gpr_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_RELATIV);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* DST_REL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_LAST_USE);
             const bitmask_t packed = pack_field(10, 10, fld, false);  /* LAST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.src_type;
             const bitmask_t packed = pack_field(50, 52, fld, false);  /* SRC_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.dst_type;
             const bitmask_t packed = pack_field(46, 48, fld, false);  /* DST_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.round;
             const bitmask_t packed = pack_field(55, 56, fld, false);  /* ROUND */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* DM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .DST_REL = !!(src->dsts[0]->flags & IR3_REG_RELATIV),  /* DST_REL */
 };

               bitmask_t tmp = encode__cat1_dst(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .LAST = !!(src->srcs[0]->flags & IR3_REG_LAST_USE),  /* LAST */
    .SRC_R = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U),  /* SRC_R */
    .HALF = __instruction_expr_anon_3(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat1_gpr_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_RELATIV);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* DST_REL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_LAST_USE);
             const bitmask_t packed = pack_field(10, 10, fld, false);  /* LAST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_13(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.src_type;
             const bitmask_t packed = pack_field(50, 52, fld, false);  /* SRC_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.dst_type;
             const bitmask_t packed = pack_field(46, 48, fld, false);  /* DST_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.round;
             const bitmask_t packed = pack_field(55, 56, fld, false);  /* ROUND */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* DM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .DST_REL = !!(src->dsts[0]->flags & IR3_REG_RELATIV),  /* DST_REL */
 };

               bitmask_t tmp = encode__cat1_dst(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .LAST = !!(src->srcs[0]->flags & IR3_REG_LAST_USE),  /* LAST */
    .SRC_R = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U),  /* SRC_R */
    .HALF = __instruction_expr_anon_3(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat1_gpr_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[1]);
             const bitmask_t packed = pack_field(23, 29, fld, false);  /* INVOCATION */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_RELATIV);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* DST_REL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_LAST_USE);
             const bitmask_t packed = pack_field(10, 10, fld, false);  /* LAST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    if (__instruction_expr_anon_6(s, p, src)) {

       const bitmask_t packed = pack_field(32, 39, 244, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    if (__instruction_expr_anon_6(s, p, src)) {

       const bitmask_t packed = pack_field(46, 48, 4, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    if (__instruction_expr_anon_6(s, p, src)) {

       const bitmask_t packed = pack_field(50, 52, 4, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    if (__instruction_expr_anon_7(s, p, src)) {

       const bitmask_t packed = pack_field(32, 39, 245, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    if (__instruction_expr_anon_7(s, p, src)) {

       const bitmask_t packed = pack_field(46, 48, 2, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    if (__instruction_expr_anon_7(s, p, src)) {

       const bitmask_t packed = pack_field(50, 52, 2, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_14(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.src_type;
             const bitmask_t packed = pack_field(50, 52, fld, false);  /* SRC_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.dst_type;
             const bitmask_t packed = pack_field(46, 48, fld, false);  /* DST_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.round;
             const bitmask_t packed = pack_field(55, 56, fld, false);  /* ROUND */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* DM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .DST_REL = !!(src->dsts[0]->flags & IR3_REG_RELATIV),  /* DST_REL */
 };

               bitmask_t tmp = encode__cat1_dst(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .LAST = !!(src->srcs[0]->flags & IR3_REG_LAST_USE),  /* LAST */
    .SRC_R = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U),  /* SRC_R */
    .HALF = __instruction_expr_anon_3(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat1_gpr_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_RELATIV);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* DST_REL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_LAST_USE);
             const bitmask_t packed = pack_field(10, 10, fld, false);  /* LAST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    if (__instruction_expr_anon_6(s, p, src)) {

       const bitmask_t packed = pack_field(32, 39, 244, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    if (__instruction_expr_anon_6(s, p, src)) {

       const bitmask_t packed = pack_field(46, 48, 4, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    if (__instruction_expr_anon_6(s, p, src)) {

       const bitmask_t packed = pack_field(50, 52, 4, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    if (__instruction_expr_anon_7(s, p, src)) {

       const bitmask_t packed = pack_field(32, 39, 245, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    if (__instruction_expr_anon_7(s, p, src)) {

       const bitmask_t packed = pack_field(46, 48, 2, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    if (__instruction_expr_anon_7(s, p, src)) {

       const bitmask_t packed = pack_field(50, 52, 2, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_15(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__instruction_expr_anon_6(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.round;
             const bitmask_t packed = pack_field(55, 56, fld, false);  /* ROUND */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* DM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_R = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U),  /* SRC_R */
    .HALF = __instruction_expr_anon_3(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat1_relative_gpr_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 9, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.src_type;
             const bitmask_t packed = pack_field(50, 52, fld, false);  /* SRC_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .DST_REL = !!(src->dsts[0]->flags & IR3_REG_RELATIV),  /* DST_REL */
 };

               bitmask_t tmp = encode__cat1_dst(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.dst_type;
             const bitmask_t packed = pack_field(46, 48, fld, false);  /* DST_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_RELATIV);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* DST_REL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(32, 39, 244, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(46, 48, 4, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(50, 52, 4, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    if (__instruction_expr_anon_7(s, p, src)) {

       const bitmask_t packed = pack_field(32, 39, 245, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    if (__instruction_expr_anon_7(s, p, src)) {

       const bitmask_t packed = pack_field(46, 48, 2, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    if (__instruction_expr_anon_7(s, p, src)) {

       const bitmask_t packed = pack_field(50, 52, 2, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    if (__instruction_expr_anon_7(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.round;
             const bitmask_t packed = pack_field(55, 56, fld, false);  /* ROUND */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* DM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_R = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U),  /* SRC_R */
    .HALF = __instruction_expr_anon_3(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat1_relative_gpr_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 9, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.src_type;
             const bitmask_t packed = pack_field(50, 52, fld, false);  /* SRC_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .DST_REL = !!(src->dsts[0]->flags & IR3_REG_RELATIV),  /* DST_REL */
 };

               bitmask_t tmp = encode__cat1_dst(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.dst_type;
             const bitmask_t packed = pack_field(46, 48, fld, false);  /* DST_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_RELATIV);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* DST_REL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(32, 39, 245, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(46, 48, 2, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(50, 52, 2, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    if (__instruction_expr_anon_8(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.src_type;
             const bitmask_t packed = pack_field(50, 52, fld, false);  /* SRC_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.dst_type;
             const bitmask_t packed = pack_field(46, 48, fld, false);  /* DST_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.round;
             const bitmask_t packed = pack_field(55, 56, fld, false);  /* ROUND */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* DM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .DST_REL = !!(src->dsts[0]->flags & IR3_REG_RELATIV),  /* DST_REL */
 };

               bitmask_t tmp = encode__cat1_dst(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_R = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U),  /* SRC_R */
    .HALF = __instruction_expr_anon_3(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat1_relative_gpr_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 9, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_RELATIV);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* DST_REL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.src_type;
             const bitmask_t packed = pack_field(50, 52, fld, false);  /* SRC_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.dst_type;
             const bitmask_t packed = pack_field(46, 48, fld, false);  /* DST_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.round;
             const bitmask_t packed = pack_field(55, 56, fld, false);  /* ROUND */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* DM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .DST_REL = !!(src->dsts[0]->flags & IR3_REG_RELATIV),  /* DST_REL */
 };

               bitmask_t tmp = encode__cat1_dst(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_R = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U),  /* SRC_R */
    .HALF = __instruction_expr_anon_3(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat1_relative_gpr_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 9, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_RELATIV);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* DST_REL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_16(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__instruction_expr_anon_6(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.round;
             const bitmask_t packed = pack_field(55, 56, fld, false);  /* ROUND */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* DM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_R = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U),  /* SRC_R */
    .HALF = __instruction_expr_anon_3(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat1_relative_const_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 9, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.src_type;
             const bitmask_t packed = pack_field(50, 52, fld, false);  /* SRC_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .DST_REL = !!(src->dsts[0]->flags & IR3_REG_RELATIV),  /* DST_REL */
 };

               bitmask_t tmp = encode__cat1_dst(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.dst_type;
             const bitmask_t packed = pack_field(46, 48, fld, false);  /* DST_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_RELATIV);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* DST_REL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(32, 39, 244, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(46, 48, 4, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(50, 52, 4, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    if (__instruction_expr_anon_7(s, p, src)) {

       const bitmask_t packed = pack_field(32, 39, 245, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    if (__instruction_expr_anon_7(s, p, src)) {

       const bitmask_t packed = pack_field(46, 48, 2, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    if (__instruction_expr_anon_7(s, p, src)) {

       const bitmask_t packed = pack_field(50, 52, 2, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    if (__instruction_expr_anon_7(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.round;
             const bitmask_t packed = pack_field(55, 56, fld, false);  /* ROUND */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* DM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_R = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U),  /* SRC_R */
    .HALF = __instruction_expr_anon_3(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat1_relative_const_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 9, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.src_type;
             const bitmask_t packed = pack_field(50, 52, fld, false);  /* SRC_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .DST_REL = !!(src->dsts[0]->flags & IR3_REG_RELATIV),  /* DST_REL */
 };

               bitmask_t tmp = encode__cat1_dst(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.dst_type;
             const bitmask_t packed = pack_field(46, 48, fld, false);  /* DST_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_RELATIV);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* DST_REL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(32, 39, 245, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(46, 48, 2, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(50, 52, 2, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    if (__instruction_expr_anon_8(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.src_type;
             const bitmask_t packed = pack_field(50, 52, fld, false);  /* SRC_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.dst_type;
             const bitmask_t packed = pack_field(46, 48, fld, false);  /* DST_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.round;
             const bitmask_t packed = pack_field(55, 56, fld, false);  /* ROUND */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* DM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .DST_REL = !!(src->dsts[0]->flags & IR3_REG_RELATIV),  /* DST_REL */
 };

               bitmask_t tmp = encode__cat1_dst(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_R = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U),  /* SRC_R */
    .HALF = __instruction_expr_anon_3(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat1_relative_const_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 9, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_RELATIV);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* DST_REL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.src_type;
             const bitmask_t packed = pack_field(50, 52, fld, false);  /* SRC_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.dst_type;
             const bitmask_t packed = pack_field(46, 48, fld, false);  /* DST_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.round;
             const bitmask_t packed = pack_field(55, 56, fld, false);  /* ROUND */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* DM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .DST_REL = !!(src->dsts[0]->flags & IR3_REG_RELATIV),  /* DST_REL */
 };

               bitmask_t tmp = encode__cat1_dst(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_R = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U),  /* SRC_R */
    .HALF = __instruction_expr_anon_3(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat1_relative_const_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 9, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_RELATIV);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* DST_REL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_R) || !!(src->flags & IR3_INSTR_U);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_17(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.src_type;
             const bitmask_t packed = pack_field(50, 52, fld, false);  /* SRC_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.dst_type;
             const bitmask_t packed = pack_field(46, 48, fld, false);  /* DST_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.round;
             const bitmask_t packed = pack_field(55, 56, fld, false);  /* ROUND */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* DM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .DST_HALF = __instruction_expr_anon_4(s, p, src),  /* DST_HALF */
 };

               bitmask_t tmp = encode__cat1_multi_dst(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST0 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .DST_HALF = __instruction_expr_anon_4(s, p, src),  /* DST_HALF */
 };

               bitmask_t tmp = encode__cat1_multi_dst(s, &bp, src->dsts[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(16, 23, fld, false);  /* DST1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HALF = __instruction_expr_anon_3(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat1_multi_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* SRC0 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HALF = __instruction_expr_anon_3(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat1_multi_src(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(8, 15, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_18(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.src_type;
             const bitmask_t packed = pack_field(50, 52, fld, false);  /* SRC_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.dst_type;
             const bitmask_t packed = pack_field(46, 48, fld, false);  /* DST_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.round;
             const bitmask_t packed = pack_field(55, 56, fld, false);  /* ROUND */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* DM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .DST_HALF = __instruction_expr_anon_4(s, p, src),  /* DST_HALF */
 };

               bitmask_t tmp = encode__cat1_multi_dst(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST0 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HALF = __instruction_expr_anon_3(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat1_multi_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* SRC0 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HALF = __instruction_expr_anon_3(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat1_multi_src(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(8, 15, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HALF = __instruction_expr_anon_3(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat1_multi_src(s, &bp, src->srcs[2]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(16, 23, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HALF = __instruction_expr_anon_3(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat1_multi_src(s, &bp, src->srcs[3]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(24, 31, fld, false);  /* SRC3 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_19(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.src_type;
             const bitmask_t packed = pack_field(50, 52, fld, false);  /* SRC_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.dst_type;
             const bitmask_t packed = pack_field(46, 48, fld, false);  /* DST_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat1.round;
             const bitmask_t packed = pack_field(55, 56, fld, false);  /* ROUND */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* DM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .DST_HALF = __instruction_expr_anon_4(s, p, src),  /* DST_HALF */
 };

               bitmask_t tmp = encode__cat1_multi_dst(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST0 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .DST_HALF = __instruction_expr_anon_4(s, p, src),  /* DST_HALF */
 };

               bitmask_t tmp = encode__cat1_multi_dst(s, &bp, src->dsts[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(8, 15, fld, false);  /* DST1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .DST_HALF = __instruction_expr_anon_4(s, p, src),  /* DST_HALF */
 };

               bitmask_t tmp = encode__cat1_multi_dst(s, &bp, src->dsts[2]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(16, 23, fld, false);  /* DST2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .DST_HALF = __instruction_expr_anon_4(s, p, src),  /* DST_HALF */
 };

               bitmask_t tmp = encode__cat1_multi_dst(s, &bp, src->dsts[3]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(24, 31, fld, false);  /* DST3 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HALF = __instruction_expr_anon_3(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat1_multi_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* SRC0 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_20(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .DST_REL = !!(src->dsts[0]->flags & IR3_REG_RELATIV),  /* DST_REL */
 };

               bitmask_t tmp = encode__cat1_dst(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_RELATIV);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* DST_REL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_21(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__instruction_expr___cat2_cat3_nop_encoding(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* SAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_EI);
             const bitmask_t packed = pack_field(47, 47, fld, false);  /* EI */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__dst_gpr_cat2(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_R = __instruction_expr___zero(s, p, src),  /* ZERO */
    .FULL = !(src->srcs[0]->flags & IR3_REG_HALF),  /* FULL */
 };

               bitmask_t tmp = encode__multisrc(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 15, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_R = __instruction_expr___zero(s, p, src),  /* ZERO */
    .FULL = !(src->srcs[0]->flags & IR3_REG_HALF),  /* FULL */
 };

               bitmask_t tmp = encode__multisrc(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(16, 31, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC1_R(src);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC1_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC2_R(src);
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* SRC2_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !(src->srcs[0]->flags & IR3_REG_HALF);
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = ((src->dsts[0]->num >> 2) == 62) ? 0 :
			!!((src->srcs[0]->flags ^ src->dsts[0]->flags) & IR3_REG_HALF);
             const bitmask_t packed = pack_field(46, 46, fld, false);  /* DST_CONV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* SAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_EI);
             const bitmask_t packed = pack_field(47, 47, fld, false);  /* EI */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__dst_gpr_cat2(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_R = extract_SRC1_R(src),  /* SRC1_R */
    .FULL = !(src->srcs[0]->flags & IR3_REG_HALF),  /* FULL */
 };

               bitmask_t tmp = encode__multisrc(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 15, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_R = extract_SRC2_R(src),  /* SRC2_R */
    .FULL = !(src->srcs[0]->flags & IR3_REG_HALF),  /* FULL */
 };

               bitmask_t tmp = encode__multisrc(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(16, 31, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC1_R(src);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC1_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC2_R(src);
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* SRC2_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !(src->srcs[0]->flags & IR3_REG_HALF);
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = ((src->dsts[0]->num >> 2) == 62) ? 0 :
			!!((src->srcs[0]->flags ^ src->dsts[0]->flags) & IR3_REG_HALF);
             const bitmask_t packed = pack_field(46, 46, fld, false);  /* DST_CONV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}




static bitmask_t
snippet__instruction_22(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__instruction_expr___cat2_cat3_nop_encoding(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* SAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_EI);
             const bitmask_t packed = pack_field(47, 47, fld, false);  /* EI */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__dst_gpr_cat2(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_R = __instruction_expr___zero(s, p, src),  /* ZERO */
    .FULL = !(src->srcs[0]->flags & IR3_REG_HALF),  /* FULL */
 };

               bitmask_t tmp = encode__multisrc(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 15, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_R = __instruction_expr___zero(s, p, src),  /* ZERO */
    .FULL = !(src->srcs[0]->flags & IR3_REG_HALF),  /* FULL */
 };

               bitmask_t tmp = encode__multisrc(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(16, 31, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC1_R(src);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC1_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC2_R(src);
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* SRC2_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !(src->srcs[0]->flags & IR3_REG_HALF);
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = ((src->dsts[0]->num >> 2) == 62) ? 0 :
			!!((src->srcs[0]->flags ^ src->dsts[0]->flags) & IR3_REG_HALF);
             const bitmask_t packed = pack_field(46, 46, fld, false);  /* DST_CONV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* SAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_EI);
             const bitmask_t packed = pack_field(47, 47, fld, false);  /* EI */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__dst_gpr_cat2(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_R = extract_SRC1_R(src),  /* SRC1_R */
    .FULL = !(src->srcs[0]->flags & IR3_REG_HALF),  /* FULL */
 };

               bitmask_t tmp = encode__multisrc(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 15, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_R = extract_SRC2_R(src),  /* SRC2_R */
    .FULL = !(src->srcs[0]->flags & IR3_REG_HALF),  /* FULL */
 };

               bitmask_t tmp = encode__multisrc(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(16, 31, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC1_R(src);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC1_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC2_R(src);
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* SRC2_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !(src->srcs[0]->flags & IR3_REG_HALF);
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = ((src->dsts[0]->num >> 2) == 62) ? 0 :
			!!((src->srcs[0]->flags ^ src->dsts[0]->flags) & IR3_REG_HALF);
             const bitmask_t packed = pack_field(46, 46, fld, false);  /* DST_CONV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}






static bitmask_t
snippet__instruction_23(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__instruction_expr___cat2_cat3_nop_encoding(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* SAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_EI);
             const bitmask_t packed = pack_field(47, 47, fld, false);  /* EI */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__dst_gpr_cat2(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_R = __instruction_expr___zero(s, p, src),  /* ZERO */
    .FULL = !(src->srcs[0]->flags & IR3_REG_HALF),  /* FULL */
 };

               bitmask_t tmp = encode__multisrc(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 15, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC1_R(src);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC1_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC2_R(src);
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* SRC2_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !(src->srcs[0]->flags & IR3_REG_HALF);
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = ((src->dsts[0]->num >> 2) == 62) ? 0 :
			!!((src->srcs[0]->flags ^ src->dsts[0]->flags) & IR3_REG_HALF);
             const bitmask_t packed = pack_field(46, 46, fld, false);  /* DST_CONV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* SAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_EI);
             const bitmask_t packed = pack_field(47, 47, fld, false);  /* EI */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__dst_gpr_cat2(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_R = extract_SRC1_R(src),  /* SRC1_R */
    .FULL = !(src->srcs[0]->flags & IR3_REG_HALF),  /* FULL */
 };

               bitmask_t tmp = encode__multisrc(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 15, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC1_R(src);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC1_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC2_R(src);
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* SRC2_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !(src->srcs[0]->flags & IR3_REG_HALF);
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = ((src->dsts[0]->num >> 2) == 62) ? 0 :
			!!((src->srcs[0]->flags ^ src->dsts[0]->flags) & IR3_REG_HALF);
             const bitmask_t packed = pack_field(46, 46, fld, false);  /* DST_CONV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_24(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__instruction_expr___cat2_cat3_nop_encoding(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* SAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat2.condition;
             const bitmask_t packed = pack_field(48, 50, fld, false);  /* COND */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_EI);
             const bitmask_t packed = pack_field(47, 47, fld, false);  /* EI */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__dst_gpr_cat2(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_R = __instruction_expr___zero(s, p, src),  /* ZERO */
    .FULL = !(src->srcs[0]->flags & IR3_REG_HALF),  /* FULL */
 };

               bitmask_t tmp = encode__multisrc(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 15, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_R = __instruction_expr___zero(s, p, src),  /* ZERO */
    .FULL = !(src->srcs[0]->flags & IR3_REG_HALF),  /* FULL */
 };

               bitmask_t tmp = encode__multisrc(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(16, 31, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC1_R(src);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC1_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC2_R(src);
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* SRC2_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !(src->srcs[0]->flags & IR3_REG_HALF);
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = ((src->dsts[0]->num >> 2) == 62) ? 0 :
			!!((src->srcs[0]->flags ^ src->dsts[0]->flags) & IR3_REG_HALF);
             const bitmask_t packed = pack_field(46, 46, fld, false);  /* DST_CONV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* SAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat2.condition;
             const bitmask_t packed = pack_field(48, 50, fld, false);  /* COND */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->dsts[0]->flags & IR3_REG_EI);
             const bitmask_t packed = pack_field(47, 47, fld, false);  /* EI */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__dst_gpr_cat2(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_R = extract_SRC1_R(src),  /* SRC1_R */
    .FULL = !(src->srcs[0]->flags & IR3_REG_HALF),  /* FULL */
 };

               bitmask_t tmp = encode__multisrc(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 15, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_R = extract_SRC2_R(src),  /* SRC2_R */
    .FULL = !(src->srcs[0]->flags & IR3_REG_HALF),  /* FULL */
 };

               bitmask_t tmp = encode__multisrc(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(16, 31, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC1_R(src);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC1_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC2_R(src);
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* SRC2_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !(src->srcs[0]->flags & IR3_REG_HALF);
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = ((src->dsts[0]->num >> 2) == 62) ? 0 :
			!!((src->srcs[0]->flags ^ src->dsts[0]->flags) & IR3_REG_HALF);
             const bitmask_t packed = pack_field(46, 46, fld, false);  /* DST_CONV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}














































static bitmask_t
snippet__instruction_25(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__instruction_expr___cat2_cat3_nop_encoding(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* SAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & (IR3_REG_FNEG | IR3_REG_SNEG | IR3_REG_BNOT));
             const bitmask_t packed = pack_field(14, 14, fld, false);  /* SRC1_NEG */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .IMMED_ENCODING = __instruction_expr___false(s, p, src),  /* IMMED_ENCODING */
 };

               bitmask_t tmp = encode__cat3_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 12, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[1]->flags & (IR3_REG_FNEG | IR3_REG_SNEG | IR3_REG_BNOT));
             const bitmask_t packed = pack_field(30, 30, fld, false);  /* SRC2_NEG */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(47, 54, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[2]->flags & (IR3_REG_FNEG | IR3_REG_SNEG | IR3_REG_BNOT));
             const bitmask_t packed = pack_field(31, 31, fld, false);  /* SRC3_NEG */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .IMMED_ENCODING = __instruction_expr___false(s, p, src),  /* IMMED_ENCODING */
 };

               bitmask_t tmp = encode__cat3_src(s, &bp, src->srcs[2]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(16, 28, fld, false);  /* SRC3 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC1_R(src);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC1_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC2_R(src);
             const bitmask_t packed = pack_field(15, 15, fld, false);  /* SRC2_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = ((src->dsts[0]->num >> 2) == 62) ? 0 :
			!!((src->srcs[0]->flags ^ src->dsts[0]->flags) & IR3_REG_HALF);
             const bitmask_t packed = pack_field(46, 46, fld, false);  /* DST_CONV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* SAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & (IR3_REG_FNEG | IR3_REG_SNEG | IR3_REG_BNOT));
             const bitmask_t packed = pack_field(14, 14, fld, false);  /* SRC1_NEG */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC1_R(src);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC1_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .IMMED_ENCODING = __instruction_expr___false(s, p, src),  /* IMMED_ENCODING */
 };

               bitmask_t tmp = encode__cat3_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 12, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[1]->flags & (IR3_REG_FNEG | IR3_REG_SNEG | IR3_REG_BNOT));
             const bitmask_t packed = pack_field(30, 30, fld, false);  /* SRC2_NEG */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC2_R(src);
             const bitmask_t packed = pack_field(15, 15, fld, false);  /* SRC2_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(47, 54, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[2]->flags & (IR3_REG_FNEG | IR3_REG_SNEG | IR3_REG_BNOT));
             const bitmask_t packed = pack_field(31, 31, fld, false);  /* SRC3_NEG */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[2]->flags & IR3_REG_R);
             const bitmask_t packed = pack_field(29, 29, fld, false);  /* SRC3_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .IMMED_ENCODING = __instruction_expr___false(s, p, src),  /* IMMED_ENCODING */
 };

               bitmask_t tmp = encode__cat3_src(s, &bp, src->srcs[2]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(16, 28, fld, false);  /* SRC3 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = ((src->dsts[0]->num >> 2) == 62) ? 0 :
			!!((src->srcs[0]->flags ^ src->dsts[0]->flags) & IR3_REG_HALF);
             const bitmask_t packed = pack_field(46, 46, fld, false);  /* DST_CONV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}






















static bitmask_t
snippet__instruction_26(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__instruction_expr___cat2_cat3_nop_encoding(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & (IR3_REG_FNEG | IR3_REG_SNEG | IR3_REG_BNOT));
             const bitmask_t packed = pack_field(14, 14, fld, false);  /* SRC1_NEG */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .IMMED_ENCODING = __instruction_expr___true(s, p, src),  /* IMMED_ENCODING */
 };

               bitmask_t tmp = encode__cat3_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 12, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[1]->flags & (IR3_REG_FNEG | IR3_REG_SNEG | IR3_REG_BNOT));
             const bitmask_t packed = pack_field(30, 30, fld, false);  /* SRC2_NEG */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(47, 54, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[2]->flags & (IR3_REG_FNEG | IR3_REG_SNEG | IR3_REG_BNOT));
             const bitmask_t packed = pack_field(31, 31, fld, false);  /* SRC3_NEG */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .IMMED_ENCODING = __instruction_expr___true(s, p, src),  /* IMMED_ENCODING */
 };

               bitmask_t tmp = encode__cat3_src(s, &bp, src->srcs[2]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(16, 28, fld, false);  /* SRC3 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC1_R(src);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC1_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC2_R(src);
             const bitmask_t packed = pack_field(15, 15, fld, false);  /* SRC2_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !(src->srcs[1]->flags & IR3_REG_HALF);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = ((src->dsts[0]->num >> 2) == 62) ? 0 :
			!!((src->srcs[1]->flags ^ src->dsts[0]->flags) & IR3_REG_HALF);
             const bitmask_t packed = pack_field(46, 46, fld, false);  /* DST_CONV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & (IR3_REG_FNEG | IR3_REG_SNEG | IR3_REG_BNOT));
             const bitmask_t packed = pack_field(14, 14, fld, false);  /* SRC1_NEG */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC1_R(src);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC1_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .IMMED_ENCODING = __instruction_expr___true(s, p, src),  /* IMMED_ENCODING */
 };

               bitmask_t tmp = encode__cat3_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 12, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[1]->flags & (IR3_REG_FNEG | IR3_REG_SNEG | IR3_REG_BNOT));
             const bitmask_t packed = pack_field(30, 30, fld, false);  /* SRC2_NEG */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC2_R(src);
             const bitmask_t packed = pack_field(15, 15, fld, false);  /* SRC2_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(47, 54, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[2]->flags & (IR3_REG_FNEG | IR3_REG_SNEG | IR3_REG_BNOT));
             const bitmask_t packed = pack_field(31, 31, fld, false);  /* SRC3_NEG */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[2]->flags & IR3_REG_R);
             const bitmask_t packed = pack_field(29, 29, fld, false);  /* SRC3_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .IMMED_ENCODING = __instruction_expr___true(s, p, src),  /* IMMED_ENCODING */
 };

               bitmask_t tmp = encode__cat3_src(s, &bp, src->srcs[2]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(16, 28, fld, false);  /* SRC3 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !(src->srcs[1]->flags & IR3_REG_HALF);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = ((src->dsts[0]->num >> 2) == 62) ? 0 :
			!!((src->srcs[1]->flags ^ src->dsts[0]->flags) & IR3_REG_HALF);
             const bitmask_t packed = pack_field(46, 46, fld, false);  /* DST_CONV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}







static bitmask_t
snippet__instruction_27(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__instruction_expr___cat2_cat3_nop_encoding(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat3.signedness;
             const bitmask_t packed = pack_field(14, 14, fld, false);  /* SRC_SIGN */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat3.packed;
             const bitmask_t packed = pack_field(30, 30, fld, false);  /* SRC_PACK */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* SAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat3_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 12, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(47, 54, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[2]->flags & (IR3_REG_FNEG | IR3_REG_SNEG | IR3_REG_BNOT));
             const bitmask_t packed = pack_field(31, 31, fld, false);  /* SRC3_NEG */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[2]->flags & IR3_REG_R);
             const bitmask_t packed = pack_field(29, 29, fld, false);  /* SRC3_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat3_src(s, &bp, src->srcs[2]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(16, 28, fld, false);  /* SRC3 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC1_R(src);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC1_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC2_R(src);
             const bitmask_t packed = pack_field(15, 15, fld, false);  /* SRC2_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat3.signedness;
             const bitmask_t packed = pack_field(14, 14, fld, false);  /* SRC_SIGN */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat3.packed;
             const bitmask_t packed = pack_field(30, 30, fld, false);  /* SRC_PACK */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* SAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC1_R(src);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC1_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat3_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 12, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC2_R(src);
             const bitmask_t packed = pack_field(15, 15, fld, false);  /* SRC2_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(47, 54, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[2]->flags & (IR3_REG_FNEG | IR3_REG_SNEG | IR3_REG_BNOT));
             const bitmask_t packed = pack_field(31, 31, fld, false);  /* SRC3_NEG */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[2]->flags & IR3_REG_R);
             const bitmask_t packed = pack_field(29, 29, fld, false);  /* SRC3_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat3_src(s, &bp, src->srcs[2]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(16, 28, fld, false);  /* SRC3 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}




static bitmask_t
snippet__instruction_28(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__instruction_expr___cat2_cat3_nop_encoding(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & (IR3_REG_FNEG | IR3_REG_SNEG | IR3_REG_BNOT));
             const bitmask_t packed = pack_field(14, 14, fld, false);  /* SRC1_NEG */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat3_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 12, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[1]->flags & (IR3_REG_FNEG | IR3_REG_SNEG | IR3_REG_BNOT));
             const bitmask_t packed = pack_field(30, 30, fld, false);  /* SRC2_NEG */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(47, 54, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .IMMED_ENCODING = __instruction_expr___true(s, p, src),  /* IMMED_ENCODING */
 };

               bitmask_t tmp = encode__cat3_src(s, &bp, src->srcs[2]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(16, 28, fld, false);  /* SRC3 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC1_R(src);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC1_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC2_R(src);
             const bitmask_t packed = pack_field(15, 15, fld, false);  /* SRC2_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = ((src->dsts[0]->num >> 2) == 62) ? 1 :
			!(src->dsts[0]->flags & IR3_REG_HALF);
             const bitmask_t packed = pack_field(46, 46, fld, false);  /* DST_FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !(src->srcs[0]->flags & IR3_REG_HALF);
             const bitmask_t packed = pack_field(31, 31, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & (IR3_REG_FNEG | IR3_REG_SNEG | IR3_REG_BNOT));
             const bitmask_t packed = pack_field(14, 14, fld, false);  /* SRC1_NEG */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC1_R(src);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC1_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat3_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 12, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[1]->flags & (IR3_REG_FNEG | IR3_REG_SNEG | IR3_REG_BNOT));
             const bitmask_t packed = pack_field(30, 30, fld, false);  /* SRC2_NEG */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_SRC2_R(src);
             const bitmask_t packed = pack_field(15, 15, fld, false);  /* SRC2_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(47, 54, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[2]->flags & IR3_REG_R);
             const bitmask_t packed = pack_field(29, 29, fld, false);  /* SRC3_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .IMMED_ENCODING = __instruction_expr___true(s, p, src),  /* IMMED_ENCODING */
 };

               bitmask_t tmp = encode__cat3_src(s, &bp, src->srcs[2]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(16, 28, fld, false);  /* SRC3 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = ((src->dsts[0]->num >> 2) == 62) ? 1 :
			!(src->dsts[0]->flags & IR3_REG_HALF);
             const bitmask_t packed = pack_field(46, 46, fld, false);  /* DST_FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !(src->srcs[0]->flags & IR3_REG_HALF);
             const bitmask_t packed = pack_field(31, 31, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}




static bitmask_t
snippet__instruction_29(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SAT);
             const bitmask_t packed = pack_field(42, 42, fld, false);  /* SAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->repeat;
             const bitmask_t packed = pack_field(40, 41, fld, false);  /* REPEAT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_UL);
             const bitmask_t packed = pack_field(45, 45, fld, false);  /* UL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_R = !!(src->srcs[0]->flags & IR3_REG_R),  /* SRC_R */
    .FULL = !(src->srcs[0]->flags & IR3_REG_HALF),  /* FULL */
 };

               bitmask_t tmp = encode__multisrc(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 15, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !(src->srcs[0]->flags & IR3_REG_HALF);
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = ((src->dsts[0]->num >> 2) == 62) ? 0 :
			!!((src->srcs[0]->flags ^ src->dsts[0]->flags) & IR3_REG_HALF);
             const bitmask_t packed = pack_field(46, 46, fld, false);  /* DST_CONV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_R);
             const bitmask_t packed = pack_field(43, 43, fld, false);  /* SRC_R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}












static bitmask_t
snippet__instruction_30(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__instruction_expr_anon_19(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_3D);
             const bitmask_t packed = pack_field(48, 48, fld, false);  /* 3D */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_A);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* A */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_O);
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* O */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_V);
             const bitmask_t packed = pack_field(50, 50, fld, false);  /* SV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .BINDLESS = __instruction_expr___cat5_s2enb_is_bindless(s, p, src),  /* BINDLESS */
    .BASE_HI = src->cat5.tex_base >> 1,  /* BASE_HI */
 };

               bitmask_t tmp = encode__cat5_s2en_bindless_base(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(47, 47, fld, false);  /* BASE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_INV_1D);
             const bitmask_t packed = pack_field(18, 18, fld, false);  /* 1D */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TYPE = __instruction_expr___true(s, p, src),  /* HAS_TYPE */
 };

               bitmask_t tmp = encode__cat5_type(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(44, 46, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->dsts[0]->wrmask;
             const bitmask_t packed = pack_field(40, 43, fld, false);  /* WRMASK */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___one(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat5_src1(s, &bp, extract_cat5_SRC(src, 0));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___one(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .O = !!(src->flags & IR3_INSTR_O),  /* O */
    .CLP = __instruction_expr___false(s, p, src),  /* CLP */
    .SRC2_IMM_OFFSET = !!(src->flags & IR3_INSTR_IMM_OFFSET),  /* SRC2_IMM_OFFSET */
 };

               bitmask_t tmp = encode__cat5_src2(s, &bp, extract_cat5_SRC(src, 1));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(9, 16, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .BINDLESS = __instruction_expr___cat5_s2enb_is_bindless(s, p, src),  /* BINDLESS */
    .DESC_MODE = extract_cat5_DESC_MODE(src),  /* DESC_MODE */
    .HAS_SAMP = __instruction_expr___true(s, p, src),  /* HAS_SAMP */
    .HAS_TEX = __instruction_expr___true(s, p, src),  /* HAS_TEX */
 };

               bitmask_t tmp = encode__cat5_src3(s, &bp, (src->srcs_count > 0) ? src->srcs[0] : NULL);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(21, 28, fld, false);  /* SRC3 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & (IR3_INSTR_S2EN | IR3_INSTR_B));
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* S2EN_BINDLESS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_DESC_MODE(src);
             const bitmask_t packed = pack_field(29, 31, fld, false);  /* DESC_MODE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat5.tex_base >> 1;
             const bitmask_t packed = pack_field(19, 20, fld, false);  /* BASE_HI */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_IMM_OFFSET);
             const bitmask_t packed = pack_field(53, 53, fld, false);  /* SRC2_IMM_OFFSET */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_FULL(src);
             const bitmask_t packed = pack_field(0, 0, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(19, 20, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(47, 47, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_3D);
             const bitmask_t packed = pack_field(48, 48, fld, false);  /* 3D */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_A);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* A */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_O);
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* O */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_V);
             const bitmask_t packed = pack_field(50, 50, fld, false);  /* SV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_INV_1D);
             const bitmask_t packed = pack_field(18, 18, fld, false);  /* 1D */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TYPE = __instruction_expr___true(s, p, src),  /* HAS_TYPE */
 };

               bitmask_t tmp = encode__cat5_type(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(44, 46, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->dsts[0]->wrmask;
             const bitmask_t packed = pack_field(40, 43, fld, false);  /* WRMASK */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___one(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat5_src1(s, &bp, extract_cat5_SRC(src, 0));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___one(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .O = !!(src->flags & IR3_INSTR_O),  /* O */
    .CLP = __instruction_expr___false(s, p, src),  /* CLP */
    .SRC2_IMM_OFFSET = !!(src->flags & IR3_INSTR_IMM_OFFSET),  /* SRC2_IMM_OFFSET */
 };

               bitmask_t tmp = encode__cat5_src2(s, &bp, extract_cat5_SRC(src, 1));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(9, 16, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_SAMP = __instruction_expr___true(s, p, src),  /* HAS_SAMP */
 };

               bitmask_t tmp = encode__cat5_samp(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(21, 24, fld, false);  /* SAMP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TEX = __instruction_expr___true(s, p, src),  /* HAS_TEX */
 };

               bitmask_t tmp = encode__cat5_tex(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(25, 31, fld, false);  /* TEX */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & (IR3_INSTR_S2EN | IR3_INSTR_B));
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* S2EN_BINDLESS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_IMM_OFFSET);
             const bitmask_t packed = pack_field(53, 53, fld, false);  /* SRC2_IMM_OFFSET */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_FULL(src);
             const bitmask_t packed = pack_field(0, 0, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(19, 20, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(47, 47, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_31(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__instruction_expr_anon_19(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_3D);
             const bitmask_t packed = pack_field(48, 48, fld, false);  /* 3D */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_A);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* A */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .BINDLESS = __instruction_expr___cat5_s2enb_is_bindless(s, p, src),  /* BINDLESS */
    .BASE_HI = src->cat5.tex_base >> 1,  /* BASE_HI */
 };

               bitmask_t tmp = encode__cat5_s2en_bindless_base(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(47, 47, fld, false);  /* BASE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TYPE = __instruction_expr___true(s, p, src),  /* HAS_TYPE */
 };

               bitmask_t tmp = encode__cat5_type(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(44, 46, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->dsts[0]->wrmask;
             const bitmask_t packed = pack_field(40, 43, fld, false);  /* WRMASK */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___one(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat5_src1(s, &bp, extract_cat5_SRC(src, 0));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___one(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .O = __instruction_expr___false(s, p, src),  /* O */
    .CLP = __instruction_expr___false(s, p, src),  /* CLP */
    .SRC2_IMM_OFFSET = __instruction_expr___false(s, p, src),  /* SRC2_IMM_OFFSET */
 };

               bitmask_t tmp = encode__cat5_src2(s, &bp, extract_cat5_SRC(src, 1));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(9, 16, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .BINDLESS = __instruction_expr___cat5_s2enb_is_bindless(s, p, src),  /* BINDLESS */
    .DESC_MODE = extract_cat5_DESC_MODE(src),  /* DESC_MODE */
    .HAS_SAMP = __instruction_expr___true(s, p, src),  /* HAS_SAMP */
    .HAS_TEX = __instruction_expr___true(s, p, src),  /* HAS_TEX */
 };

               bitmask_t tmp = encode__cat5_src3(s, &bp, (src->srcs_count > 0) ? src->srcs[0] : NULL);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(21, 28, fld, false);  /* SRC3 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & (IR3_INSTR_S2EN | IR3_INSTR_B));
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* S2EN_BINDLESS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_DESC_MODE(src);
             const bitmask_t packed = pack_field(29, 31, fld, false);  /* DESC_MODE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat5.tex_base >> 1;
             const bitmask_t packed = pack_field(19, 20, fld, false);  /* BASE_HI */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_FULL(src);
             const bitmask_t packed = pack_field(0, 0, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(19, 20, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(47, 47, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_3D);
             const bitmask_t packed = pack_field(48, 48, fld, false);  /* 3D */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_A);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* A */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TYPE = __instruction_expr___true(s, p, src),  /* HAS_TYPE */
 };

               bitmask_t tmp = encode__cat5_type(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(44, 46, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->dsts[0]->wrmask;
             const bitmask_t packed = pack_field(40, 43, fld, false);  /* WRMASK */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___one(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat5_src1(s, &bp, extract_cat5_SRC(src, 0));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___one(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .O = __instruction_expr___false(s, p, src),  /* O */
    .CLP = __instruction_expr___false(s, p, src),  /* CLP */
    .SRC2_IMM_OFFSET = __instruction_expr___false(s, p, src),  /* SRC2_IMM_OFFSET */
 };

               bitmask_t tmp = encode__cat5_src2(s, &bp, extract_cat5_SRC(src, 1));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(9, 16, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_SAMP = __instruction_expr___true(s, p, src),  /* HAS_SAMP */
 };

               bitmask_t tmp = encode__cat5_samp(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(21, 24, fld, false);  /* SAMP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TEX = __instruction_expr___true(s, p, src),  /* HAS_TEX */
 };

               bitmask_t tmp = encode__cat5_tex(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(25, 31, fld, false);  /* TEX */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & (IR3_INSTR_S2EN | IR3_INSTR_B));
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* S2EN_BINDLESS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_FULL(src);
             const bitmask_t packed = pack_field(0, 0, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(19, 20, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(47, 47, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_32(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__instruction_expr_anon_19(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_3D);
             const bitmask_t packed = pack_field(48, 48, fld, false);  /* 3D */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_A);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* A */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .BINDLESS = __instruction_expr___cat5_s2enb_is_bindless(s, p, src),  /* BINDLESS */
    .BASE_HI = src->cat5.tex_base >> 1,  /* BASE_HI */
 };

               bitmask_t tmp = encode__cat5_s2en_bindless_base(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(47, 47, fld, false);  /* BASE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TYPE = __instruction_expr___true(s, p, src),  /* HAS_TYPE */
 };

               bitmask_t tmp = encode__cat5_type(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(44, 46, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->dsts[0]->wrmask;
             const bitmask_t packed = pack_field(40, 43, fld, false);  /* WRMASK */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___two(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat5_src1(s, &bp, extract_cat5_SRC(src, 0));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___two(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .O = __instruction_expr___false(s, p, src),  /* O */
    .CLP = __instruction_expr___false(s, p, src),  /* CLP */
    .SRC2_IMM_OFFSET = __instruction_expr___false(s, p, src),  /* SRC2_IMM_OFFSET */
 };

               bitmask_t tmp = encode__cat5_src2(s, &bp, extract_cat5_SRC(src, 1));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(9, 16, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .BINDLESS = __instruction_expr___cat5_s2enb_is_bindless(s, p, src),  /* BINDLESS */
    .DESC_MODE = extract_cat5_DESC_MODE(src),  /* DESC_MODE */
    .HAS_SAMP = __instruction_expr___true(s, p, src),  /* HAS_SAMP */
    .HAS_TEX = __instruction_expr___true(s, p, src),  /* HAS_TEX */
 };

               bitmask_t tmp = encode__cat5_src3(s, &bp, (src->srcs_count > 0) ? src->srcs[0] : NULL);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(21, 28, fld, false);  /* SRC3 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & (IR3_INSTR_S2EN | IR3_INSTR_B));
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* S2EN_BINDLESS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_DESC_MODE(src);
             const bitmask_t packed = pack_field(29, 31, fld, false);  /* DESC_MODE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat5.tex_base >> 1;
             const bitmask_t packed = pack_field(19, 20, fld, false);  /* BASE_HI */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_FULL(src);
             const bitmask_t packed = pack_field(0, 0, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(19, 20, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(47, 47, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_3D);
             const bitmask_t packed = pack_field(48, 48, fld, false);  /* 3D */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_A);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* A */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TYPE = __instruction_expr___true(s, p, src),  /* HAS_TYPE */
 };

               bitmask_t tmp = encode__cat5_type(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(44, 46, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->dsts[0]->wrmask;
             const bitmask_t packed = pack_field(40, 43, fld, false);  /* WRMASK */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___two(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat5_src1(s, &bp, extract_cat5_SRC(src, 0));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___two(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .O = __instruction_expr___false(s, p, src),  /* O */
    .CLP = __instruction_expr___false(s, p, src),  /* CLP */
    .SRC2_IMM_OFFSET = __instruction_expr___false(s, p, src),  /* SRC2_IMM_OFFSET */
 };

               bitmask_t tmp = encode__cat5_src2(s, &bp, extract_cat5_SRC(src, 1));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(9, 16, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_SAMP = __instruction_expr___true(s, p, src),  /* HAS_SAMP */
 };

               bitmask_t tmp = encode__cat5_samp(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(21, 24, fld, false);  /* SAMP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TEX = __instruction_expr___true(s, p, src),  /* HAS_TEX */
 };

               bitmask_t tmp = encode__cat5_tex(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(25, 31, fld, false);  /* TEX */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & (IR3_INSTR_S2EN | IR3_INSTR_B));
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* S2EN_BINDLESS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_FULL(src);
             const bitmask_t packed = pack_field(0, 0, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(19, 20, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(47, 47, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_33(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__instruction_expr_anon_19(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_3D);
             const bitmask_t packed = pack_field(48, 48, fld, false);  /* 3D */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_A);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* A */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat5.match_mode;
             const bitmask_t packed = pack_field(50, 50, fld, false);  /* MATCH_MODE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .BINDLESS = __instruction_expr___cat5_s2enb_is_bindless(s, p, src),  /* BINDLESS */
    .BASE_HI = src->cat5.tex_base >> 1,  /* BASE_HI */
 };

               bitmask_t tmp = encode__cat5_s2en_bindless_base(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(47, 47, fld, false);  /* BASE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TYPE = __instruction_expr___true(s, p, src),  /* HAS_TYPE */
 };

               bitmask_t tmp = encode__cat5_type(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(44, 46, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->dsts[0]->wrmask;
             const bitmask_t packed = pack_field(40, 43, fld, false);  /* WRMASK */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___two(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat5_src1(s, &bp, extract_cat5_SRC(src, 0));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___two(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .O = __instruction_expr___false(s, p, src),  /* O */
    .CLP = __instruction_expr___false(s, p, src),  /* CLP */
    .SRC2_IMM_OFFSET = __instruction_expr___false(s, p, src),  /* SRC2_IMM_OFFSET */
 };

               bitmask_t tmp = encode__cat5_src2(s, &bp, extract_cat5_SRC(src, 1));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(9, 16, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .BINDLESS = __instruction_expr___cat5_s2enb_is_bindless(s, p, src),  /* BINDLESS */
    .DESC_MODE = extract_cat5_DESC_MODE(src),  /* DESC_MODE */
    .HAS_SAMP = __instruction_expr___false(s, p, src),  /* HAS_SAMP */
    .HAS_TEX = __instruction_expr___true(s, p, src),  /* HAS_TEX */
 };

               bitmask_t tmp = encode__cat5_src3(s, &bp, (src->srcs_count > 0) ? src->srcs[0] : NULL);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(21, 28, fld, false);  /* SRC3 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & (IR3_INSTR_S2EN | IR3_INSTR_B));
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* S2EN_BINDLESS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_DESC_MODE(src);
             const bitmask_t packed = pack_field(29, 31, fld, false);  /* DESC_MODE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat5.tex_base >> 1;
             const bitmask_t packed = pack_field(19, 20, fld, false);  /* BASE_HI */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_FULL(src);
             const bitmask_t packed = pack_field(0, 0, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(19, 20, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(47, 47, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_3D);
             const bitmask_t packed = pack_field(48, 48, fld, false);  /* 3D */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_A);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* A */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TYPE = __instruction_expr___true(s, p, src),  /* HAS_TYPE */
 };

               bitmask_t tmp = encode__cat5_type(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(44, 46, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->dsts[0]->wrmask;
             const bitmask_t packed = pack_field(40, 43, fld, false);  /* WRMASK */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___two(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat5_src1(s, &bp, extract_cat5_SRC(src, 0));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___two(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .O = __instruction_expr___false(s, p, src),  /* O */
    .CLP = __instruction_expr___false(s, p, src),  /* CLP */
    .SRC2_IMM_OFFSET = __instruction_expr___false(s, p, src),  /* SRC2_IMM_OFFSET */
 };

               bitmask_t tmp = encode__cat5_src2(s, &bp, extract_cat5_SRC(src, 1));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(9, 16, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_SAMP = __instruction_expr___false(s, p, src),  /* HAS_SAMP */
 };

               bitmask_t tmp = encode__cat5_samp(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(21, 24, fld, false);  /* SAMP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TEX = __instruction_expr___true(s, p, src),  /* HAS_TEX */
 };

               bitmask_t tmp = encode__cat5_tex(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(25, 31, fld, false);  /* TEX */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & (IR3_INSTR_S2EN | IR3_INSTR_B));
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* S2EN_BINDLESS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_FULL(src);
             const bitmask_t packed = pack_field(0, 0, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(19, 20, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(47, 47, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_34(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__instruction_expr_anon_19(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_3D);
             const bitmask_t packed = pack_field(48, 48, fld, false);  /* 3D */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_A);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* A */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_O);
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* O */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_P);
             const bitmask_t packed = pack_field(53, 53, fld, false);  /* P */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_S);
             const bitmask_t packed = pack_field(50, 50, fld, false);  /* SV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .BINDLESS = __instruction_expr___cat5_s2enb_is_bindless(s, p, src),  /* BINDLESS */
    .BASE_HI = src->cat5.tex_base >> 1,  /* BASE_HI */
 };

               bitmask_t tmp = encode__cat5_s2en_bindless_base(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(47, 47, fld, false);  /* BASE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_CLP);
             const bitmask_t packed = pack_field(18, 18, fld, false);  /* CLP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TYPE = __instruction_expr___true(s, p, src),  /* HAS_TYPE */
 };

               bitmask_t tmp = encode__cat5_type(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(44, 46, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->dsts[0]->wrmask;
             const bitmask_t packed = pack_field(40, 43, fld, false);  /* WRMASK */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___two(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat5_src1(s, &bp, extract_cat5_SRC(src, 0));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___two(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .O = !!(src->flags & IR3_INSTR_O),  /* O */
    .CLP = !!(src->flags & IR3_INSTR_CLP),  /* CLP */
    .SRC2_IMM_OFFSET = __instruction_expr___false(s, p, src),  /* SRC2_IMM_OFFSET */
 };

               bitmask_t tmp = encode__cat5_src2(s, &bp, extract_cat5_SRC(src, 1));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(9, 16, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .BINDLESS = __instruction_expr___cat5_s2enb_is_bindless(s, p, src),  /* BINDLESS */
    .DESC_MODE = extract_cat5_DESC_MODE(src),  /* DESC_MODE */
    .HAS_SAMP = __instruction_expr___true(s, p, src),  /* HAS_SAMP */
    .HAS_TEX = __instruction_expr___true(s, p, src),  /* HAS_TEX */
 };

               bitmask_t tmp = encode__cat5_src3(s, &bp, (src->srcs_count > 0) ? src->srcs[0] : NULL);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(21, 28, fld, false);  /* SRC3 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & (IR3_INSTR_S2EN | IR3_INSTR_B));
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* S2EN_BINDLESS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_DESC_MODE(src);
             const bitmask_t packed = pack_field(29, 31, fld, false);  /* DESC_MODE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat5.tex_base >> 1;
             const bitmask_t packed = pack_field(19, 20, fld, false);  /* BASE_HI */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_FULL(src);
             const bitmask_t packed = pack_field(0, 0, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(19, 20, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(47, 47, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_3D);
             const bitmask_t packed = pack_field(48, 48, fld, false);  /* 3D */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_A);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* A */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_O);
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* O */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_P);
             const bitmask_t packed = pack_field(53, 53, fld, false);  /* P */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_S);
             const bitmask_t packed = pack_field(50, 50, fld, false);  /* SV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_CLP);
             const bitmask_t packed = pack_field(18, 18, fld, false);  /* CLP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TYPE = __instruction_expr___true(s, p, src),  /* HAS_TYPE */
 };

               bitmask_t tmp = encode__cat5_type(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(44, 46, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->dsts[0]->wrmask;
             const bitmask_t packed = pack_field(40, 43, fld, false);  /* WRMASK */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___two(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat5_src1(s, &bp, extract_cat5_SRC(src, 0));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___two(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .O = !!(src->flags & IR3_INSTR_O),  /* O */
    .CLP = !!(src->flags & IR3_INSTR_CLP),  /* CLP */
    .SRC2_IMM_OFFSET = __instruction_expr___false(s, p, src),  /* SRC2_IMM_OFFSET */
 };

               bitmask_t tmp = encode__cat5_src2(s, &bp, extract_cat5_SRC(src, 1));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(9, 16, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_SAMP = __instruction_expr___true(s, p, src),  /* HAS_SAMP */
 };

               bitmask_t tmp = encode__cat5_samp(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(21, 24, fld, false);  /* SAMP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TEX = __instruction_expr___true(s, p, src),  /* HAS_TEX */
 };

               bitmask_t tmp = encode__cat5_tex(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(25, 31, fld, false);  /* TEX */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & (IR3_INSTR_S2EN | IR3_INSTR_B));
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* S2EN_BINDLESS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_FULL(src);
             const bitmask_t packed = pack_field(0, 0, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(19, 20, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(47, 47, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_35(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__instruction_expr_anon_19(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_3D);
             const bitmask_t packed = pack_field(48, 48, fld, false);  /* 3D */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_A);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* A */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_O);
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* O */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_P);
             const bitmask_t packed = pack_field(53, 53, fld, false);  /* P */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_S);
             const bitmask_t packed = pack_field(50, 50, fld, false);  /* SV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .BINDLESS = __instruction_expr___cat5_s2enb_is_bindless(s, p, src),  /* BINDLESS */
    .BASE_HI = src->cat5.tex_base >> 1,  /* BASE_HI */
 };

               bitmask_t tmp = encode__cat5_s2en_bindless_base(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(47, 47, fld, false);  /* BASE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_CLP);
             const bitmask_t packed = pack_field(18, 18, fld, false);  /* CLP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TYPE = __instruction_expr___true(s, p, src),  /* HAS_TYPE */
 };

               bitmask_t tmp = encode__cat5_type(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(44, 46, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->dsts[0]->wrmask;
             const bitmask_t packed = pack_field(40, 43, fld, false);  /* WRMASK */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___one(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat5_src1(s, &bp, extract_cat5_SRC(src, 0));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___one(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .O = !!(src->flags & IR3_INSTR_O),  /* O */
    .CLP = !!(src->flags & IR3_INSTR_CLP),  /* CLP */
    .SRC2_IMM_OFFSET = __instruction_expr___false(s, p, src),  /* SRC2_IMM_OFFSET */
 };

               bitmask_t tmp = encode__cat5_src2(s, &bp, extract_cat5_SRC(src, 1));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(9, 16, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .BINDLESS = __instruction_expr___cat5_s2enb_is_bindless(s, p, src),  /* BINDLESS */
    .DESC_MODE = extract_cat5_DESC_MODE(src),  /* DESC_MODE */
    .HAS_SAMP = __instruction_expr___true(s, p, src),  /* HAS_SAMP */
    .HAS_TEX = __instruction_expr___true(s, p, src),  /* HAS_TEX */
 };

               bitmask_t tmp = encode__cat5_src3(s, &bp, (src->srcs_count > 0) ? src->srcs[0] : NULL);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(21, 28, fld, false);  /* SRC3 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & (IR3_INSTR_S2EN | IR3_INSTR_B));
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* S2EN_BINDLESS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_DESC_MODE(src);
             const bitmask_t packed = pack_field(29, 31, fld, false);  /* DESC_MODE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat5.tex_base >> 1;
             const bitmask_t packed = pack_field(19, 20, fld, false);  /* BASE_HI */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_FULL(src);
             const bitmask_t packed = pack_field(0, 0, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(19, 20, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(47, 47, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_3D);
             const bitmask_t packed = pack_field(48, 48, fld, false);  /* 3D */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_A);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* A */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_O);
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* O */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_P);
             const bitmask_t packed = pack_field(53, 53, fld, false);  /* P */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_S);
             const bitmask_t packed = pack_field(50, 50, fld, false);  /* SV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_CLP);
             const bitmask_t packed = pack_field(18, 18, fld, false);  /* CLP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TYPE = __instruction_expr___true(s, p, src),  /* HAS_TYPE */
 };

               bitmask_t tmp = encode__cat5_type(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(44, 46, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->dsts[0]->wrmask;
             const bitmask_t packed = pack_field(40, 43, fld, false);  /* WRMASK */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___one(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat5_src1(s, &bp, extract_cat5_SRC(src, 0));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___one(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .O = !!(src->flags & IR3_INSTR_O),  /* O */
    .CLP = !!(src->flags & IR3_INSTR_CLP),  /* CLP */
    .SRC2_IMM_OFFSET = __instruction_expr___false(s, p, src),  /* SRC2_IMM_OFFSET */
 };

               bitmask_t tmp = encode__cat5_src2(s, &bp, extract_cat5_SRC(src, 1));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(9, 16, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_SAMP = __instruction_expr___true(s, p, src),  /* HAS_SAMP */
 };

               bitmask_t tmp = encode__cat5_samp(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(21, 24, fld, false);  /* SAMP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TEX = __instruction_expr___true(s, p, src),  /* HAS_TEX */
 };

               bitmask_t tmp = encode__cat5_tex(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(25, 31, fld, false);  /* TEX */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & (IR3_INSTR_S2EN | IR3_INSTR_B));
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* S2EN_BINDLESS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_FULL(src);
             const bitmask_t packed = pack_field(0, 0, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(19, 20, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(47, 47, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}










static bitmask_t
snippet__instruction_36(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__instruction_expr_anon_19(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_3D);
             const bitmask_t packed = pack_field(48, 48, fld, false);  /* 3D */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_A);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* A */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_O);
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* O */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_P);
             const bitmask_t packed = pack_field(53, 53, fld, false);  /* P */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_S);
             const bitmask_t packed = pack_field(50, 50, fld, false);  /* SV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .BINDLESS = __instruction_expr___cat5_s2enb_is_bindless(s, p, src),  /* BINDLESS */
    .BASE_HI = src->cat5.tex_base >> 1,  /* BASE_HI */
 };

               bitmask_t tmp = encode__cat5_s2en_bindless_base(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(47, 47, fld, false);  /* BASE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_CLP);
             const bitmask_t packed = pack_field(18, 18, fld, false);  /* CLP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TYPE = __instruction_expr___true(s, p, src),  /* HAS_TYPE */
 };

               bitmask_t tmp = encode__cat5_type(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(44, 46, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->dsts[0]->wrmask;
             const bitmask_t packed = pack_field(40, 43, fld, false);  /* WRMASK */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___one(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat5_src1(s, &bp, extract_cat5_SRC(src, 0));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___one(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .O = !!(src->flags & IR3_INSTR_O),  /* O */
    .CLP = !!(src->flags & IR3_INSTR_CLP),  /* CLP */
    .SRC2_IMM_OFFSET = __instruction_expr___false(s, p, src),  /* SRC2_IMM_OFFSET */
 };

               bitmask_t tmp = encode__cat5_src2(s, &bp, extract_cat5_SRC(src, 1));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(9, 16, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .BINDLESS = __instruction_expr___cat5_s2enb_is_bindless(s, p, src),  /* BINDLESS */
    .DESC_MODE = extract_cat5_DESC_MODE(src),  /* DESC_MODE */
    .HAS_SAMP = __instruction_expr___false(s, p, src),  /* HAS_SAMP */
    .HAS_TEX = __instruction_expr___true(s, p, src),  /* HAS_TEX */
 };

               bitmask_t tmp = encode__cat5_src3(s, &bp, (src->srcs_count > 0) ? src->srcs[0] : NULL);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(21, 28, fld, false);  /* SRC3 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & (IR3_INSTR_S2EN | IR3_INSTR_B));
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* S2EN_BINDLESS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_DESC_MODE(src);
             const bitmask_t packed = pack_field(29, 31, fld, false);  /* DESC_MODE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat5.tex_base >> 1;
             const bitmask_t packed = pack_field(19, 20, fld, false);  /* BASE_HI */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_FULL(src);
             const bitmask_t packed = pack_field(0, 0, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(19, 20, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(47, 47, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_3D);
             const bitmask_t packed = pack_field(48, 48, fld, false);  /* 3D */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_A);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* A */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_O);
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* O */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_P);
             const bitmask_t packed = pack_field(53, 53, fld, false);  /* P */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_S);
             const bitmask_t packed = pack_field(50, 50, fld, false);  /* SV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_CLP);
             const bitmask_t packed = pack_field(18, 18, fld, false);  /* CLP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TYPE = __instruction_expr___true(s, p, src),  /* HAS_TYPE */
 };

               bitmask_t tmp = encode__cat5_type(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(44, 46, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->dsts[0]->wrmask;
             const bitmask_t packed = pack_field(40, 43, fld, false);  /* WRMASK */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___one(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat5_src1(s, &bp, extract_cat5_SRC(src, 0));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___one(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .O = !!(src->flags & IR3_INSTR_O),  /* O */
    .CLP = !!(src->flags & IR3_INSTR_CLP),  /* CLP */
    .SRC2_IMM_OFFSET = __instruction_expr___false(s, p, src),  /* SRC2_IMM_OFFSET */
 };

               bitmask_t tmp = encode__cat5_src2(s, &bp, extract_cat5_SRC(src, 1));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(9, 16, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_SAMP = __instruction_expr___false(s, p, src),  /* HAS_SAMP */
 };

               bitmask_t tmp = encode__cat5_samp(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(21, 24, fld, false);  /* SAMP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TEX = __instruction_expr___true(s, p, src),  /* HAS_TEX */
 };

               bitmask_t tmp = encode__cat5_tex(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(25, 31, fld, false);  /* TEX */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & (IR3_INSTR_S2EN | IR3_INSTR_B));
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* S2EN_BINDLESS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_FULL(src);
             const bitmask_t packed = pack_field(0, 0, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(19, 20, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(47, 47, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_37(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__instruction_expr_anon_19(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_3D);
             const bitmask_t packed = pack_field(48, 48, fld, false);  /* 3D */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_A);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* A */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_O);
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* O */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_P);
             const bitmask_t packed = pack_field(53, 53, fld, false);  /* P */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_S);
             const bitmask_t packed = pack_field(50, 50, fld, false);  /* SV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .BINDLESS = __instruction_expr___cat5_s2enb_is_bindless(s, p, src),  /* BINDLESS */
    .BASE_HI = src->cat5.tex_base >> 1,  /* BASE_HI */
 };

               bitmask_t tmp = encode__cat5_s2en_bindless_base(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(47, 47, fld, false);  /* BASE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_CLP);
             const bitmask_t packed = pack_field(18, 18, fld, false);  /* CLP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TYPE = __instruction_expr___true(s, p, src),  /* HAS_TYPE */
 };

               bitmask_t tmp = encode__cat5_type(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(44, 46, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->dsts[0]->wrmask;
             const bitmask_t packed = pack_field(40, 43, fld, false);  /* WRMASK */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___zero(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat5_src1(s, &bp, extract_cat5_SRC(src, 0));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___zero(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .O = !!(src->flags & IR3_INSTR_O),  /* O */
    .CLP = !!(src->flags & IR3_INSTR_CLP),  /* CLP */
    .SRC2_IMM_OFFSET = __instruction_expr___false(s, p, src),  /* SRC2_IMM_OFFSET */
 };

               bitmask_t tmp = encode__cat5_src2(s, &bp, extract_cat5_SRC(src, 1));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(9, 16, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .BINDLESS = __instruction_expr___cat5_s2enb_is_bindless(s, p, src),  /* BINDLESS */
    .DESC_MODE = extract_cat5_DESC_MODE(src),  /* DESC_MODE */
    .HAS_SAMP = __instruction_expr___false(s, p, src),  /* HAS_SAMP */
    .HAS_TEX = __instruction_expr___true(s, p, src),  /* HAS_TEX */
 };

               bitmask_t tmp = encode__cat5_src3(s, &bp, (src->srcs_count > 0) ? src->srcs[0] : NULL);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(21, 28, fld, false);  /* SRC3 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & (IR3_INSTR_S2EN | IR3_INSTR_B));
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* S2EN_BINDLESS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_DESC_MODE(src);
             const bitmask_t packed = pack_field(29, 31, fld, false);  /* DESC_MODE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat5.tex_base >> 1;
             const bitmask_t packed = pack_field(19, 20, fld, false);  /* BASE_HI */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_FULL(src);
             const bitmask_t packed = pack_field(0, 0, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(19, 20, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(47, 47, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_3D);
             const bitmask_t packed = pack_field(48, 48, fld, false);  /* 3D */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_A);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* A */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_O);
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* O */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_P);
             const bitmask_t packed = pack_field(53, 53, fld, false);  /* P */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_S);
             const bitmask_t packed = pack_field(50, 50, fld, false);  /* SV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_CLP);
             const bitmask_t packed = pack_field(18, 18, fld, false);  /* CLP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TYPE = __instruction_expr___true(s, p, src),  /* HAS_TYPE */
 };

               bitmask_t tmp = encode__cat5_type(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(44, 46, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->dsts[0]->wrmask;
             const bitmask_t packed = pack_field(40, 43, fld, false);  /* WRMASK */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___zero(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat5_src1(s, &bp, extract_cat5_SRC(src, 0));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___zero(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .O = !!(src->flags & IR3_INSTR_O),  /* O */
    .CLP = !!(src->flags & IR3_INSTR_CLP),  /* CLP */
    .SRC2_IMM_OFFSET = __instruction_expr___false(s, p, src),  /* SRC2_IMM_OFFSET */
 };

               bitmask_t tmp = encode__cat5_src2(s, &bp, extract_cat5_SRC(src, 1));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(9, 16, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_SAMP = __instruction_expr___false(s, p, src),  /* HAS_SAMP */
 };

               bitmask_t tmp = encode__cat5_samp(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(21, 24, fld, false);  /* SAMP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TEX = __instruction_expr___true(s, p, src),  /* HAS_TEX */
 };

               bitmask_t tmp = encode__cat5_tex(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(25, 31, fld, false);  /* TEX */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & (IR3_INSTR_S2EN | IR3_INSTR_B));
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* S2EN_BINDLESS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_FULL(src);
             const bitmask_t packed = pack_field(0, 0, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(19, 20, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(47, 47, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}





static bitmask_t
snippet__instruction_38(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__instruction_expr_anon_19(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_3D);
             const bitmask_t packed = pack_field(48, 48, fld, false);  /* 3D */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_A);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* A */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_O);
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* O */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_P);
             const bitmask_t packed = pack_field(53, 53, fld, false);  /* P */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_S);
             const bitmask_t packed = pack_field(50, 50, fld, false);  /* SV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .BINDLESS = __instruction_expr___cat5_s2enb_is_bindless(s, p, src),  /* BINDLESS */
    .BASE_HI = src->cat5.tex_base >> 1,  /* BASE_HI */
 };

               bitmask_t tmp = encode__cat5_s2en_bindless_base(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(47, 47, fld, false);  /* BASE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_CLP);
             const bitmask_t packed = pack_field(18, 18, fld, false);  /* CLP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TYPE = __instruction_expr___true(s, p, src),  /* HAS_TYPE */
 };

               bitmask_t tmp = encode__cat5_type(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(44, 46, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->dsts[0]->wrmask;
             const bitmask_t packed = pack_field(40, 43, fld, false);  /* WRMASK */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___one(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat5_src1(s, &bp, extract_cat5_SRC(src, 0));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___one(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .O = !!(src->flags & IR3_INSTR_O),  /* O */
    .CLP = !!(src->flags & IR3_INSTR_CLP),  /* CLP */
    .SRC2_IMM_OFFSET = __instruction_expr___false(s, p, src),  /* SRC2_IMM_OFFSET */
 };

               bitmask_t tmp = encode__cat5_src2(s, &bp, extract_cat5_SRC(src, 1));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(9, 16, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .BINDLESS = __instruction_expr___cat5_s2enb_is_bindless(s, p, src),  /* BINDLESS */
    .DESC_MODE = extract_cat5_DESC_MODE(src),  /* DESC_MODE */
    .HAS_SAMP = __instruction_expr___false(s, p, src),  /* HAS_SAMP */
    .HAS_TEX = __instruction_expr___false(s, p, src),  /* HAS_TEX */
 };

               bitmask_t tmp = encode__cat5_src3(s, &bp, (src->srcs_count > 0) ? src->srcs[0] : NULL);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(21, 28, fld, false);  /* SRC3 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & (IR3_INSTR_S2EN | IR3_INSTR_B));
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* S2EN_BINDLESS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_DESC_MODE(src);
             const bitmask_t packed = pack_field(29, 31, fld, false);  /* DESC_MODE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat5.tex_base >> 1;
             const bitmask_t packed = pack_field(19, 20, fld, false);  /* BASE_HI */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_FULL(src);
             const bitmask_t packed = pack_field(0, 0, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(19, 20, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(47, 47, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_3D);
             const bitmask_t packed = pack_field(48, 48, fld, false);  /* 3D */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_A);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* A */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_O);
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* O */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_P);
             const bitmask_t packed = pack_field(53, 53, fld, false);  /* P */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_S);
             const bitmask_t packed = pack_field(50, 50, fld, false);  /* SV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_CLP);
             const bitmask_t packed = pack_field(18, 18, fld, false);  /* CLP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TYPE = __instruction_expr___true(s, p, src),  /* HAS_TYPE */
 };

               bitmask_t tmp = encode__cat5_type(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(44, 46, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->dsts[0]->wrmask;
             const bitmask_t packed = pack_field(40, 43, fld, false);  /* WRMASK */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___one(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat5_src1(s, &bp, extract_cat5_SRC(src, 0));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___one(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .O = !!(src->flags & IR3_INSTR_O),  /* O */
    .CLP = !!(src->flags & IR3_INSTR_CLP),  /* CLP */
    .SRC2_IMM_OFFSET = __instruction_expr___false(s, p, src),  /* SRC2_IMM_OFFSET */
 };

               bitmask_t tmp = encode__cat5_src2(s, &bp, extract_cat5_SRC(src, 1));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(9, 16, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_SAMP = __instruction_expr___false(s, p, src),  /* HAS_SAMP */
 };

               bitmask_t tmp = encode__cat5_samp(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(21, 24, fld, false);  /* SAMP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TEX = __instruction_expr___false(s, p, src),  /* HAS_TEX */
 };

               bitmask_t tmp = encode__cat5_tex(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(25, 31, fld, false);  /* TEX */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & (IR3_INSTR_S2EN | IR3_INSTR_B));
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* S2EN_BINDLESS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_FULL(src);
             const bitmask_t packed = pack_field(0, 0, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(19, 20, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(47, 47, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}












static bitmask_t
snippet__instruction_39(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__instruction_expr_anon_19(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_3D);
             const bitmask_t packed = pack_field(48, 48, fld, false);  /* 3D */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_A);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* A */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_O);
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* O */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_P);
             const bitmask_t packed = pack_field(53, 53, fld, false);  /* P */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_S);
             const bitmask_t packed = pack_field(50, 50, fld, false);  /* SV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .BINDLESS = __instruction_expr___cat5_s2enb_is_bindless(s, p, src),  /* BINDLESS */
    .BASE_HI = src->cat5.tex_base >> 1,  /* BASE_HI */
 };

               bitmask_t tmp = encode__cat5_s2en_bindless_base(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(47, 47, fld, false);  /* BASE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_CLP);
             const bitmask_t packed = pack_field(18, 18, fld, false);  /* CLP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TYPE = __instruction_expr___false(s, p, src),  /* HAS_TYPE */
 };

               bitmask_t tmp = encode__cat5_type(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(44, 46, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->dsts[0]->wrmask;
             const bitmask_t packed = pack_field(40, 43, fld, false);  /* WRMASK */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___one(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat5_src1(s, &bp, extract_cat5_SRC(src, 0));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___one(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .O = !!(src->flags & IR3_INSTR_O),  /* O */
    .CLP = !!(src->flags & IR3_INSTR_CLP),  /* CLP */
    .SRC2_IMM_OFFSET = __instruction_expr___false(s, p, src),  /* SRC2_IMM_OFFSET */
 };

               bitmask_t tmp = encode__cat5_src2(s, &bp, extract_cat5_SRC(src, 1));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(9, 16, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .BINDLESS = __instruction_expr___cat5_s2enb_is_bindless(s, p, src),  /* BINDLESS */
    .DESC_MODE = extract_cat5_DESC_MODE(src),  /* DESC_MODE */
    .HAS_SAMP = __instruction_expr___false(s, p, src),  /* HAS_SAMP */
    .HAS_TEX = __instruction_expr___false(s, p, src),  /* HAS_TEX */
 };

               bitmask_t tmp = encode__cat5_src3(s, &bp, (src->srcs_count > 0) ? src->srcs[0] : NULL);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(21, 28, fld, false);  /* SRC3 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & (IR3_INSTR_S2EN | IR3_INSTR_B));
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* S2EN_BINDLESS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_DESC_MODE(src);
             const bitmask_t packed = pack_field(29, 31, fld, false);  /* DESC_MODE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat5.tex_base >> 1;
             const bitmask_t packed = pack_field(19, 20, fld, false);  /* BASE_HI */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_FULL(src);
             const bitmask_t packed = pack_field(0, 0, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(19, 20, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(47, 47, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_3D);
             const bitmask_t packed = pack_field(48, 48, fld, false);  /* 3D */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_A);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* A */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_O);
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* O */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_P);
             const bitmask_t packed = pack_field(53, 53, fld, false);  /* P */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_S);
             const bitmask_t packed = pack_field(50, 50, fld, false);  /* SV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_CLP);
             const bitmask_t packed = pack_field(18, 18, fld, false);  /* CLP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TYPE = __instruction_expr___false(s, p, src),  /* HAS_TYPE */
 };

               bitmask_t tmp = encode__cat5_type(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(44, 46, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->dsts[0]->wrmask;
             const bitmask_t packed = pack_field(40, 43, fld, false);  /* WRMASK */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___one(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat5_src1(s, &bp, extract_cat5_SRC(src, 0));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___one(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .O = !!(src->flags & IR3_INSTR_O),  /* O */
    .CLP = !!(src->flags & IR3_INSTR_CLP),  /* CLP */
    .SRC2_IMM_OFFSET = __instruction_expr___false(s, p, src),  /* SRC2_IMM_OFFSET */
 };

               bitmask_t tmp = encode__cat5_src2(s, &bp, extract_cat5_SRC(src, 1));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(9, 16, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_SAMP = __instruction_expr___false(s, p, src),  /* HAS_SAMP */
 };

               bitmask_t tmp = encode__cat5_samp(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(21, 24, fld, false);  /* SAMP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TEX = __instruction_expr___false(s, p, src),  /* HAS_TEX */
 };

               bitmask_t tmp = encode__cat5_tex(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(25, 31, fld, false);  /* TEX */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & (IR3_INSTR_S2EN | IR3_INSTR_B));
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* S2EN_BINDLESS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_FULL(src);
             const bitmask_t packed = pack_field(0, 0, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(19, 20, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(47, 47, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}





static bitmask_t
snippet__instruction_40(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__instruction_expr_anon_19(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_3D);
             const bitmask_t packed = pack_field(48, 48, fld, false);  /* 3D */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_A);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* A */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_O);
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* O */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_P);
             const bitmask_t packed = pack_field(53, 53, fld, false);  /* P */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_S);
             const bitmask_t packed = pack_field(50, 50, fld, false);  /* SV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .BINDLESS = __instruction_expr___cat5_s2enb_is_bindless(s, p, src),  /* BINDLESS */
    .BASE_HI = src->cat5.tex_base >> 1,  /* BASE_HI */
 };

               bitmask_t tmp = encode__cat5_s2en_bindless_base(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(47, 47, fld, false);  /* BASE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_CLP);
             const bitmask_t packed = pack_field(18, 18, fld, false);  /* CLP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TYPE = __instruction_expr___true(s, p, src),  /* HAS_TYPE */
 };

               bitmask_t tmp = encode__cat5_type(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(44, 46, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->dsts[0]->wrmask;
             const bitmask_t packed = pack_field(40, 43, fld, false);  /* WRMASK */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___zero(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat5_src1(s, &bp, extract_cat5_SRC(src, 0));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___zero(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .O = !!(src->flags & IR3_INSTR_O),  /* O */
    .CLP = !!(src->flags & IR3_INSTR_CLP),  /* CLP */
    .SRC2_IMM_OFFSET = __instruction_expr___false(s, p, src),  /* SRC2_IMM_OFFSET */
 };

               bitmask_t tmp = encode__cat5_src2(s, &bp, extract_cat5_SRC(src, 1));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(9, 16, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .BINDLESS = __instruction_expr___cat5_s2enb_is_bindless(s, p, src),  /* BINDLESS */
    .DESC_MODE = extract_cat5_DESC_MODE(src),  /* DESC_MODE */
    .HAS_SAMP = __instruction_expr___false(s, p, src),  /* HAS_SAMP */
    .HAS_TEX = __instruction_expr___false(s, p, src),  /* HAS_TEX */
 };

               bitmask_t tmp = encode__cat5_src3(s, &bp, (src->srcs_count > 0) ? src->srcs[0] : NULL);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(21, 28, fld, false);  /* SRC3 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & (IR3_INSTR_S2EN | IR3_INSTR_B));
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* S2EN_BINDLESS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_DESC_MODE(src);
             const bitmask_t packed = pack_field(29, 31, fld, false);  /* DESC_MODE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat5.tex_base >> 1;
             const bitmask_t packed = pack_field(19, 20, fld, false);  /* BASE_HI */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_FULL(src);
             const bitmask_t packed = pack_field(0, 0, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(19, 20, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(47, 47, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_3D);
             const bitmask_t packed = pack_field(48, 48, fld, false);  /* 3D */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_A);
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* A */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_O);
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* O */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_P);
             const bitmask_t packed = pack_field(53, 53, fld, false);  /* P */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_S);
             const bitmask_t packed = pack_field(50, 50, fld, false);  /* SV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_CLP);
             const bitmask_t packed = pack_field(18, 18, fld, false);  /* CLP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TYPE = __instruction_expr___true(s, p, src),  /* HAS_TYPE */
 };

               bitmask_t tmp = encode__cat5_type(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(44, 46, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->dsts[0]->wrmask;
             const bitmask_t packed = pack_field(40, 43, fld, false);  /* WRMASK */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___zero(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat5_src1(s, &bp, extract_cat5_SRC(src, 0));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___zero(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .O = !!(src->flags & IR3_INSTR_O),  /* O */
    .CLP = !!(src->flags & IR3_INSTR_CLP),  /* CLP */
    .SRC2_IMM_OFFSET = __instruction_expr___false(s, p, src),  /* SRC2_IMM_OFFSET */
 };

               bitmask_t tmp = encode__cat5_src2(s, &bp, extract_cat5_SRC(src, 1));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(9, 16, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_SAMP = __instruction_expr___false(s, p, src),  /* HAS_SAMP */
 };

               bitmask_t tmp = encode__cat5_samp(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(21, 24, fld, false);  /* SAMP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TEX = __instruction_expr___false(s, p, src),  /* HAS_TEX */
 };

               bitmask_t tmp = encode__cat5_tex(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(25, 31, fld, false);  /* TEX */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & (IR3_INSTR_S2EN | IR3_INSTR_B));
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* S2EN_BINDLESS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_FULL(src);
             const bitmask_t packed = pack_field(0, 0, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(19, 20, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    {

       const bitmask_t packed = pack_field(47, 47, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_41(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_42(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TYPE = __instruction_expr___true(s, p, src),  /* HAS_TYPE */
 };

               bitmask_t tmp = encode__cat5_type(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(44, 46, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->dsts[0]->wrmask;
             const bitmask_t packed = pack_field(40, 43, fld, false);  /* WRMASK */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___one(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat5_src1(s, &bp, extract_cat5_SRC(src, 0));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = util_logbase2(src->cat5.cluster_size) - 1;
             const bitmask_t packed = pack_field(19, 20, fld, false);  /* W */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_FULL(src);
             const bitmask_t packed = pack_field(0, 0, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(47, 47, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_43(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TYPE = __instruction_expr___true(s, p, src),  /* HAS_TYPE */
 };

               bitmask_t tmp = encode__cat5_type(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(44, 46, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->dsts[0]->wrmask;
             const bitmask_t packed = pack_field(40, 43, fld, false);  /* WRMASK */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___two(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat5_src1(s, &bp, extract_cat5_SRC(src, 0));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___two(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .O = !!(src->flags & IR3_INSTR_O),  /* O */
    .CLP = __instruction_expr___false(s, p, src),  /* CLP */
    .SRC2_IMM_OFFSET = __instruction_expr___false(s, p, src),  /* SRC2_IMM_OFFSET */
 };

               bitmask_t tmp = encode__cat5_src2(s, &bp, extract_cat5_SRC(src, 1));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(9, 16, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_O);
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* O */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_FULL(src);
             const bitmask_t packed = pack_field(0, 0, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(47, 47, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_44(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .HAS_TYPE = __instruction_expr___true(s, p, src),  /* HAS_TYPE */
 };

               bitmask_t tmp = encode__cat5_type(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(44, 46, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->dsts[0]->wrmask;
             const bitmask_t packed = pack_field(40, 43, fld, false);  /* WRMASK */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___one(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
 };

               bitmask_t tmp = encode__cat5_src1(s, &bp, extract_cat5_SRC(src, 0));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .NUM_SRC = __instruction_expr___one(s, p, src),  /* NUM_SRC */
    .HALF = __instruction_expr___multisrc_half(s, p, src),  /* HALF */
    .O = !!(src->flags & IR3_INSTR_O),  /* O */
    .CLP = __instruction_expr___false(s, p, src),  /* CLP */
    .SRC2_IMM_OFFSET = __instruction_expr___false(s, p, src),  /* SRC2_IMM_OFFSET */
 };

               bitmask_t tmp = encode__cat5_src2(s, &bp, extract_cat5_SRC(src, 1));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(9, 16, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_O);
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* O */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat5_FULL(src);
             const bitmask_t packed = pack_field(0, 0, fld, false);  /* FULL */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(47, 47, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}





static bitmask_t
snippet__instruction_45(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.type;
             const bitmask_t packed = pack_field(49, 51, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_CONST = __instruction_expr___false(s, p, src),  /* SRC1_CONST */
 };

               bitmask_t tmp = encode__cat6_src_const_or_gpr(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(14, 21, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_iim(src->srcs[1]);
             const bitmask_t packed = pack_field(1, 13, fld, true);  /* OFF */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[2]);
             const bitmask_t packed = pack_field(24, 26, fld, false);  /* SIZE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_46(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__instruction_expr_anon_31(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.type;
             const bitmask_t packed = pack_field(49, 51, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_CONST = __instruction_expr___false(s, p, src),  /* SRC1_CONST */
 };

               bitmask_t tmp = encode__cat6_src_const_or_gpr(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(14, 21, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[3]);
             const bitmask_t packed = pack_field(9, 10, fld, false);  /* OFF */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[4]);
             const bitmask_t packed = pack_field(24, 26, fld, false);  /* SIZE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[2]);
             const bitmask_t packed = pack_field(12, 13, fld, false);  /* SRC2_SHIFT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(11, 11, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    if (__instruction_expr_anon_32(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.type;
             const bitmask_t packed = pack_field(49, 51, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_CONST = __instruction_expr___false(s, p, src),  /* SRC1_CONST */
 };

               bitmask_t tmp = encode__cat6_src_const_or_gpr(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(14, 21, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[4]);
             const bitmask_t packed = pack_field(24, 26, fld, false);  /* SIZE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[3]);
             const bitmask_t packed = pack_field(9, 10, fld, false);  /* OFF */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[2]);
             const bitmask_t packed = pack_field(12, 13, fld, false);  /* SRC2_SHIFT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(11, 11, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    if (__instruction_expr_anon_33(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.type;
             const bitmask_t packed = pack_field(49, 51, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_CONST = __instruction_expr___false(s, p, src),  /* SRC1_CONST */
 };

               bitmask_t tmp = encode__cat6_src_const_or_gpr(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(14, 21, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[3]);
             const bitmask_t packed = pack_field(9, 10, fld, false);  /* OFF */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[4]);
             const bitmask_t packed = pack_field(24, 26, fld, false);  /* SIZE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[2]);
             const bitmask_t packed = pack_field(12, 13, fld, false);  /* SRC2_SHIFT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(11, 11, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.type;
             const bitmask_t packed = pack_field(49, 51, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_CONST = __instruction_expr___false(s, p, src),  /* SRC1_CONST */
 };

               bitmask_t tmp = encode__cat6_src_const_or_gpr(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(14, 21, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[2]);
             const bitmask_t packed = pack_field(12, 13, fld, false);  /* SRC2_SHIFT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[3]);
             const bitmask_t packed = pack_field(9, 10, fld, false);  /* OFF */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[4]);
             const bitmask_t packed = pack_field(24, 26, fld, false);  /* SIZE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(11, 11, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_47(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.type;
             const bitmask_t packed = pack_field(49, 51, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_CONST = !!(src->srcs[0]->flags & IR3_REG_CONST),  /* SRC1_CONST */
 };

               bitmask_t tmp = encode__cat6_src_const_or_gpr(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(14, 21, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[3]);
             const bitmask_t packed = pack_field(41, 48, fld, false);  /* OFF */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[4]);
             const bitmask_t packed = pack_field(24, 26, fld, false);  /* SIZE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_CONST);
             const bitmask_t packed = pack_field(13, 13, fld, false);  /* SRC1_CONST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_48(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.type;
             const bitmask_t packed = pack_field(49, 51, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__const_dst(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 40, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_CONST = __instruction_expr___false(s, p, src),  /* SRC1_CONST */
 };

               bitmask_t tmp = encode__cat6_src_const_or_gpr(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(14, 21, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[2]);
             const bitmask_t packed = pack_field(1, 13, fld, true);  /* OFF */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .LOAD_SIZE_IM = !!(src->srcs[3]->flags & IR3_REG_IMMED),  /* LOAD_SIZE_IM */
 };

               bitmask_t tmp = encode__cat6_src_load_size(s, &bp, src->srcs[3]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(24, 31, fld, false);  /* LOAD_SIZE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[3]->flags & IR3_REG_IMMED);
             const bitmask_t packed = pack_field(23, 23, fld, false);  /* LOAD_SIZE_IM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_49(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.type;
             const bitmask_t packed = pack_field(49, 51, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_CONST = __instruction_expr___false(s, p, src),  /* SRC1_CONST */
 };

               bitmask_t tmp = encode__cat6_src_const_or_gpr(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(41, 48, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[2]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC3 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[3]);
             const bitmask_t packed = pack_field(24, 26, fld, false);  /* SIZE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = 1;
             const bitmask_t packed = pack_field(40, 40, fld, false);  /* DST_OFF */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_iim(src->srcs[1]) & 0xff;
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* OFF_LO */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_iim(src->srcs[1]) >> 8;
             const bitmask_t packed = pack_field(9, 13, fld, true);  /* OFF_HI */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_50(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    if (__instruction_expr_anon_35(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.type;
             const bitmask_t packed = pack_field(49, 51, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_CONST = __instruction_expr___false(s, p, src),  /* SRC1_CONST */
 };

               bitmask_t tmp = encode__cat6_src_const_or_gpr(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(41, 48, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[3]);
             const bitmask_t packed = pack_field(9, 10, fld, false);  /* OFF */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[4]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC3 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[5]);
             const bitmask_t packed = pack_field(24, 26, fld, false);  /* SIZE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = 1;
             const bitmask_t packed = pack_field(40, 40, fld, false);  /* DST_OFF */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[2]);
             const bitmask_t packed = pack_field(12, 13, fld, false);  /* SRC2_SHIFT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(11, 11, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    if (__instruction_expr_anon_36(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.type;
             const bitmask_t packed = pack_field(49, 51, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_CONST = __instruction_expr___false(s, p, src),  /* SRC1_CONST */
 };

               bitmask_t tmp = encode__cat6_src_const_or_gpr(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(41, 48, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[4]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC3 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[5]);
             const bitmask_t packed = pack_field(24, 26, fld, false);  /* SIZE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = 1;
             const bitmask_t packed = pack_field(40, 40, fld, false);  /* DST_OFF */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[3]);
             const bitmask_t packed = pack_field(9, 10, fld, false);  /* OFF */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[2]);
             const bitmask_t packed = pack_field(12, 13, fld, false);  /* SRC2_SHIFT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(11, 11, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    if (__instruction_expr_anon_37(s, p, src)) {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.type;
             const bitmask_t packed = pack_field(49, 51, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_CONST = __instruction_expr___false(s, p, src),  /* SRC1_CONST */
 };

               bitmask_t tmp = encode__cat6_src_const_or_gpr(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(41, 48, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[3]);
             const bitmask_t packed = pack_field(9, 10, fld, false);  /* OFF */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[4]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC3 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[5]);
             const bitmask_t packed = pack_field(24, 26, fld, false);  /* SIZE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = 1;
             const bitmask_t packed = pack_field(40, 40, fld, false);  /* DST_OFF */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[2]);
             const bitmask_t packed = pack_field(12, 13, fld, false);  /* SRC2_SHIFT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(11, 11, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    } else


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.type;
             const bitmask_t packed = pack_field(49, 51, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_CONST = __instruction_expr___false(s, p, src),  /* SRC1_CONST */
 };

               bitmask_t tmp = encode__cat6_src_const_or_gpr(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(41, 48, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[2]);
             const bitmask_t packed = pack_field(12, 13, fld, false);  /* SRC2_SHIFT */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[3]);
             const bitmask_t packed = pack_field(9, 10, fld, false);  /* OFF */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[4]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC3 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[5]);
             const bitmask_t packed = pack_field(24, 26, fld, false);  /* SIZE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = 1;
             const bitmask_t packed = pack_field(40, 40, fld, false);  /* DST_OFF */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(11, 11, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_51(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.type;
             const bitmask_t packed = pack_field(49, 51, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_CONST = !!(src->srcs[0]->flags & IR3_REG_CONST),  /* SRC1_CONST */
 };

               bitmask_t tmp = encode__cat6_src_const_or_gpr(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(41, 48, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[3]);
             const bitmask_t packed = pack_field(14, 21, fld, false);  /* OFF */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[4]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC3 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[5]);
             const bitmask_t packed = pack_field(24, 26, fld, false);  /* SIZE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = 1;
             const bitmask_t packed = pack_field(40, 40, fld, false);  /* DST_OFF */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_CONST);
             const bitmask_t packed = pack_field(13, 13, fld, false);  /* SRC1_CONST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_52(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.type;
             const bitmask_t packed = pack_field(49, 51, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(14, 21, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_iim(src->srcs[1]);
             const bitmask_t packed = pack_field(1, 13, fld, true);  /* OFF */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[2]);
             const bitmask_t packed = pack_field(24, 31, fld, false);  /* SIZE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}





static bitmask_t
snippet__instruction_53(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.type;
             const bitmask_t packed = pack_field(49, 51, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[0]);
             const bitmask_t packed = pack_field(1, 13, fld, false);  /* OFF */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[1]);
             const bitmask_t packed = pack_field(24, 31, fld, false);  /* SIZE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_54(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.type;
             const bitmask_t packed = pack_field(49, 51, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(41, 48, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[2]);
             const bitmask_t packed = pack_field(24, 31, fld, false);  /* SIZE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.dst_offset & 0xff;
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* OFF_LO */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.dst_offset >> 8;
             const bitmask_t packed = pack_field(9, 13, fld, true);  /* OFF_HI */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}





static bitmask_t
snippet__instruction_55(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.type;
             const bitmask_t packed = pack_field(49, 51, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__const_dst(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 40, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.iim_val;
             const bitmask_t packed = pack_field(24, 26, fld, false);  /* SIZE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_56(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.type;
             const bitmask_t packed = pack_field(49, 51, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_IM = __instruction_expr___true(s, p, src),  /* SRC_IM */
 };

               bitmask_t tmp = encode__cat6_src(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.iim_val;
             const bitmask_t packed = pack_field(24, 31, fld, false);  /* SIZE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[0]) >> 8;
             const bitmask_t packed = pack_field(9, 13, fld, false);  /* DST_HI */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[0]) & 0xff;
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST_LO */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    {

       const bitmask_t packed = pack_field(40, 40, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_57(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.type;
             const bitmask_t packed = pack_field(49, 51, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_IM = !!(src->srcs[0]->flags & IR3_REG_IMMED),  /* UAV_IM */
 };

               bitmask_t tmp = encode__cat6_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(41, 48, fld, false);  /* UAV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.d - 1;
             const bitmask_t packed = pack_field(9, 10, fld, false);  /* D_MINUS_ONE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_IMMED);
             const bitmask_t packed = pack_field(53, 53, fld, false);  /* UAV_IM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_58(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__cat6_typed(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(11, 11, fld, false);  /* TYPED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.type;
             const bitmask_t packed = pack_field(49, 51, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_IM = !!(src->srcs[0]->flags & IR3_REG_IMMED),  /* UAV_IM */
 };

               bitmask_t tmp = encode__cat6_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(41, 48, fld, false);  /* UAV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_IM = !!(src->srcs[1]->flags & IR3_REG_IMMED),  /* SRC1_IM */
 };

               bitmask_t tmp = encode__cat6_src(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(14, 21, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_IM = !!(src->srcs[2]->flags & IR3_REG_IMMED),  /* SRC2_IM */
 };

               bitmask_t tmp = encode__cat6_src(s, &bp, src->srcs[2]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(24, 31, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.d - 1;
             const bitmask_t packed = pack_field(9, 10, fld, false);  /* D_MINUS_ONE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.iim_val - 1;
             const bitmask_t packed = pack_field(12, 13, fld, false);  /* TYPE_SIZE_MINUS_ONE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_IMMED);
             const bitmask_t packed = pack_field(53, 53, fld, false);  /* UAV_IM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[1]->flags & IR3_REG_IMMED);
             const bitmask_t packed = pack_field(22, 22, fld, false);  /* SRC1_IM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[2]->flags & IR3_REG_IMMED);
             const bitmask_t packed = pack_field(23, 23, fld, false);  /* SRC2_IM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}





static bitmask_t
snippet__instruction_59(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__cat6_typed(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(11, 11, fld, false);  /* TYPED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.type;
             const bitmask_t packed = pack_field(49, 51, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_IM = !!(src->srcs[0]->flags & IR3_REG_IMMED),  /* UAV_IM */
 };

               bitmask_t tmp = encode__cat6_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(41, 48, fld, false);  /* UAV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_IM = !!(src->srcs[2]->flags & IR3_REG_IMMED),  /* SRC2_IM */
 };

               bitmask_t tmp = encode__cat6_src(s, &bp, src->srcs[2]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(24, 31, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_IM = !!(src->srcs[3]->flags & IR3_REG_IMMED),  /* SRC3_IM */
 };

               bitmask_t tmp = encode__cat6_src(s, &bp, src->srcs[3]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* SRC3 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.d - 1;
             const bitmask_t packed = pack_field(9, 10, fld, false);  /* D_MINUS_ONE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.iim_val - 1;
             const bitmask_t packed = pack_field(12, 13, fld, false);  /* TYPE_SIZE_MINUS_ONE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_IMMED);
             const bitmask_t packed = pack_field(53, 53, fld, false);  /* UAV_IM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[2]->flags & IR3_REG_IMMED);
             const bitmask_t packed = pack_field(23, 23, fld, false);  /* SRC2_IM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[3]->flags & IR3_REG_IMMED);
             const bitmask_t packed = pack_field(40, 40, fld, false);  /* SRC3_IM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}






static bitmask_t
snippet__instruction_60(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__cat6_typed(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(11, 11, fld, false);  /* TYPED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.type;
             const bitmask_t packed = pack_field(49, 51, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_IM = !!(extract_cat6_SRC(src, 0)->flags & IR3_REG_IMMED),  /* SRC1_IM */
 };

               bitmask_t tmp = encode__cat6_src(s, &bp, extract_cat6_SRC(src, 0));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(14, 21, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_IM = !!(extract_cat6_SRC(src, 1)->flags & IR3_REG_IMMED),  /* SRC2_IM */
 };

               bitmask_t tmp = encode__cat6_src(s, &bp, extract_cat6_SRC(src, 1));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(24, 31, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.d - 1;
             const bitmask_t packed = pack_field(9, 10, fld, false);  /* D_MINUS_ONE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.iim_val - 1;
             const bitmask_t packed = pack_field(12, 13, fld, false);  /* TYPE_SIZE_MINUS_ONE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(extract_cat6_SRC(src, 0)->flags & IR3_REG_IMMED);
             const bitmask_t packed = pack_field(22, 22, fld, false);  /* SRC1_IM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(extract_cat6_SRC(src, 1)->flags & IR3_REG_IMMED);
             const bitmask_t packed = pack_field(23, 23, fld, false);  /* SRC2_IM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}













static bitmask_t
snippet__instruction_61(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__cat6_typed(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(11, 11, fld, false);  /* TYPED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.type;
             const bitmask_t packed = pack_field(49, 51, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_IM = !!(src->srcs[0]->flags & IR3_REG_IMMED),  /* UAV_IM */
 };

               bitmask_t tmp = encode__cat6_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(41, 48, fld, false);  /* UAV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_IM = !!(extract_cat6_SRC(src, 0)->flags & IR3_REG_IMMED),  /* SRC1_IM */
 };

               bitmask_t tmp = encode__cat6_src(s, &bp, extract_cat6_SRC(src, 0));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(14, 21, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_IM = !!(extract_cat6_SRC(src, 1)->flags & IR3_REG_IMMED),  /* SRC2_IM */
 };

               bitmask_t tmp = encode__cat6_src(s, &bp, extract_cat6_SRC(src, 1));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(24, 31, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, extract_cat6_SRC(src, 2));
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC3 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.d - 1;
             const bitmask_t packed = pack_field(9, 10, fld, false);  /* D_MINUS_ONE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.iim_val - 1;
             const bitmask_t packed = pack_field(12, 13, fld, false);  /* TYPE_SIZE_MINUS_ONE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_IMMED);
             const bitmask_t packed = pack_field(53, 53, fld, false);  /* UAV_IM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(extract_cat6_SRC(src, 0)->flags & IR3_REG_IMMED);
             const bitmask_t packed = pack_field(22, 22, fld, false);  /* SRC1_IM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(extract_cat6_SRC(src, 1)->flags & IR3_REG_IMMED);
             const bitmask_t packed = pack_field(23, 23, fld, false);  /* SRC2_IM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



































static bitmask_t
snippet__instruction_62(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_CONST = !!(src->srcs[0]->flags & IR3_REG_CONST),  /* SRC1_CONST */
 };

               bitmask_t tmp = encode__cat6_src_const_or_gpr(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(14, 21, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[2]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(24, 31, fld, false);  /* SRC3 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[3]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(41, 48, fld, false);  /* SRC4 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[0]->flags & IR3_REG_CONST);
             const bitmask_t packed = pack_field(13, 13, fld, false);  /* SRC1_CONST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_63(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat6_DESC_MODE(src);
             const bitmask_t packed = pack_field(6, 7, fld, false);  /* MODE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .BINDLESS = !!(src->flags & IR3_INSTR_B),  /* BINDLESS */
 };

               bitmask_t tmp = encode__cat6_base(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 3, fld, false);  /* BASE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_IM = !!(src->srcs[1]->flags & IR3_REG_IMMED),  /* SRC1_IM */
    .SH = __instruction_expr_anon_40(s, p, src),  /* SH */
 };

               bitmask_t tmp = encode__cat6_src_sp(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(24, 31, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_IM = __instruction_expr___cat6_direct(s, p, src),  /* SRC2_IM */
 };

               bitmask_t tmp = encode__cat6_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(41, 48, fld, false);  /* UBO */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.iim_val - 1;
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* LOAD_SIZE_MINUS_ONE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_B);
             const bitmask_t packed = pack_field(8, 8, fld, false);  /* BINDLESS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[1]->flags & IR3_REG_IMMED);
             const bitmask_t packed = pack_field(23, 23, fld, false);  /* SRC1_IM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = ISA_GPU_ID() >= 800;
             const bitmask_t packed = pack_field(21, 21, fld, false);  /* SP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_64(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_U);
             const bitmask_t packed = pack_field(11, 11, fld, false);  /* U */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.d;
             const bitmask_t packed = pack_field(9, 10, fld, false);  /* OFFSET */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat6_DESC_MODE(src);
             const bitmask_t packed = pack_field(6, 7, fld, false);  /* MODE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .BINDLESS = !!(src->flags & IR3_INSTR_B),  /* BINDLESS */
 };

               bitmask_t tmp = encode__cat6_base(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 3, fld, false);  /* BASE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_IM = !!(src->srcs[1]->flags & IR3_REG_IMMED),  /* SRC1_IM */
    .SH = __instruction_expr_anon_40(s, p, src),  /* SH */
 };

               bitmask_t tmp = encode__cat6_src_sp(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(24, 31, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_IM = __instruction_expr___cat6_direct(s, p, src),  /* SRC2_IM */
 };

               bitmask_t tmp = encode__cat6_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(41, 48, fld, false);  /* UBO */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.iim_val - 1;
             const bitmask_t packed = pack_field(12, 13, fld, false);  /* TYPE_SIZE_MINUS_ONE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_B);
             const bitmask_t packed = pack_field(8, 8, fld, false);  /* BINDLESS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[1]->flags & IR3_REG_IMMED);
             const bitmask_t packed = pack_field(23, 23, fld, false);  /* SRC1_IM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = ISA_GPU_ID() >= 800;
             const bitmask_t packed = pack_field(21, 21, fld, false);  /* SP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_65(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.type;
             const bitmask_t packed = pack_field(49, 51, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}





static bitmask_t
snippet__instruction_66(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__cat6_typed(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(11, 11, fld, false);  /* TYPED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.type;
             const bitmask_t packed = pack_field(49, 51, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat6_DESC_MODE(src);
             const bitmask_t packed = pack_field(6, 7, fld, false);  /* MODE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .BINDLESS = !!(src->flags & IR3_INSTR_B),  /* BINDLESS */
 };

               bitmask_t tmp = encode__cat6_base(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 3, fld, false);  /* BASE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_IM = __instruction_expr___cat6_direct(s, p, src),  /* UAV_IM */
 };

               bitmask_t tmp = encode__cat6_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(41, 48, fld, false);  /* UAV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.d - 1;
             const bitmask_t packed = pack_field(9, 10, fld, false);  /* D_MINUS_ONE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.iim_val - 1;
             const bitmask_t packed = pack_field(12, 13, fld, false);  /* TYPE_SIZE_MINUS_ONE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_B);
             const bitmask_t packed = pack_field(8, 8, fld, false);  /* BINDLESS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}




static bitmask_t
snippet__instruction_67(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__cat6_typed(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(11, 11, fld, false);  /* TYPED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.type;
             const bitmask_t packed = pack_field(49, 51, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_RCK);
             const bitmask_t packed = pack_field(40, 40, fld, false);  /* RCK */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat6_DESC_MODE(src);
             const bitmask_t packed = pack_field(6, 7, fld, false);  /* MODE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .BINDLESS = !!(src->flags & IR3_INSTR_B),  /* BINDLESS */
 };

               bitmask_t tmp = encode__cat6_base(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 3, fld, false);  /* BASE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[3]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(24, 31, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_IM = __instruction_expr___cat6_direct(s, p, src),  /* UAV_IM */
 };

               bitmask_t tmp = encode__cat6_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(41, 48, fld, false);  /* UAV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_IMM_OFFSET);
             const bitmask_t packed = pack_field(23, 23, fld, false);  /* HAS_OFFSET */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.d - 1;
             const bitmask_t packed = pack_field(9, 10, fld, false);  /* D_MINUS_ONE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.iim_val - 1;
             const bitmask_t packed = pack_field(12, 13, fld, false);  /* TYPE_SIZE_MINUS_ONE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_B);
             const bitmask_t packed = pack_field(8, 8, fld, false);  /* BINDLESS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[2]) >> 5;
             const bitmask_t packed = pack_field(4, 5, fld, false);  /* OFFSET_HI */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[2]) & 0x1f;
             const bitmask_t packed = pack_field(54, 58, fld, false);  /* OFFSET_LO */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    if (__instruction_expr_anon_42(s, p, src)) {

       const bitmask_t packed = pack_field(4, 5, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    if (__instruction_expr_anon_42(s, p, src)) {

       const bitmask_t packed = pack_field(54, 58, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_68(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__cat6_typed(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(11, 11, fld, false);  /* TYPED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.type;
             const bitmask_t packed = pack_field(49, 51, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_RCK);
             const bitmask_t packed = pack_field(40, 40, fld, false);  /* RCK */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat6_DESC_MODE(src);
             const bitmask_t packed = pack_field(6, 7, fld, false);  /* MODE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .BINDLESS = !!(src->flags & IR3_INSTR_B),  /* BINDLESS */
 };

               bitmask_t tmp = encode__cat6_base(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 3, fld, false);  /* BASE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(24, 31, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_IM = __instruction_expr___cat6_direct(s, p, src),  /* UAV_IM */
 };

               bitmask_t tmp = encode__cat6_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(41, 48, fld, false);  /* UAV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_IMM_OFFSET);
             const bitmask_t packed = pack_field(23, 23, fld, false);  /* HAS_OFFSET */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.d - 1;
             const bitmask_t packed = pack_field(9, 10, fld, false);  /* D_MINUS_ONE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.iim_val - 1;
             const bitmask_t packed = pack_field(12, 13, fld, false);  /* TYPE_SIZE_MINUS_ONE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_B);
             const bitmask_t packed = pack_field(8, 8, fld, false);  /* BINDLESS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[2]) >> 5;
             const bitmask_t packed = pack_field(4, 5, fld, false);  /* OFFSET_HI */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_reg_uim(src->srcs[2]) & 0x1f;
             const bitmask_t packed = pack_field(54, 58, fld, false);  /* OFFSET_LO */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }



       
    if (__instruction_expr_anon_42(s, p, src)) {

       const bitmask_t packed = pack_field(4, 5, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }


       
    if (__instruction_expr_anon_42(s, p, src)) {

       const bitmask_t packed = pack_field(54, 58, 0, false);
       BITSET_OR(val.bitset, val.bitset, packed.bitset);
       
    }

      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_69(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__cat6_typed(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(11, 11, fld, false);  /* TYPED */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.type;
             const bitmask_t packed = pack_field(49, 51, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = extract_cat6_DESC_MODE(src);
             const bitmask_t packed = pack_field(6, 7, fld, false);  /* MODE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .BINDLESS = !!(src->flags & IR3_INSTR_B),  /* BINDLESS */
 };

               bitmask_t tmp = encode__cat6_base(s, &bp, src);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 3, fld, false);  /* BASE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[2]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(24, 31, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_IM = __instruction_expr___cat6_direct(s, p, src),  /* UAV_IM */
 };

               bitmask_t tmp = encode__cat6_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(41, 48, fld, false);  /* UAV */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.d - 1;
             const bitmask_t packed = pack_field(9, 10, fld, false);  /* D_MINUS_ONE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.iim_val - 1;
             const bitmask_t packed = pack_field(12, 13, fld, false);  /* TYPE_SIZE_MINUS_ONE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_B);
             const bitmask_t packed = pack_field(8, 8, fld, false);  /* BINDLESS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}











static bitmask_t
snippet__instruction_70(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.shfl_mode;
             const bitmask_t packed = pack_field(45, 47, fld, false);  /* MODE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat6.type;
             const bitmask_t packed = pack_field(49, 51, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(1, 8, fld, false);  /* SRC1 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             { 
 struct bitset_params bp = {
    .SRC_IM = !!(src->srcs[1]->flags & IR3_REG_IMMED),  /* SRC2_IM */
 };

               bitmask_t tmp = encode__cat6_src(s, &bp, src->srcs[1]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(24, 31, fld, false);  /* SRC2 */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->srcs[1]->flags & IR3_REG_IMMED);
             const bitmask_t packed = pack_field(23, 23, fld, false);  /* SRC2_IM */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_71(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat7.g;
             const bitmask_t packed = pack_field(54, 54, fld, false);  /* G */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat7.l;
             const bitmask_t packed = pack_field(53, 53, fld, false);  /* L */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat7.r;
             const bitmask_t packed = pack_field(52, 52, fld, false);  /* R */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat7.w;
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* W */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}




static bitmask_t
snippet__instruction_72(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = 1;
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* DURATION */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_73(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}



static bitmask_t
snippet__instruction_74(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = 1;
             const bitmask_t packed = pack_field(51, 51, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}








static bitmask_t
snippet__instruction_75(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   bitmask_t val = uint64_t_to_bitmask(0);









      uint64_t fld;

      (void)fld;


    
    {


           
    {

             fld = !!(src->flags & IR3_INSTR_SY);
             const bitmask_t packed = pack_field(60, 60, fld, false);  /* SY */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_SS);
             const bitmask_t packed = pack_field(44, 44, fld, false);  /* SS */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !!(src->flags & IR3_INSTR_JP);
             const bitmask_t packed = pack_field(59, 59, fld, false);  /* JP */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = !src->cat7.alias_type_float;
             const bitmask_t packed = pack_field(48, 48, fld, false);  /* TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = (src->srcs[0]->flags & IR3_REG_HALF) ? 0 : 1;
             const bitmask_t packed = pack_field(50, 50, fld, false);  /* TYPE_SIZE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat7.alias_table_size_minus_one;
             const bitmask_t packed = pack_field(40, 43, fld, false);  /* TABLE_SIZE_MINUS_ONE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    if (__instruction_expr_anon_58(s, p, src)) {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__dst_rt(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 36, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    } else


           
    {

             { 
 struct bitset_params bp = {
 };

               bitmask_t tmp = encode__reg_gpr(s, &bp, src->dsts[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(32, 39, fld, false);  /* DST */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    if (__instruction_expr_anon_56(s, p, src)) {

             { 
 struct bitset_params bp = {
    .TYPE_SIZE = (src->srcs[0]->flags & IR3_REG_HALF) ? 0 : 1,  /* TYPE_SIZE */
 };

               bitmask_t tmp = encode__alias_gpr_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 7, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    } else


           
    if (__instruction_expr_anon_57(s, p, src)) {

             { 
 struct bitset_params bp = {
    .TYPE_SIZE = (src->srcs[0]->flags & IR3_REG_HALF) ? 0 : 1,  /* TYPE_SIZE */
 };

               bitmask_t tmp = encode__alias_const_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 10, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    } else


           
    {

             { 
 struct bitset_params bp = {
    .TYPE_SIZE = (src->srcs[0]->flags & IR3_REG_HALF) ? 0 : 1,  /* TYPE_SIZE */
    .TYPE = !src->cat7.alias_type_float,  /* TYPE */
 };

               bitmask_t tmp = encode__alias_immed_src(s, &bp, src->srcs[0]);
               fld = bitmask_to_uint64_t(tmp);
             }
             const bitmask_t packed = pack_field(0, 31, fld, false);  /* SRC */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = (src->srcs[0]->flags & IR3_REG_CONST) ? 1 : ((src->srcs[0]->flags & IR3_REG_IMMED) ? 2 : 0);
             const bitmask_t packed = pack_field(51, 52, fld, false);  /* SRC_REG_TYPE */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat7.alias_scope >> 1;
             const bitmask_t packed = pack_field(49, 49, fld, false);  /* SCOPE_HI */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


           
    {

             fld = src->cat7.alias_scope & 0x1;
             const bitmask_t packed = pack_field(47, 47, fld, false);  /* SCOPE_LO */
             BITSET_OR(val.bitset, val.bitset, packed.bitset);
             
    }


      {}  /* in case no unconditional field to close out last '} else' */
    
    }


   return val;
}


static bitmask_t
encode__instruction(struct encode_state *s, const struct bitset_params *p, const struct ir3_instruction * src)
{
   switch (__instruction_case(s, src)) {
   case OPC_NOP: {


      bitmask_t val = uint64_t_to_bitmask(0x0);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_0(s, p, src).bitset);
      return val;
    }
   case OPC_END: {


      bitmask_t val = uint64_t_to_bitmask(0x300000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_0(s, p, src).bitset);
      return val;
    }
   case OPC_RET: {


      bitmask_t val = uint64_t_to_bitmask(0x200000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_0(s, p, src).bitset);
      return val;
    }
   case OPC_EMIT: {


      bitmask_t val = uint64_t_to_bitmask(0x380000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_0(s, p, src).bitset);
      return val;
    }
   case OPC_CUT: {


      bitmask_t val = uint64_t_to_bitmask(0x400000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_0(s, p, src).bitset);
      return val;
    }
   case OPC_CHMASK: {


      bitmask_t val = uint64_t_to_bitmask(0x480000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_0(s, p, src).bitset);
      return val;
    }
   case OPC_CHSH: {


      bitmask_t val = uint64_t_to_bitmask(0x500000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_0(s, p, src).bitset);
      return val;
    }
   case OPC_FLOW_REV: {


      bitmask_t val = uint64_t_to_bitmask(0x580000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_0(s, p, src).bitset);
      return val;
    }
   case OPC_SHPE: {


      bitmask_t val = uint64_t_to_bitmask(0x402000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_0(s, p, src).bitset);
      return val;
    }
   case OPC_PREDT: {


      bitmask_t val = uint64_t_to_bitmask(0x682000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_0(s, p, src).bitset);
      return val;
    }
   case OPC_PREDF: {


      bitmask_t val = uint64_t_to_bitmask(0x702000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_0(s, p, src).bitset);
      return val;
    }
   case OPC_PREDE: {


      bitmask_t val = uint64_t_to_bitmask(0x782000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_0(s, p, src).bitset);
      return val;
    }
   case OPC_KILL: {


      bitmask_t val = uint64_t_to_bitmask(0x280000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_1(s, p, src).bitset);
      return val;
    }
   case OPC_JUMP: {


      bitmask_t val = uint64_t_to_bitmask(0x100000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_2(s, p, src).bitset);
      return val;
    }
   case OPC_CALL: {


      bitmask_t val = uint64_t_to_bitmask(0x180000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_2(s, p, src).bitset);
      return val;
    }
   case OPC_BKT: {


      bitmask_t val = uint64_t_to_bitmask(0x2000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_2(s, p, src).bitset);
      return val;
    }
   case OPC_GETLAST: {
      if (s->gen >= 600 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0x202000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_3(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_GETONE: {


      bitmask_t val = uint64_t_to_bitmask(0x282000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_2(s, p, src).bitset);
      return val;
    }
   case OPC_SHPS: {


      bitmask_t val = uint64_t_to_bitmask(0x382000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_2(s, p, src).bitset);
      return val;
    }
   case OPC_BRAC: {


      bitmask_t val = uint64_t_to_bitmask(0x80006000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_4(s, p, src).bitset);
      return val;
    }
   case OPC_BRAX: {


      bitmask_t val = uint64_t_to_bitmask(0x8000c000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_5(s, p, src).bitset);
      return val;
    }
   case OPC_BR: {


      bitmask_t val = uint64_t_to_bitmask(0x80000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_6(s, p, src).bitset);
      return val;
    }
   case OPC_BANY: {


      bitmask_t val = uint64_t_to_bitmask(0x80008000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_6(s, p, src).bitset);
      return val;
    }
   case OPC_BALL: {


      bitmask_t val = uint64_t_to_bitmask(0x8000a000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_6(s, p, src).bitset);
      return val;
    }
   case OPC_BRAO: {


      bitmask_t val = uint64_t_to_bitmask(0x80002000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_7(s, p, src).bitset);
      return val;
    }
   case OPC_BRAA: {


      bitmask_t val = uint64_t_to_bitmask(0x80004000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_7(s, p, src).bitset);
      return val;
    }
   case OPC_MOVA_R_IMMED: {


      bitmask_t val = uint64_t_to_bitmask(0x204200f400000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_8(s, p, src).bitset);
      return val;
    }
   case OPC_MOVA_R_GPR: {


      bitmask_t val = uint64_t_to_bitmask(0x200200f400000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_9(s, p, src).bitset);
      return val;
    }
   case OPC_MOV_IMMED: {


      bitmask_t val = uint64_t_to_bitmask(0x2040000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_10(s, p, src).bitset);
      return val;
    }
   case OPC_MOV_CONST: {


      bitmask_t val = uint64_t_to_bitmask(0x2020000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_11(s, p, src).bitset);
      return val;
    }
   case OPC_MOV_GPR: {


      bitmask_t val = uint64_t_to_bitmask(0x2000000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_12(s, p, src).bitset);
      return val;
    }
   case OPC_MOVS_IMMED: {


      bitmask_t val = uint64_t_to_bitmask(0x2000000080000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_13(s, p, src).bitset);
      return val;
    }
   case OPC_MOVS_A0: {


      bitmask_t val = uint64_t_to_bitmask(0x20000000c0000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_14(s, p, src).bitset);
      return val;
    }
   case OPC_MOV_RELGPR: {


      bitmask_t val = uint64_t_to_bitmask(0x2000000000000800);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_15(s, p, src).bitset);
      return val;
    }
   case OPC_MOV_RELCONST: {


      bitmask_t val = uint64_t_to_bitmask(0x2000000000000c00);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_16(s, p, src).bitset);
      return val;
    }
   case OPC_SWZ: {


      bitmask_t val = uint64_t_to_bitmask(0x2400000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_17(s, p, src).bitset);
      return val;
    }
   case OPC_GAT: {


      bitmask_t val = uint64_t_to_bitmask(0x2400010000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_18(s, p, src).bitset);
      return val;
    }
   case OPC_SCT: {


      bitmask_t val = uint64_t_to_bitmask(0x2400020000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_19(s, p, src).bitset);
      return val;
    }
   case OPC_MOVMSK: {


      bitmask_t val = uint64_t_to_bitmask(0x260cc00000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_20(s, p, src).bitset);
      return val;
    }
   case OPC_BARY_F: {


      bitmask_t val = uint64_t_to_bitmask(0x4720000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_21(s, p, src).bitset);
      return val;
    }
   case OPC_FLAT_B: {
      if (s->gen >= 600 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0x4724000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_21(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_ADD_F: {


      bitmask_t val = uint64_t_to_bitmask(0x4000000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_22(s, p, src).bitset);
      return val;
    }
   case OPC_MIN_F: {


      bitmask_t val = uint64_t_to_bitmask(0x4020000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_22(s, p, src).bitset);
      return val;
    }
   case OPC_MAX_F: {


      bitmask_t val = uint64_t_to_bitmask(0x4040000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_22(s, p, src).bitset);
      return val;
    }
   case OPC_MUL_F: {


      bitmask_t val = uint64_t_to_bitmask(0x4060000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_22(s, p, src).bitset);
      return val;
    }
   case OPC_SIGN_F: {


      bitmask_t val = uint64_t_to_bitmask(0x4080000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_23(s, p, src).bitset);
      return val;
    }
   case OPC_CMPS_F: {


      bitmask_t val = uint64_t_to_bitmask(0x40a0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_24(s, p, src).bitset);
      return val;
    }
   case OPC_ABSNEG_F: {


      bitmask_t val = uint64_t_to_bitmask(0x40c0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_23(s, p, src).bitset);
      return val;
    }
   case OPC_CMPV_F: {


      bitmask_t val = uint64_t_to_bitmask(0x40e0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_24(s, p, src).bitset);
      return val;
    }
   case OPC_FLOOR_F: {


      bitmask_t val = uint64_t_to_bitmask(0x4120000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_23(s, p, src).bitset);
      return val;
    }
   case OPC_CEIL_F: {


      bitmask_t val = uint64_t_to_bitmask(0x4140000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_23(s, p, src).bitset);
      return val;
    }
   case OPC_RNDNE_F: {


      bitmask_t val = uint64_t_to_bitmask(0x4160000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_23(s, p, src).bitset);
      return val;
    }
   case OPC_RNDAZ_F: {


      bitmask_t val = uint64_t_to_bitmask(0x4180000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_23(s, p, src).bitset);
      return val;
    }
   case OPC_TRUNC_F: {


      bitmask_t val = uint64_t_to_bitmask(0x41a0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_23(s, p, src).bitset);
      return val;
    }
   case OPC_ADD_U: {


      bitmask_t val = uint64_t_to_bitmask(0x4200000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_22(s, p, src).bitset);
      return val;
    }
   case OPC_ADD_S: {


      bitmask_t val = uint64_t_to_bitmask(0x4220000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_22(s, p, src).bitset);
      return val;
    }
   case OPC_SUB_U: {


      bitmask_t val = uint64_t_to_bitmask(0x4240000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_22(s, p, src).bitset);
      return val;
    }
   case OPC_SUB_S: {


      bitmask_t val = uint64_t_to_bitmask(0x4260000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_22(s, p, src).bitset);
      return val;
    }
   case OPC_CMPS_U: {


      bitmask_t val = uint64_t_to_bitmask(0x4280000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_24(s, p, src).bitset);
      return val;
    }
   case OPC_CMPS_S: {


      bitmask_t val = uint64_t_to_bitmask(0x42a0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_24(s, p, src).bitset);
      return val;
    }
   case OPC_MIN_U: {


      bitmask_t val = uint64_t_to_bitmask(0x42c0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_22(s, p, src).bitset);
      return val;
    }
   case OPC_MIN_S: {


      bitmask_t val = uint64_t_to_bitmask(0x42e0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_22(s, p, src).bitset);
      return val;
    }
   case OPC_MAX_U: {


      bitmask_t val = uint64_t_to_bitmask(0x4300000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_22(s, p, src).bitset);
      return val;
    }
   case OPC_MAX_S: {


      bitmask_t val = uint64_t_to_bitmask(0x4320000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_22(s, p, src).bitset);
      return val;
    }
   case OPC_ABSNEG_S: {


      bitmask_t val = uint64_t_to_bitmask(0x4340000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_23(s, p, src).bitset);
      return val;
    }
   case OPC_AND_B: {


      bitmask_t val = uint64_t_to_bitmask(0x4380000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_22(s, p, src).bitset);
      return val;
    }
   case OPC_OR_B: {


      bitmask_t val = uint64_t_to_bitmask(0x43a0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_22(s, p, src).bitset);
      return val;
    }
   case OPC_NOT_B: {


      bitmask_t val = uint64_t_to_bitmask(0x43c0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_23(s, p, src).bitset);
      return val;
    }
   case OPC_XOR_B: {


      bitmask_t val = uint64_t_to_bitmask(0x43e0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_22(s, p, src).bitset);
      return val;
    }
   case OPC_CMPV_U: {


      bitmask_t val = uint64_t_to_bitmask(0x4420000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_24(s, p, src).bitset);
      return val;
    }
   case OPC_CMPV_S: {


      bitmask_t val = uint64_t_to_bitmask(0x4440000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_24(s, p, src).bitset);
      return val;
    }
   case OPC_MUL_F_MUL2: {
      if (s->gen >= 700 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0x4460000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_24(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_ADD_F_MUL2: {
      if (s->gen >= 700 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0x4480000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_24(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_MUL_F_DIV2: {
      if (s->gen >= 700 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0x44a0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_24(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_ADD_F_DIV2: {
      if (s->gen >= 700 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0x44c0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_24(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_MUL_U24: {


      bitmask_t val = uint64_t_to_bitmask(0x4600000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_22(s, p, src).bitset);
      return val;
    }
   case OPC_MUL_S24: {


      bitmask_t val = uint64_t_to_bitmask(0x4620000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_22(s, p, src).bitset);
      return val;
    }
   case OPC_MULL_U: {


      bitmask_t val = uint64_t_to_bitmask(0x4640000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_22(s, p, src).bitset);
      return val;
    }
   case OPC_BFREV_B: {


      bitmask_t val = uint64_t_to_bitmask(0x4660000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_23(s, p, src).bitset);
      return val;
    }
   case OPC_CLZ_S: {


      bitmask_t val = uint64_t_to_bitmask(0x4680000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_23(s, p, src).bitset);
      return val;
    }
   case OPC_CLZ_B: {


      bitmask_t val = uint64_t_to_bitmask(0x46a0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_23(s, p, src).bitset);
      return val;
    }
   case OPC_SHL_B: {


      bitmask_t val = uint64_t_to_bitmask(0x46c0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_22(s, p, src).bitset);
      return val;
    }
   case OPC_SHR_B: {


      bitmask_t val = uint64_t_to_bitmask(0x46e0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_22(s, p, src).bitset);
      return val;
    }
   case OPC_ASHR_B: {


      bitmask_t val = uint64_t_to_bitmask(0x4700000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_22(s, p, src).bitset);
      return val;
    }
   case OPC_MGEN_B: {


      bitmask_t val = uint64_t_to_bitmask(0x4740000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_22(s, p, src).bitset);
      return val;
    }
   case OPC_GETBIT_B: {


      bitmask_t val = uint64_t_to_bitmask(0x4760000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_22(s, p, src).bitset);
      return val;
    }
   case OPC_SETRM: {


      bitmask_t val = uint64_t_to_bitmask(0x4780000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_23(s, p, src).bitset);
      return val;
    }
   case OPC_CBITS_B: {


      bitmask_t val = uint64_t_to_bitmask(0x47a0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_23(s, p, src).bitset);
      return val;
    }
   case OPC_SHB: {


      bitmask_t val = uint64_t_to_bitmask(0x47c0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_22(s, p, src).bitset);
      return val;
    }
   case OPC_MSAD: {


      bitmask_t val = uint64_t_to_bitmask(0x47e0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_22(s, p, src).bitset);
      return val;
    }
   case OPC_MAD_U16: {


      bitmask_t val = uint64_t_to_bitmask(0x6000000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_25(s, p, src).bitset);
      return val;
    }
   case OPC_MADSH_U16: {


      bitmask_t val = uint64_t_to_bitmask(0x6080000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_25(s, p, src).bitset);
      return val;
    }
   case OPC_MAD_S16: {


      bitmask_t val = uint64_t_to_bitmask(0x6100000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_25(s, p, src).bitset);
      return val;
    }
   case OPC_MADSH_M16: {


      bitmask_t val = uint64_t_to_bitmask(0x6180000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_25(s, p, src).bitset);
      return val;
    }
   case OPC_MAD_U24: {


      bitmask_t val = uint64_t_to_bitmask(0x6200000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_25(s, p, src).bitset);
      return val;
    }
   case OPC_MAD_S24: {


      bitmask_t val = uint64_t_to_bitmask(0x6280000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_25(s, p, src).bitset);
      return val;
    }
   case OPC_MAD_F16: {


      bitmask_t val = uint64_t_to_bitmask(0x6300000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_25(s, p, src).bitset);
      return val;
    }
   case OPC_MAD_F32: {


      bitmask_t val = uint64_t_to_bitmask(0x6380000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_25(s, p, src).bitset);
      return val;
    }
   case OPC_SEL_B16: {


      bitmask_t val = uint64_t_to_bitmask(0x6400000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_25(s, p, src).bitset);
      return val;
    }
   case OPC_SEL_B32: {


      bitmask_t val = uint64_t_to_bitmask(0x6480000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_25(s, p, src).bitset);
      return val;
    }
   case OPC_SEL_S16: {


      bitmask_t val = uint64_t_to_bitmask(0x6500000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_25(s, p, src).bitset);
      return val;
    }
   case OPC_SEL_S32: {


      bitmask_t val = uint64_t_to_bitmask(0x6580000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_25(s, p, src).bitset);
      return val;
    }
   case OPC_SEL_F16: {


      bitmask_t val = uint64_t_to_bitmask(0x6600000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_25(s, p, src).bitset);
      return val;
    }
   case OPC_SEL_F32: {


      bitmask_t val = uint64_t_to_bitmask(0x6680000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_25(s, p, src).bitset);
      return val;
    }
   case OPC_SAD_S16: {


      bitmask_t val = uint64_t_to_bitmask(0x6700000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_25(s, p, src).bitset);
      return val;
    }
   case OPC_SAD_S32: {


      bitmask_t val = uint64_t_to_bitmask(0x6780000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_25(s, p, src).bitset);
      return val;
    }
   case OPC_MAD_F16_MUL2: {
      if (s->gen >= 700 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0x6200000000002000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_25(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_MAD_F32_MUL2: {
      if (s->gen >= 700 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0x6280000000002000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_25(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_MAD_F16_DIV2: {
      if (s->gen >= 700 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0x6300000000002000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_25(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_MAD_F32_DIV2: {
      if (s->gen >= 700 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0x6380000000002000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_25(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_SHRM: {


      bitmask_t val = uint64_t_to_bitmask(0x6400000000002000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_26(s, p, src).bitset);
      return val;
    }
   case OPC_SHLM: {


      bitmask_t val = uint64_t_to_bitmask(0x6480000000002000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_26(s, p, src).bitset);
      return val;
    }
   case OPC_SHRG: {


      bitmask_t val = uint64_t_to_bitmask(0x6500000000002000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_26(s, p, src).bitset);
      return val;
    }
   case OPC_SHLG: {


      bitmask_t val = uint64_t_to_bitmask(0x6580000000002000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_26(s, p, src).bitset);
      return val;
    }
   case OPC_ANDG: {


      bitmask_t val = uint64_t_to_bitmask(0x6600000000002000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_26(s, p, src).bitset);
      return val;
    }
   case OPC_DP2ACC: {
      if (s->gen >= 0 && s->gen <= 799) {


      bitmask_t val = uint64_t_to_bitmask(0x6680000000002000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_27(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_DP4ACC: {


      bitmask_t val = uint64_t_to_bitmask(0x6680400000002000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_27(s, p, src).bitset);
      return val;
    }
   case OPC_WMM: {


      bitmask_t val = uint64_t_to_bitmask(0x6700000000002000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_28(s, p, src).bitset);
      return val;
    }
   case OPC_WMM_ACCU: {


      bitmask_t val = uint64_t_to_bitmask(0x6700040000002000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_28(s, p, src).bitset);
      return val;
    }
   case OPC_RCP: {


      bitmask_t val = uint64_t_to_bitmask(0x8000000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_29(s, p, src).bitset);
      return val;
    }
   case OPC_RSQ: {


      bitmask_t val = uint64_t_to_bitmask(0x8020000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_29(s, p, src).bitset);
      return val;
    }
   case OPC_LOG2: {


      bitmask_t val = uint64_t_to_bitmask(0x8040000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_29(s, p, src).bitset);
      return val;
    }
   case OPC_EXP2: {


      bitmask_t val = uint64_t_to_bitmask(0x8060000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_29(s, p, src).bitset);
      return val;
    }
   case OPC_SIN: {


      bitmask_t val = uint64_t_to_bitmask(0x8080000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_29(s, p, src).bitset);
      return val;
    }
   case OPC_COS: {


      bitmask_t val = uint64_t_to_bitmask(0x80a0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_29(s, p, src).bitset);
      return val;
    }
   case OPC_SQRT: {


      bitmask_t val = uint64_t_to_bitmask(0x80c0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_29(s, p, src).bitset);
      return val;
    }
   case OPC_HRSQ: {


      bitmask_t val = uint64_t_to_bitmask(0x8120000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_29(s, p, src).bitset);
      return val;
    }
   case OPC_HLOG2: {


      bitmask_t val = uint64_t_to_bitmask(0x8140000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_29(s, p, src).bitset);
      return val;
    }
   case OPC_HEXP2: {


      bitmask_t val = uint64_t_to_bitmask(0x8160000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_29(s, p, src).bitset);
      return val;
    }
   case OPC_ISAM: {


      bitmask_t val = uint64_t_to_bitmask(0xa000000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_30(s, p, src).bitset);
      return val;
    }
   case OPC_IMG_BINDLESS_HOF: {


      bitmask_t val = uint64_t_to_bitmask(0xa740000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_31(s, p, src).bitset);
      return val;
    }
   case OPC_IMG_BINDLESS_PCMN: {


      bitmask_t val = uint64_t_to_bitmask(0xa744000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_32(s, p, src).bitset);
      return val;
    }
   case OPC_IMG_BINDLESS: {


      bitmask_t val = uint64_t_to_bitmask(0xa750000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_33(s, p, src).bitset);
      return val;
    }
   case OPC_ISAML: {


      bitmask_t val = uint64_t_to_bitmask(0xa040000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_34(s, p, src).bitset);
      return val;
    }
   case OPC_ISAMM: {


      bitmask_t val = uint64_t_to_bitmask(0xa080000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_35(s, p, src).bitset);
      return val;
    }
   case OPC_SAM: {


      bitmask_t val = uint64_t_to_bitmask(0xa0c0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_35(s, p, src).bitset);
      return val;
    }
   case OPC_SAMB: {


      bitmask_t val = uint64_t_to_bitmask(0xa100000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_34(s, p, src).bitset);
      return val;
    }
   case OPC_SAML: {


      bitmask_t val = uint64_t_to_bitmask(0xa140000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_34(s, p, src).bitset);
      return val;
    }
   case OPC_SAMGQ: {


      bitmask_t val = uint64_t_to_bitmask(0xa180000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_35(s, p, src).bitset);
      return val;
    }
   case OPC_GETLOD: {


      bitmask_t val = uint64_t_to_bitmask(0xa1c0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_35(s, p, src).bitset);
      return val;
    }
   case OPC_CONV: {


      bitmask_t val = uint64_t_to_bitmask(0xa200000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_34(s, p, src).bitset);
      return val;
    }
   case OPC_CONVM: {


      bitmask_t val = uint64_t_to_bitmask(0xa240000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_34(s, p, src).bitset);
      return val;
    }
   case OPC_GETSIZE: {


      bitmask_t val = uint64_t_to_bitmask(0xa280000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_36(s, p, src).bitset);
      return val;
    }
   case OPC_GETBUF: {


      bitmask_t val = uint64_t_to_bitmask(0xa2c0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_37(s, p, src).bitset);
      return val;
    }
   case OPC_GETPOS: {


      bitmask_t val = uint64_t_to_bitmask(0xa300000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_36(s, p, src).bitset);
      return val;
    }
   case OPC_GETINFO: {


      bitmask_t val = uint64_t_to_bitmask(0xa340000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_37(s, p, src).bitset);
      return val;
    }
   case OPC_DSX: {


      bitmask_t val = uint64_t_to_bitmask(0xa380000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_38(s, p, src).bitset);
      return val;
    }
   case OPC_DSY: {


      bitmask_t val = uint64_t_to_bitmask(0xa3c0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_38(s, p, src).bitset);
      return val;
    }
   case OPC_GATHER4R: {


      bitmask_t val = uint64_t_to_bitmask(0xa400000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_35(s, p, src).bitset);
      return val;
    }
   case OPC_GATHER4G: {


      bitmask_t val = uint64_t_to_bitmask(0xa440000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_35(s, p, src).bitset);
      return val;
    }
   case OPC_GATHER4B: {


      bitmask_t val = uint64_t_to_bitmask(0xa480000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_35(s, p, src).bitset);
      return val;
    }
   case OPC_GATHER4A: {


      bitmask_t val = uint64_t_to_bitmask(0xa4c0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_35(s, p, src).bitset);
      return val;
    }
   case OPC_SAMGP0: {


      bitmask_t val = uint64_t_to_bitmask(0xa500000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_35(s, p, src).bitset);
      return val;
    }
   case OPC_SAMGP1: {


      bitmask_t val = uint64_t_to_bitmask(0xa540000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_35(s, p, src).bitset);
      return val;
    }
   case OPC_SAMGP2: {


      bitmask_t val = uint64_t_to_bitmask(0xa580000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_35(s, p, src).bitset);
      return val;
    }
   case OPC_SAMGP3: {


      bitmask_t val = uint64_t_to_bitmask(0xa5c0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_35(s, p, src).bitset);
      return val;
    }
   case OPC_DSXPP_1: {


      bitmask_t val = uint64_t_to_bitmask(0xa600000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_39(s, p, src).bitset);
      return val;
    }
   case OPC_DSYPP_1: {


      bitmask_t val = uint64_t_to_bitmask(0xa640000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_39(s, p, src).bitset);
      return val;
    }
   case OPC_RGETPOS: {


      bitmask_t val = uint64_t_to_bitmask(0xa680000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_38(s, p, src).bitset);
      return val;
    }
   case OPC_RGETINFO: {


      bitmask_t val = uint64_t_to_bitmask(0xa6c0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_40(s, p, src).bitset);
      return val;
    }
   case OPC_TCINV: {


      bitmask_t val = uint64_t_to_bitmask(0xa700000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_41(s, p, src).bitset);
      return val;
    }
   case OPC_BRCST_ACTIVE: {
      if (s->gen >= 600 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0xa7c0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_42(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_QUAD_SHUFFLE_BRCST: {


      bitmask_t val = uint64_t_to_bitmask(0xa7e0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_43(s, p, src).bitset);
      return val;
    }
   case OPC_QUAD_SHUFFLE_HORIZ: {


      bitmask_t val = uint64_t_to_bitmask(0xa7e0000000080000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_44(s, p, src).bitset);
      return val;
    }
   case OPC_QUAD_SHUFFLE_VERT: {


      bitmask_t val = uint64_t_to_bitmask(0xa7e0000000100000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_44(s, p, src).bitset);
      return val;
    }
   case OPC_QUAD_SHUFFLE_DIAG: {


      bitmask_t val = uint64_t_to_bitmask(0xa7e0000000180000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_44(s, p, src).bitset);
      return val;
    }
   case OPC_LDG: {


      bitmask_t val = uint64_t_to_bitmask(0xc000000000800001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_45(s, p, src).bitset);
      return val;
    }
   case OPC_LDG_A: {
      if (s->gen >= 600 && s->gen <= 699) {


      bitmask_t val = uint64_t_to_bitmask(0xc000000000c00001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_46(s, p, src).bitset);
      return val;
      }
      if (s->gen >= 700 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0xc000000000c00001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_47(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_LDG_K: {
      if (s->gen >= 600 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0xc010000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_48(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_STG: {


      bitmask_t val = uint64_t_to_bitmask(0xc0c0000000800000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_49(s, p, src).bitset);
      return val;
    }
   case OPC_STG_A: {
      if (s->gen >= 600 && s->gen <= 699) {


      bitmask_t val = uint64_t_to_bitmask(0xc0d0000000800000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_50(s, p, src).bitset);
      return val;
      }
      if (s->gen >= 700 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0xc0d0000000800000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_51(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_LDL: {


      bitmask_t val = uint64_t_to_bitmask(0xc040000000800001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_52(s, p, src).bitset);
      return val;
    }
   case OPC_LDP: {


      bitmask_t val = uint64_t_to_bitmask(0xc080000000800001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_52(s, p, src).bitset);
      return val;
    }
   case OPC_LDLW: {


      bitmask_t val = uint64_t_to_bitmask(0xc280000000800001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_52(s, p, src).bitset);
      return val;
    }
   case OPC_LDLV: {


      bitmask_t val = uint64_t_to_bitmask(0xc7c0000000c00000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_53(s, p, src).bitset);
      return val;
    }
   case OPC_STL: {


      bitmask_t val = uint64_t_to_bitmask(0xc100010000800000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_54(s, p, src).bitset);
      return val;
    }
   case OPC_STP: {


      bitmask_t val = uint64_t_to_bitmask(0xc140010000800000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_54(s, p, src).bitset);
      return val;
    }
   case OPC_STLW: {


      bitmask_t val = uint64_t_to_bitmask(0xc2c0010000800000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_54(s, p, src).bitset);
      return val;
    }
   case OPC_STC: {
      if (s->gen >= 600 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0xc700000000800000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_55(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_STSC: {
      if (s->gen >= 700 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0xc740000000c00000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_56(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_RESINFO: {


      bitmask_t val = uint64_t_to_bitmask(0xc3c0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_57(s, p, src).bitset);
      return val;
    }
   case OPC_LDGB: {
      if (s->gen >= 0 && s->gen <= 499) {


      bitmask_t val = uint64_t_to_bitmask(0xc6c0000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_58(s, p, src).bitset);
      return val;
      }
      if (s->gen >= 500 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0xc6c0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_58(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_LDIB: {


      bitmask_t val = uint64_t_to_bitmask(0xc180000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_58(s, p, src).bitset);
      return val;
    }
   case OPC_STGB: {
      if (s->gen >= 500 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0xc700000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_59(s, p, src).bitset);
      return val;
      }
      if (s->gen >= 0 && s->gen <= 499) {


      bitmask_t val = uint64_t_to_bitmask(0xc700000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_59(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_STIB: {
      if (s->gen >= 500 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0xc740000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_59(s, p, src).bitset);
      return val;
      }
      if (s->gen >= 0 && s->gen <= 499) {


      bitmask_t val = uint64_t_to_bitmask(0xc740000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_59(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_ATOMIC_ADD: {


      bitmask_t val = uint64_t_to_bitmask(0xc400000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_60(s, p, src).bitset);
      return val;
    }
   case OPC_ATOMIC_SUB: {


      bitmask_t val = uint64_t_to_bitmask(0xc440000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_60(s, p, src).bitset);
      return val;
    }
   case OPC_ATOMIC_XCHG: {


      bitmask_t val = uint64_t_to_bitmask(0xc480000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_60(s, p, src).bitset);
      return val;
    }
   case OPC_ATOMIC_INC: {


      bitmask_t val = uint64_t_to_bitmask(0xc4c0000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_60(s, p, src).bitset);
      return val;
    }
   case OPC_ATOMIC_DEC: {


      bitmask_t val = uint64_t_to_bitmask(0xc500000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_60(s, p, src).bitset);
      return val;
    }
   case OPC_ATOMIC_CMPXCHG: {


      bitmask_t val = uint64_t_to_bitmask(0xc540000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_60(s, p, src).bitset);
      return val;
    }
   case OPC_ATOMIC_MIN: {


      bitmask_t val = uint64_t_to_bitmask(0xc580000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_60(s, p, src).bitset);
      return val;
    }
   case OPC_ATOMIC_MAX: {


      bitmask_t val = uint64_t_to_bitmask(0xc5c0000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_60(s, p, src).bitset);
      return val;
    }
   case OPC_ATOMIC_AND: {


      bitmask_t val = uint64_t_to_bitmask(0xc600000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_60(s, p, src).bitset);
      return val;
    }
   case OPC_ATOMIC_OR: {


      bitmask_t val = uint64_t_to_bitmask(0xc640000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_60(s, p, src).bitset);
      return val;
    }
   case OPC_ATOMIC_XOR: {


      bitmask_t val = uint64_t_to_bitmask(0xc680000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_60(s, p, src).bitset);
      return val;
    }
   case OPC_ATOMIC_S_ADD: {
      if (s->gen >= 0 && s->gen <= 499) {


      bitmask_t val = uint64_t_to_bitmask(0xc410000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_61(s, p, src).bitset);
      return val;
      }
      if (s->gen >= 500 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0xc410000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_61(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_ATOMIC_S_SUB: {
      if (s->gen >= 0 && s->gen <= 499) {


      bitmask_t val = uint64_t_to_bitmask(0xc450000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_61(s, p, src).bitset);
      return val;
      }
      if (s->gen >= 500 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0xc450000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_61(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_ATOMIC_S_XCHG: {
      if (s->gen >= 0 && s->gen <= 499) {


      bitmask_t val = uint64_t_to_bitmask(0xc490000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_61(s, p, src).bitset);
      return val;
      }
      if (s->gen >= 500 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0xc490000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_61(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_ATOMIC_S_INC: {
      if (s->gen >= 0 && s->gen <= 499) {


      bitmask_t val = uint64_t_to_bitmask(0xc4d0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_61(s, p, src).bitset);
      return val;
      }
      if (s->gen >= 500 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0xc4d0000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_61(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_ATOMIC_S_DEC: {
      if (s->gen >= 0 && s->gen <= 499) {


      bitmask_t val = uint64_t_to_bitmask(0xc510000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_61(s, p, src).bitset);
      return val;
      }
      if (s->gen >= 500 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0xc510000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_61(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_ATOMIC_S_CMPXCHG: {
      if (s->gen >= 0 && s->gen <= 499) {


      bitmask_t val = uint64_t_to_bitmask(0xc550000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_61(s, p, src).bitset);
      return val;
      }
      if (s->gen >= 500 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0xc550000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_61(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_ATOMIC_S_MIN: {
      if (s->gen >= 0 && s->gen <= 499) {


      bitmask_t val = uint64_t_to_bitmask(0xc590000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_61(s, p, src).bitset);
      return val;
      }
      if (s->gen >= 500 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0xc590000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_61(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_ATOMIC_S_MAX: {
      if (s->gen >= 0 && s->gen <= 499) {


      bitmask_t val = uint64_t_to_bitmask(0xc5d0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_61(s, p, src).bitset);
      return val;
      }
      if (s->gen >= 500 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0xc5d0000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_61(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_ATOMIC_S_AND: {
      if (s->gen >= 0 && s->gen <= 499) {


      bitmask_t val = uint64_t_to_bitmask(0xc610000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_61(s, p, src).bitset);
      return val;
      }
      if (s->gen >= 500 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0xc610000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_61(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_ATOMIC_S_OR: {
      if (s->gen >= 0 && s->gen <= 499) {


      bitmask_t val = uint64_t_to_bitmask(0xc650000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_61(s, p, src).bitset);
      return val;
      }
      if (s->gen >= 500 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0xc650000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_61(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_ATOMIC_S_XOR: {
      if (s->gen >= 0 && s->gen <= 499) {


      bitmask_t val = uint64_t_to_bitmask(0xc690000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_61(s, p, src).bitset);
      return val;
      }
      if (s->gen >= 500 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0xc690000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_61(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_ATOMIC_G_ADD: {


      bitmask_t val = uint64_t_to_bitmask(0xc410000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_60(s, p, src).bitset);
      return val;
    }
   case OPC_ATOMIC_G_SUB: {


      bitmask_t val = uint64_t_to_bitmask(0xc450000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_60(s, p, src).bitset);
      return val;
    }
   case OPC_ATOMIC_G_XCHG: {


      bitmask_t val = uint64_t_to_bitmask(0xc490000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_60(s, p, src).bitset);
      return val;
    }
   case OPC_ATOMIC_G_INC: {


      bitmask_t val = uint64_t_to_bitmask(0xc4d0000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_60(s, p, src).bitset);
      return val;
    }
   case OPC_ATOMIC_G_DEC: {


      bitmask_t val = uint64_t_to_bitmask(0xc510000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_60(s, p, src).bitset);
      return val;
    }
   case OPC_ATOMIC_G_CMPXCHG: {


      bitmask_t val = uint64_t_to_bitmask(0xc550000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_60(s, p, src).bitset);
      return val;
    }
   case OPC_ATOMIC_G_MIN: {


      bitmask_t val = uint64_t_to_bitmask(0xc590000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_60(s, p, src).bitset);
      return val;
    }
   case OPC_ATOMIC_G_MAX: {


      bitmask_t val = uint64_t_to_bitmask(0xc5d0000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_60(s, p, src).bitset);
      return val;
    }
   case OPC_ATOMIC_G_AND: {


      bitmask_t val = uint64_t_to_bitmask(0xc610000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_60(s, p, src).bitset);
      return val;
    }
   case OPC_ATOMIC_G_OR: {


      bitmask_t val = uint64_t_to_bitmask(0xc650000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_60(s, p, src).bitset);
      return val;
    }
   case OPC_ATOMIC_G_XOR: {


      bitmask_t val = uint64_t_to_bitmask(0xc690000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_60(s, p, src).bitset);
      return val;
    }
   case OPC_RAY_INTERSECTION: {
      if (s->gen >= 700 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0xc380000000000001);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_62(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_LDC_K: {


      bitmask_t val = uint64_t_to_bitmask(0xc036000000478000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_63(s, p, src).bitset);
      return val;
    }
   case OPC_LDC: {


      bitmask_t val = uint64_t_to_bitmask(0xc026000000478000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_64(s, p, src).bitset);
      return val;
    }
   case OPC_GETSPID: {


      bitmask_t val = uint64_t_to_bitmask(0xc020000000490000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_65(s, p, src).bitset);
      return val;
    }
   case OPC_GETWID: {


      bitmask_t val = uint64_t_to_bitmask(0xc020000000494000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_65(s, p, src).bitset);
      return val;
    }
   case OPC_GETFIBERID: {
      if (s->gen >= 600 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0xc020000000c98000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_65(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_RESINFO_B: {


      bitmask_t val = uint64_t_to_bitmask(0xc02000000063c000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_66(s, p, src).bitset);
      return val;
    }
   case OPC_RESBASE: {


      bitmask_t val = uint64_t_to_bitmask(0xc020000000630000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_66(s, p, src).bitset);
      return val;
    }
   case OPC_STIB_B: {


      bitmask_t val = uint64_t_to_bitmask(0xc020000000674000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_67(s, p, src).bitset);
      return val;
    }
   case OPC_LDIB_B: {


      bitmask_t val = uint64_t_to_bitmask(0xc020000000618000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_68(s, p, src).bitset);
      return val;
    }
   case OPC_ATOMIC_B_ADD: {


      bitmask_t val = uint64_t_to_bitmask(0xc030000000640000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_69(s, p, src).bitset);
      return val;
    }
   case OPC_ATOMIC_B_SUB: {


      bitmask_t val = uint64_t_to_bitmask(0xc030000000644000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_69(s, p, src).bitset);
      return val;
    }
   case OPC_ATOMIC_B_XCHG: {


      bitmask_t val = uint64_t_to_bitmask(0xc030000000648000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_69(s, p, src).bitset);
      return val;
    }
   case OPC_ATOMIC_B_CMPXCHG: {


      bitmask_t val = uint64_t_to_bitmask(0xc030000000654000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_69(s, p, src).bitset);
      return val;
    }
   case OPC_ATOMIC_B_MIN: {


      bitmask_t val = uint64_t_to_bitmask(0xc030000000658000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_69(s, p, src).bitset);
      return val;
    }
   case OPC_ATOMIC_B_MAX: {


      bitmask_t val = uint64_t_to_bitmask(0xc03000000065c000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_69(s, p, src).bitset);
      return val;
    }
   case OPC_ATOMIC_B_AND: {


      bitmask_t val = uint64_t_to_bitmask(0xc030000000660000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_69(s, p, src).bitset);
      return val;
    }
   case OPC_ATOMIC_B_OR: {


      bitmask_t val = uint64_t_to_bitmask(0xc030000000664000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_69(s, p, src).bitset);
      return val;
    }
   case OPC_ATOMIC_B_XOR: {


      bitmask_t val = uint64_t_to_bitmask(0xc030000000668000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_69(s, p, src).bitset);
      return val;
    }
   case OPC_SHFL: {
      if (s->gen >= 600 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0xc6e0000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_70(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_BAR: {


      bitmask_t val = uint64_t_to_bitmask(0xe002000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_71(s, p, src).bitset);
      return val;
    }
   case OPC_FENCE: {


      bitmask_t val = uint64_t_to_bitmask(0xe082000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_71(s, p, src).bitset);
      return val;
    }
   case OPC_SLEEP: {


      bitmask_t val = uint64_t_to_bitmask(0xe100000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_72(s, p, src).bitset);
      return val;
    }
   case OPC_ICINV: {


      bitmask_t val = uint64_t_to_bitmask(0xe180000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_73(s, p, src).bitset);
      return val;
    }
   case OPC_DCCLN: {


      bitmask_t val = uint64_t_to_bitmask(0xe200000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_74(s, p, src).bitset);
      return val;
    }
   case OPC_DCINV: {


      bitmask_t val = uint64_t_to_bitmask(0xe280000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_74(s, p, src).bitset);
      return val;
    }
   case OPC_DCFLU: {


      bitmask_t val = uint64_t_to_bitmask(0xe300000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_74(s, p, src).bitset);
      return val;
    }
   case OPC_CCINV: {
      if (s->gen >= 700 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0xe2d2000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_73(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_LOCK: {
      if (s->gen >= 700 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0xe3c2000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_73(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_UNLOCK: {
      if (s->gen >= 700 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0xe3ca000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_73(s, p, src).bitset);
      return val;
      }
      break;
    }
   case OPC_ALIAS: {
      if (s->gen >= 700 && s->gen <= 4294967295) {


      bitmask_t val = uint64_t_to_bitmask(0xe400000000000000);




      BITSET_OR(val.bitset, val.bitset, snippet__instruction_75(s, p, src).bitset);
      return val;
      }
      break;
    }
   default:
      /* Note that we need the default case, because there are
       * instructions which we never expect to be encoded, (ie.
       * meta/macro instructions) as they are removed/replace
       * in earlier stages of the compiler.
       */
      break;
   }
   mesa_loge("Unhandled #instruction encode case: 0x%x\n", __instruction_case(s, src));
   return uint64_t_to_bitmask(0);
}
