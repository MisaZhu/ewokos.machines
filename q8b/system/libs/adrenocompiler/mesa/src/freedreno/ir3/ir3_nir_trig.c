#include "ir3_nir.h"

#include "nir.h"
#include "nir_builder.h"
#include "nir_search.h"
#include "nir_search_helpers.h"

/* What follows is NIR algebraic transform code for the following 2
 * transforms:
 *    ('fsin', 'x@32') => ('fsin', ('!fmad', 6.2831853, ('ffract', ('!fmad', 0.15915494, 'x', 0.5)), -3.14159265))
 *    ('fcos', 'x@32') => ('fcos', ('!fmad', 6.2831853, ('ffract', ('!fmad', 0.15915494, 'x', 0.5)), -3.14159265))
 */


static const nir_search_value_union ir3_nir_apply_trig_workarounds_values[] = {
   /* ('fsin', 'x@32') => ('fsin', ('!fmad', 6.2831853, ('ffract', ('!fmad', 0.15915494, 'x', 0.5)), -3.14159265)) */
   { .variable = {
      { nir_search_value_variable, 32 },
      0, /* x */
      false,
      -1,
      {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
   } },
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_fsin,
      -1, 0,
      { 0 },
      -1,
   } },

   { .constant = {
      { nir_search_value_constant, 32 },
      nir_type_float, { 0x401921fb53c8d4f1ull /* 6.2831853 */ },
   } },
   { .constant = {
      { nir_search_value_constant, 32 },
      nir_type_float, { 0x3fc45f306725feedull /* 0.15915494 */ },
   } },
   /* replace0_0_1_0_1 -> 0 in the cache */
   { .constant = {
      { nir_search_value_constant, 32 },
      nir_type_float, { 0x3fe0000000000000ull /* 0.5 */ },
   } },
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_exact,
      false,
      -1,
      nir_op_fmad,
      1, 1,
      { 3, 0, 4 },
      -1,
   } },
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_ffract,
      -1, 1,
      { 5 },
      -1,
   } },
   { .constant = {
      { nir_search_value_constant, 32 },
      nir_type_float, { 0xc00921fb53c8d4f1ull /* -3.14159265 */ },
   } },
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_exact,
      false,
      -1,
      nir_op_fmad,
      0, 2,
      { 2, 6, 7 },
      -1,
   } },
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_fsin,
      -1, 2,
      { 8 },
      -1,
   } },

   /* ('fcos', 'x@32') => ('fcos', ('!fmad', 6.2831853, ('ffract', ('!fmad', 0.15915494, 'x', 0.5)), -3.14159265)) */
   /* search1_0 -> 0 in the cache */
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_fcos,
      -1, 0,
      { 0 },
      -1,
   } },

   /* replace1_0_0 -> 2 in the cache */
   /* replace1_0_1_0_0 -> 3 in the cache */
   /* replace1_0_1_0_1 -> 0 in the cache */
   /* replace1_0_1_0_2 -> 4 in the cache */
   /* replace1_0_1_0 -> 5 in the cache */
   /* replace1_0_1 -> 6 in the cache */
   /* replace1_0_2 -> 7 in the cache */
   /* replace1_0 -> 8 in the cache */
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_fcos,
      -1, 2,
      { 8 },
      -1,
   } },

};



static const struct transform ir3_nir_apply_trig_workarounds_transforms[] = {
   { ~0, ~0, ~0 }, /* Sentinel */

   { 1, 9, 0 },
   { ~0, ~0, ~0 }, /* Sentinel */

   { 10, 11, 0 },
   { ~0, ~0, ~0 }, /* Sentinel */

};

static const struct per_op_table ir3_nir_apply_trig_workarounds_pass_op_table[nir_num_search_ops] = {
   [nir_op_fsin] = {
      .filter = NULL,
      
      .num_filtered_states = 1,
      .table = (const uint16_t []) {
      
         2,
      },
   },
   [nir_op_fcos] = {
      .filter = NULL,
      
      .num_filtered_states = 1,
      .table = (const uint16_t []) {
      
         3,
      },
   },
};

/* Mapping from state index to offset in transforms (0 being no transforms) */
static const uint16_t ir3_nir_apply_trig_workarounds_transform_offsets[] = {
   0,
   0,
   1,
   3,
};

static const nir_algebraic_table ir3_nir_apply_trig_workarounds_table = {
   .transforms = ir3_nir_apply_trig_workarounds_transforms,
   .transform_offsets = ir3_nir_apply_trig_workarounds_transform_offsets,
   .pass_op_table = ir3_nir_apply_trig_workarounds_pass_op_table,
   .values = ir3_nir_apply_trig_workarounds_values,
   .expression_cond = NULL,
   .variable_cond = NULL,
};

bool
ir3_nir_apply_trig_workarounds(
   nir_shader *shader
) {
   bool progress = false;
   bool condition_flags[1];
   const nir_shader_compiler_options *options = shader->options;
   const shader_info *info = &shader->info;
   (void) options;
   (void) info;

   STATIC_ASSERT(12 == ARRAY_SIZE(ir3_nir_apply_trig_workarounds_values));
   condition_flags[0] = true;

   nir_foreach_function_impl(impl, shader) {
     progress |= nir_algebraic_impl(impl, condition_flags, &ir3_nir_apply_trig_workarounds_table);
   }

   return progress;
}

