#include "ir3_nir.h"

#include "nir.h"
#include "nir_builder.h"
#include "nir_search.h"
#include "nir_search_helpers.h"

/* What follows is NIR algebraic transform code for the following 2
 * transforms:
 *    ('imul', 'a@32', 'b@32') => ('imadsh_mix16', 'b', 'a', ('imadsh_mix16', 'a', 'b', ('umul_16x16', 'a', 'b')))
 *    ('iadd', ('imul24', 'a', 'b'), 'c') => ('imad24_ir3', 'a', 'b', 'c')
 */


static const nir_search_value_union ir3_nir_lower_imul_values[] = {
   /* ('imul', 'a@32', 'b@32') => ('imadsh_mix16', 'b', 'a', ('imadsh_mix16', 'a', 'b', ('umul_16x16', 'a', 'b'))) */
   { .variable = {
      { nir_search_value_variable, 32 },
      0, /* a */
      false,
      -1,
      {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
   } },
   { .variable = {
      { nir_search_value_variable, 32 },
      1, /* b */
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
      nir_op_imul,
      0, 1,
      { 0, 1 },
      -1,
   } },

   /* replace0_0 -> 1 in the cache */
   /* replace0_1 -> 0 in the cache */
   /* replace0_2_0 -> 0 in the cache */
   /* replace0_2_1 -> 1 in the cache */
   /* replace0_2_2_0 -> 0 in the cache */
   /* replace0_2_2_1 -> 1 in the cache */
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_umul_16x16,
      0, 1,
      { 0, 1 },
      -1,
   } },
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_imadsh_mix16,
      -1, 1,
      { 0, 1, 3 },
      -1,
   } },
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_imadsh_mix16,
      -1, 1,
      { 1, 0, 4 },
      -1,
   } },

   /* ('iadd', ('imul24', 'a', 'b'), 'c') => ('imad24_ir3', 'a', 'b', 'c') */
   /* search1_0_0 -> 0 in the cache */
   /* search1_0_1 -> 1 in the cache */
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_imul24,
      1, 1,
      { 0, 1 },
      -1,
   } },
   { .variable = {
      { nir_search_value_variable, 32 },
      2, /* c */
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
      nir_op_iadd,
      0, 2,
      { 6, 7 },
      -1,
   } },

   /* replace1_0 -> 0 in the cache */
   /* replace1_1 -> 1 in the cache */
   /* replace1_2 -> 7 in the cache */
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_imad24_ir3,
      0, 1,
      { 0, 1, 7 },
      -1,
   } },

};



static const struct transform ir3_nir_lower_imul_transforms[] = {
   { ~0, ~0, ~0 }, /* Sentinel */

   { 2, 5, 0 },
   { ~0, ~0, ~0 }, /* Sentinel */

   { 8, 9, 0 },
   { ~0, ~0, ~0 }, /* Sentinel */

};

static const struct per_op_table ir3_nir_lower_imul_pass_op_table[nir_num_search_ops] = {
   [nir_op_imul] = {
      .filter = NULL,
      
      .num_filtered_states = 1,
      .table = (const uint16_t []) {
      
         2,
      },
   },
   [nir_op_iadd] = {
      .filter = (const uint16_t []) {
         0,
         0,
         0,
         1,
         0,
      },
      
      .num_filtered_states = 2,
      .table = (const uint16_t []) {
      
         0,
         4,
         4,
         4,
      },
   },
   [nir_op_imul24] = {
      .filter = NULL,
      
      .num_filtered_states = 1,
      .table = (const uint16_t []) {
      
         3,
      },
   },
};

/* Mapping from state index to offset in transforms (0 being no transforms) */
static const uint16_t ir3_nir_lower_imul_transform_offsets[] = {
   0,
   0,
   1,
   0,
   3,
};

static const nir_algebraic_table ir3_nir_lower_imul_table = {
   .transforms = ir3_nir_lower_imul_transforms,
   .transform_offsets = ir3_nir_lower_imul_transform_offsets,
   .pass_op_table = ir3_nir_lower_imul_pass_op_table,
   .values = ir3_nir_lower_imul_values,
   .expression_cond = NULL,
   .variable_cond = NULL,
};

bool
ir3_nir_lower_imul(
   nir_shader *shader
) {
   bool progress = false;
   bool condition_flags[1];
   const nir_shader_compiler_options *options = shader->options;
   const shader_info *info = &shader->info;
   (void) options;
   (void) info;

   STATIC_ASSERT(10 == ARRAY_SIZE(ir3_nir_lower_imul_values));
   condition_flags[0] = true;

   nir_foreach_function_impl(impl, shader) {
     progress |= nir_algebraic_impl(impl, condition_flags, &ir3_nir_lower_imul_table);
   }

   return progress;
}

