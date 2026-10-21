#include "ir3_nir.h"

#include "nir.h"
#include "nir_builder.h"
#include "nir_search.h"
#include "nir_search_helpers.h"

/* What follows is NIR algebraic transform code for the following 5
 * transforms:
 *    ('ior', ('ushr(is_only_used_by_ior)', 'a', 'b'), 'c') => ('shrg_ir3', 'a', 'b', 'c')
 *    ('ior', ('ishl(is_only_used_by_ior)', 'a', 'b'), 'c') => ('shlg_ir3', 'a', 'b', 'c')
 *    ('iand', ('ushr(is_only_used_by_iand)', 'a', 'b'), 'c') => ('shrm_ir3', 'a', 'b', 'c')
 *    ('iand', ('ishl(is_only_used_by_iand)', 'a', 'b'), 'c') => ('shlm_ir3', 'a', 'b', 'c')
 *    ('ior', ('iand(is_only_used_by_ior)', 'a', 'b'), 'c') => ('andg_ir3', 'a', 'b', 'c')
 */


static const nir_search_value_union ir3_nir_opt_triops_bitwise_values[] = {
   /* ('ior', ('ushr(is_only_used_by_ior)', 'a', 'b'), 'c') => ('shrg_ir3', 'a', 'b', 'c') */
   { .variable = {
      { nir_search_value_variable, -3 },
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
      { nir_search_value_expression, -3 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_ushr,
      -1, 0,
      { 0, 1 },
      0,
   } },
   { .variable = {
      { nir_search_value_variable, -3 },
      2, /* c */
      false,
      -1,
      {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
   } },
   { .expression = {
      { nir_search_value_expression, -3 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_ior,
      0, 1,
      { 2, 3 },
      -1,
   } },

   /* replace0_0 -> 0 in the cache */
   /* replace0_1 -> 1 in the cache */
   /* replace0_2 -> 3 in the cache */
   { .expression = {
      { nir_search_value_expression, -3 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_shrg_ir3,
      -1, 0,
      { 0, 1, 3 },
      -1,
   } },

   /* ('ior', ('ishl(is_only_used_by_ior)', 'a', 'b'), 'c') => ('shlg_ir3', 'a', 'b', 'c') */
   /* search1_0_0 -> 0 in the cache */
   /* search1_0_1 -> 1 in the cache */
   { .expression = {
      { nir_search_value_expression, -3 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_ishl,
      -1, 0,
      { 0, 1 },
      0,
   } },
   /* search1_1 -> 3 in the cache */
   { .expression = {
      { nir_search_value_expression, -3 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_ior,
      0, 1,
      { 6, 3 },
      -1,
   } },

   /* replace1_0 -> 0 in the cache */
   /* replace1_1 -> 1 in the cache */
   /* replace1_2 -> 3 in the cache */
   { .expression = {
      { nir_search_value_expression, -3 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_shlg_ir3,
      -1, 0,
      { 0, 1, 3 },
      -1,
   } },

   /* ('iand', ('ushr(is_only_used_by_iand)', 'a', 'b'), 'c') => ('shrm_ir3', 'a', 'b', 'c') */
   /* search2_0_0 -> 0 in the cache */
   /* search2_0_1 -> 1 in the cache */
   { .expression = {
      { nir_search_value_expression, -3 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_ushr,
      -1, 0,
      { 0, 1 },
      1,
   } },
   /* search2_1 -> 3 in the cache */
   { .expression = {
      { nir_search_value_expression, -3 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_iand,
      0, 1,
      { 9, 3 },
      -1,
   } },

   /* replace2_0 -> 0 in the cache */
   /* replace2_1 -> 1 in the cache */
   /* replace2_2 -> 3 in the cache */
   { .expression = {
      { nir_search_value_expression, -3 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_shrm_ir3,
      -1, 0,
      { 0, 1, 3 },
      -1,
   } },

   /* ('iand', ('ishl(is_only_used_by_iand)', 'a', 'b'), 'c') => ('shlm_ir3', 'a', 'b', 'c') */
   /* search3_0_0 -> 0 in the cache */
   /* search3_0_1 -> 1 in the cache */
   { .expression = {
      { nir_search_value_expression, -3 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_ishl,
      -1, 0,
      { 0, 1 },
      1,
   } },
   /* search3_1 -> 3 in the cache */
   { .expression = {
      { nir_search_value_expression, -3 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_iand,
      0, 1,
      { 12, 3 },
      -1,
   } },

   /* replace3_0 -> 0 in the cache */
   /* replace3_1 -> 1 in the cache */
   /* replace3_2 -> 3 in the cache */
   { .expression = {
      { nir_search_value_expression, -3 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_shlm_ir3,
      -1, 0,
      { 0, 1, 3 },
      -1,
   } },

   /* ('ior', ('iand(is_only_used_by_ior)', 'a', 'b'), 'c') => ('andg_ir3', 'a', 'b', 'c') */
   /* search4_0_0 -> 0 in the cache */
   { .variable = {
      { nir_search_value_variable, -3 },
      1, /* b */
      false,
      -1,
      {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
   } },
   { .expression = {
      { nir_search_value_expression, -3 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_iand,
      1, 1,
      { 0, 15 },
      0,
   } },
   /* search4_1 -> 3 in the cache */
   { .expression = {
      { nir_search_value_expression, -3 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_ior,
      0, 2,
      { 16, 3 },
      -1,
   } },

   /* replace4_0 -> 0 in the cache */
   /* replace4_1 -> 15 in the cache */
   /* replace4_2 -> 3 in the cache */
   { .expression = {
      { nir_search_value_expression, -3 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_andg_ir3,
      0, 1,
      { 0, 15, 3 },
      -1,
   } },

};

UNUSED static const nir_search_expression_cond ir3_nir_opt_triops_bitwise_expression_cond[] = {
   is_only_used_by_ior,
   is_only_used_by_iand,
};


static const struct transform ir3_nir_opt_triops_bitwise_transforms[] = {
   { ~0, ~0, ~0 }, /* Sentinel */

   { 4, 5, 0 },
   { ~0, ~0, ~0 }, /* Sentinel */

   { 7, 8, 0 },
   { ~0, ~0, ~0 }, /* Sentinel */

   { 17, 18, 0 },
   { ~0, ~0, ~0 }, /* Sentinel */

   { 4, 5, 0 },
   { 7, 8, 0 },
   { ~0, ~0, ~0 }, /* Sentinel */

   { 4, 5, 0 },
   { 17, 18, 0 },
   { ~0, ~0, ~0 }, /* Sentinel */

   { 7, 8, 0 },
   { 17, 18, 0 },
   { ~0, ~0, ~0 }, /* Sentinel */

   { 10, 11, 0 },
   { ~0, ~0, ~0 }, /* Sentinel */

   { 13, 14, 0 },
   { ~0, ~0, ~0 }, /* Sentinel */

   { 10, 11, 0 },
   { 13, 14, 0 },
   { ~0, ~0, ~0 }, /* Sentinel */

};

static const struct per_op_table ir3_nir_opt_triops_bitwise_pass_op_table[nir_num_search_ops] = {
   [nir_op_ior] = {
      .filter = (const uint16_t []) {
         0,
         0,
         1,
         2,
         3,
         0,
         0,
         0,
         0,
         0,
         0,
         3,
         3,
         3,
      },
      
      .num_filtered_states = 4,
      .table = (const uint16_t []) {
      
         0,
         5,
         6,
         7,
         5,
         5,
         8,
         9,
         6,
         8,
         6,
         10,
         7,
         9,
         10,
         7,
      },
   },
   [nir_op_ushr] = {
      .filter = NULL,
      
      .num_filtered_states = 1,
      .table = (const uint16_t []) {
      
         2,
      },
   },
   [nir_op_ishl] = {
      .filter = NULL,
      
      .num_filtered_states = 1,
      .table = (const uint16_t []) {
      
         3,
      },
   },
   [nir_op_iand] = {
      .filter = (const uint16_t []) {
         0,
         0,
         1,
         2,
         0,
         0,
         0,
         0,
         0,
         0,
         0,
         0,
         0,
         0,
      },
      
      .num_filtered_states = 3,
      .table = (const uint16_t []) {
      
         4,
         11,
         12,
         11,
         11,
         13,
         12,
         13,
         12,
      },
   },
};

/* Mapping from state index to offset in transforms (0 being no transforms) */
static const uint16_t ir3_nir_opt_triops_bitwise_transform_offsets[] = {
   0,
   0,
   0,
   0,
   0,
   1,
   3,
   5,
   7,
   10,
   13,
   16,
   18,
   20,
};

static const nir_algebraic_table ir3_nir_opt_triops_bitwise_table = {
   .transforms = ir3_nir_opt_triops_bitwise_transforms,
   .transform_offsets = ir3_nir_opt_triops_bitwise_transform_offsets,
   .pass_op_table = ir3_nir_opt_triops_bitwise_pass_op_table,
   .values = ir3_nir_opt_triops_bitwise_values,
   .expression_cond = ir3_nir_opt_triops_bitwise_expression_cond,
   .variable_cond = NULL,
};

bool
ir3_nir_opt_triops_bitwise(
   nir_shader *shader
) {
   bool progress = false;
   bool condition_flags[1];
   const nir_shader_compiler_options *options = shader->options;
   const shader_info *info = &shader->info;
   (void) options;
   (void) info;

   STATIC_ASSERT(19 == ARRAY_SIZE(ir3_nir_opt_triops_bitwise_values));
   condition_flags[0] = true;

   nir_foreach_function_impl(impl, shader) {
     progress |= nir_algebraic_impl(impl, condition_flags, &ir3_nir_opt_triops_bitwise_table);
   }

   return progress;
}

