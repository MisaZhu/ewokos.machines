#include "ir3_nir.h"

#include "nir.h"
#include "nir_builder.h"
#include "nir_search.h"
#include "nir_search_helpers.h"

/* What follows is NIR algebraic transform code for the following 2
 * transforms:
 *    ('inot(is_only_used_by_if)', ('iand', 'a', 'b')) => ('ior', ('inot', 'a'), ('inot', 'b'))
 *    ('inot(is_only_used_by_if)', ('ior', 'a', 'b')) => ('iand', ('inot', 'a'), ('inot', 'b'))
 */


static const nir_search_value_union ir3_nir_opt_branch_and_or_not_values[] = {
   /* ('inot(is_only_used_by_if)', ('iand', 'a', 'b')) => ('ior', ('inot', 'a'), ('inot', 'b')) */
   { .variable = {
      { nir_search_value_variable, -2 },
      0, /* a */
      false,
      -1,
      {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
   } },
   { .variable = {
      { nir_search_value_variable, -2 },
      1, /* b */
      false,
      -1,
      {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
   } },
   { .expression = {
      { nir_search_value_expression, -2 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_iand,
      0, 1,
      { 0, 1 },
      -1,
   } },
   { .expression = {
      { nir_search_value_expression, -2 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_inot,
      -1, 1,
      { 2 },
      0,
   } },

   /* replace0_0_0 -> 0 in the cache */
   { .expression = {
      { nir_search_value_expression, -2 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_inot,
      -1, 0,
      { 0 },
      -1,
   } },
   /* replace0_1_0 -> 1 in the cache */
   { .expression = {
      { nir_search_value_expression, -2 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_inot,
      -1, 0,
      { 1 },
      -1,
   } },
   { .expression = {
      { nir_search_value_expression, -2 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_ior,
      0, 1,
      { 4, 5 },
      -1,
   } },

   /* ('inot(is_only_used_by_if)', ('ior', 'a', 'b')) => ('iand', ('inot', 'a'), ('inot', 'b')) */
   /* search1_0_0 -> 0 in the cache */
   /* search1_0_1 -> 1 in the cache */
   { .expression = {
      { nir_search_value_expression, -2 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_ior,
      0, 1,
      { 0, 1 },
      -1,
   } },
   { .expression = {
      { nir_search_value_expression, -2 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_inot,
      -1, 1,
      { 7 },
      0,
   } },

   /* replace1_0_0 -> 0 in the cache */
   /* replace1_0 -> 4 in the cache */
   /* replace1_1_0 -> 1 in the cache */
   /* replace1_1 -> 5 in the cache */
   { .expression = {
      { nir_search_value_expression, -2 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_iand,
      0, 1,
      { 4, 5 },
      -1,
   } },

};

UNUSED static const nir_search_expression_cond ir3_nir_opt_branch_and_or_not_expression_cond[] = {
   is_only_used_by_if,
};


static const struct transform ir3_nir_opt_branch_and_or_not_transforms[] = {
   { ~0, ~0, ~0 }, /* Sentinel */

   { 3, 6, 0 },
   { ~0, ~0, ~0 }, /* Sentinel */

   { 8, 9, 0 },
   { ~0, ~0, ~0 }, /* Sentinel */

};

static const struct per_op_table ir3_nir_opt_branch_and_or_not_pass_op_table[nir_num_search_ops] = {
   [nir_op_inot] = {
      .filter = (const uint16_t []) {
         0,
         0,
         1,
         2,
         0,
         0,
      },
      
      .num_filtered_states = 3,
      .table = (const uint16_t []) {
      
         0,
         4,
         5,
      },
   },
   [nir_op_iand] = {
      .filter = NULL,
      
      .num_filtered_states = 1,
      .table = (const uint16_t []) {
      
         2,
      },
   },
   [nir_op_ior] = {
      .filter = NULL,
      
      .num_filtered_states = 1,
      .table = (const uint16_t []) {
      
         3,
      },
   },
};

/* Mapping from state index to offset in transforms (0 being no transforms) */
static const uint16_t ir3_nir_opt_branch_and_or_not_transform_offsets[] = {
   0,
   0,
   0,
   0,
   1,
   3,
};

static const nir_algebraic_table ir3_nir_opt_branch_and_or_not_table = {
   .transforms = ir3_nir_opt_branch_and_or_not_transforms,
   .transform_offsets = ir3_nir_opt_branch_and_or_not_transform_offsets,
   .pass_op_table = ir3_nir_opt_branch_and_or_not_pass_op_table,
   .values = ir3_nir_opt_branch_and_or_not_values,
   .expression_cond = ir3_nir_opt_branch_and_or_not_expression_cond,
   .variable_cond = NULL,
};

bool
ir3_nir_opt_branch_and_or_not(
   nir_shader *shader
) {
   bool progress = false;
   bool condition_flags[1];
   const nir_shader_compiler_options *options = shader->options;
   const shader_info *info = &shader->info;
   (void) options;
   (void) info;

   STATIC_ASSERT(10 == ARRAY_SIZE(ir3_nir_opt_branch_and_or_not_values));
   condition_flags[0] = true;

   nir_foreach_function_impl(impl, shader) {
     progress |= nir_algebraic_impl(impl, condition_flags, &ir3_nir_opt_branch_and_or_not_table);
   }

   return progress;
}

