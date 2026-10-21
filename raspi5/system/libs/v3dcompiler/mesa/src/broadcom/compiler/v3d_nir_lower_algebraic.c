#include "v3d_compiler.h"

#include "nir.h"
#include "nir_builder.h"
#include "nir_search.h"
#include "nir_search_helpers.h"

/* What follows is NIR algebraic transform code for the following 11
 * transforms:
 *    ('f2i8', 'a') => ('i2i8', ('f2i32', 'a'))
 *    ('f2i16', 'a') => ('i2i16', ('f2i32', 'a'))
 *    ('f2u8', 'a') => ('u2u8', ('f2u32', 'a'))
 *    ('f2u16', 'a') => ('u2u16', ('f2u32', 'a'))
 *    ('i2f32', 'a@8') => ('i2f32', ('i2i32', 'a'))
 *    ('i2f32', 'a@16') => ('i2f32', ('i2i32', 'a'))
 *    ('u2f32', 'a@8') => ('u2f32', ('u2u32', 'a'))
 *    ('u2f32', 'a@16') => ('u2f32', ('u2u32', 'a'))
 *    ('fmin', ('fmax', 'a@32', -1.0), 1.0) => ('fsat_signed', 'a')
 *    ('fmax', ('fmin', 'a@32', 1.0), -1.0) => ('fsat_signed', 'a')
 *    ('fmax', 'a@32', 0.0) => ('fclamp_pos', 'a')
 */


static const nir_search_value_union v3d_nir_lower_algebraic_values[] = {
   /* ('f2i8', 'a') => ('i2i8', ('f2i32', 'a')) */
   { .variable = {
      { nir_search_value_variable, -1 },
      0, /* a */
      false,
      -1,
      {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
   } },
   { .expression = {
      { nir_search_value_expression, 8 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_f2i8,
      -1, 0,
      { 0 },
      -1,
   } },

   /* replace0_0_0 -> 0 in the cache */
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_f2i32,
      -1, 0,
      { 0 },
      -1,
   } },
   { .expression = {
      { nir_search_value_expression, 8 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_i2i8,
      -1, 0,
      { 2 },
      -1,
   } },

   /* ('f2i16', 'a') => ('i2i16', ('f2i32', 'a')) */
   /* search1_0 -> 0 in the cache */
   { .expression = {
      { nir_search_value_expression, 16 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_f2i16,
      -1, 0,
      { 0 },
      -1,
   } },

   /* replace1_0_0 -> 0 in the cache */
   /* replace1_0 -> 2 in the cache */
   { .expression = {
      { nir_search_value_expression, 16 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_i2i16,
      -1, 0,
      { 2 },
      -1,
   } },

   /* ('f2u8', 'a') => ('u2u8', ('f2u32', 'a')) */
   /* search2_0 -> 0 in the cache */
   { .expression = {
      { nir_search_value_expression, 8 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_f2u8,
      -1, 0,
      { 0 },
      -1,
   } },

   /* replace2_0_0 -> 0 in the cache */
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_f2u32,
      -1, 0,
      { 0 },
      -1,
   } },
   { .expression = {
      { nir_search_value_expression, 8 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_u2u8,
      -1, 0,
      { 7 },
      -1,
   } },

   /* ('f2u16', 'a') => ('u2u16', ('f2u32', 'a')) */
   /* search3_0 -> 0 in the cache */
   { .expression = {
      { nir_search_value_expression, 16 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_f2u16,
      -1, 0,
      { 0 },
      -1,
   } },

   /* replace3_0_0 -> 0 in the cache */
   /* replace3_0 -> 7 in the cache */
   { .expression = {
      { nir_search_value_expression, 16 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_u2u16,
      -1, 0,
      { 7 },
      -1,
   } },

   /* ('i2f32', 'a@8') => ('i2f32', ('i2i32', 'a')) */
   { .variable = {
      { nir_search_value_variable, 8 },
      0, /* a */
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
      nir_op_i2f32,
      -1, 0,
      { 11 },
      -1,
   } },

   /* replace4_0_0 -> 11 in the cache */
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_i2i32,
      -1, 0,
      { 11 },
      -1,
   } },
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_i2f32,
      -1, 0,
      { 13 },
      -1,
   } },

   /* ('i2f32', 'a@16') => ('i2f32', ('i2i32', 'a')) */
   { .variable = {
      { nir_search_value_variable, 16 },
      0, /* a */
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
      nir_op_i2f32,
      -1, 0,
      { 15 },
      -1,
   } },

   /* replace5_0_0 -> 15 in the cache */
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_i2i32,
      -1, 0,
      { 15 },
      -1,
   } },
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_i2f32,
      -1, 0,
      { 17 },
      -1,
   } },

   /* ('u2f32', 'a@8') => ('u2f32', ('u2u32', 'a')) */
   /* search6_0 -> 11 in the cache */
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_u2f32,
      -1, 0,
      { 11 },
      -1,
   } },

   /* replace6_0_0 -> 11 in the cache */
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_u2u32,
      -1, 0,
      { 11 },
      -1,
   } },
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_u2f32,
      -1, 0,
      { 20 },
      -1,
   } },

   /* ('u2f32', 'a@16') => ('u2f32', ('u2u32', 'a')) */
   /* search7_0 -> 15 in the cache */
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_u2f32,
      -1, 0,
      { 15 },
      -1,
   } },

   /* replace7_0_0 -> 15 in the cache */
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_u2u32,
      -1, 0,
      { 15 },
      -1,
   } },
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_u2f32,
      -1, 0,
      { 23 },
      -1,
   } },

   /* ('fmin', ('fmax', 'a@32', -1.0), 1.0) => ('fsat_signed', 'a') */
   { .variable = {
      { nir_search_value_variable, 32 },
      0, /* a */
      false,
      -1,
      {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
   } },
   { .constant = {
      { nir_search_value_constant, 32 },
      nir_type_float, { 0xbff0000000000000ull /* -1.0 */ },
   } },
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      true,
      -1,
      nir_op_fmax,
      1, 1,
      { 25, 26 },
      -1,
   } },
   { .constant = {
      { nir_search_value_constant, 32 },
      nir_type_float, { 0x3ff0000000000000ull /* 1.0 */ },
   } },
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      true,
      -1,
      nir_op_fmin,
      0, 2,
      { 27, 28 },
      -1,
   } },

   /* replace8_0 -> 25 in the cache */
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_fsat_signed,
      -1, 0,
      { 25 },
      -1,
   } },

   /* ('fmax', ('fmin', 'a@32', 1.0), -1.0) => ('fsat_signed', 'a') */
   /* search9_0_0 -> 25 in the cache */
   /* search9_0_1 -> 28 in the cache */
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      true,
      -1,
      nir_op_fmin,
      1, 1,
      { 25, 28 },
      -1,
   } },
   /* search9_1 -> 26 in the cache */
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      true,
      -1,
      nir_op_fmax,
      0, 2,
      { 31, 26 },
      -1,
   } },

   /* replace9_0 -> 25 in the cache */
   /* replace9 -> 30 in the cache */

   /* ('fmax', 'a@32', 0.0) => ('fclamp_pos', 'a') */
   /* search10_0 -> 25 in the cache */
   { .constant = {
      { nir_search_value_constant, 32 },
      nir_type_float, { 0x0ull /* 0.0 */ },
   } },
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      true,
      -1,
      nir_op_fmax,
      0, 1,
      { 25, 33 },
      -1,
   } },

   /* replace10_0 -> 25 in the cache */
   { .expression = {
      { nir_search_value_expression, 32 },
      nir_fp_fast_math,
      nir_fp_fast_math,
      false,
      -1,
      nir_op_fclamp_pos,
      -1, 0,
      { 25 },
      -1,
   } },

};



static const struct transform v3d_nir_lower_algebraic_transforms[] = {
   { ~0, ~0, ~0 }, /* Sentinel */

   { 1, 3, 0 },
   { 4, 5, 0 },
   { ~0, ~0, ~0 }, /* Sentinel */

   { 6, 8, 0 },
   { 9, 10, 0 },
   { ~0, ~0, ~0 }, /* Sentinel */

   { 12, 14, 0 },
   { 16, 18, 0 },
   { ~0, ~0, ~0 }, /* Sentinel */

   { 19, 21, 0 },
   { 22, 24, 0 },
   { ~0, ~0, ~0 }, /* Sentinel */

   { 34, 35, 2 },
   { ~0, ~0, ~0 }, /* Sentinel */

   { 32, 30, 1 },
   { 34, 35, 2 },
   { ~0, ~0, ~0 }, /* Sentinel */

   { 29, 30, 1 },
   { ~0, ~0, ~0 }, /* Sentinel */

};

static const struct per_op_table v3d_nir_lower_algebraic_pass_op_table[nir_num_search_ops] = {
   [nir_search_op_f2i] = {
      .filter = NULL,
      
      .num_filtered_states = 1,
      .table = (const uint16_t []) {
      
         2,
      },
   },
   [nir_search_op_f2u] = {
      .filter = NULL,
      
      .num_filtered_states = 1,
      .table = (const uint16_t []) {
      
         3,
      },
   },
   [nir_search_op_i2f] = {
      .filter = NULL,
      
      .num_filtered_states = 1,
      .table = (const uint16_t []) {
      
         4,
      },
   },
   [nir_search_op_u2f] = {
      .filter = NULL,
      
      .num_filtered_states = 1,
      .table = (const uint16_t []) {
      
         5,
      },
   },
   [nir_op_fmin] = {
      .filter = (const uint16_t []) {
         0,
         1,
         0,
         0,
         0,
         0,
         0,
         2,
         2,
         0,
      },
      
      .num_filtered_states = 3,
      .table = (const uint16_t []) {
      
         0,
         6,
         0,
         6,
         6,
         9,
         0,
         9,
         0,
      },
   },
   [nir_op_fmax] = {
      .filter = (const uint16_t []) {
         0,
         1,
         0,
         0,
         0,
         0,
         2,
         0,
         0,
         2,
      },
      
      .num_filtered_states = 3,
      .table = (const uint16_t []) {
      
         0,
         7,
         0,
         7,
         7,
         8,
         0,
         8,
         0,
      },
   },
};

/* Mapping from state index to offset in transforms (0 being no transforms) */
static const uint16_t v3d_nir_lower_algebraic_transform_offsets[] = {
   0,
   0,
   1,
   4,
   7,
   10,
   0,
   13,
   15,
   18,
};

static const nir_algebraic_table v3d_nir_lower_algebraic_table = {
   .transforms = v3d_nir_lower_algebraic_transforms,
   .transform_offsets = v3d_nir_lower_algebraic_transform_offsets,
   .pass_op_table = v3d_nir_lower_algebraic_pass_op_table,
   .values = v3d_nir_lower_algebraic_values,
   .expression_cond = NULL,
   .variable_cond = NULL,
};

bool
v3d_nir_lower_algebraic(
   nir_shader *shader
   , const struct v3d_compile * c
) {
   bool progress = false;
   bool condition_flags[3];
   const nir_shader_compiler_options *options = shader->options;
   const shader_info *info = &shader->info;
   (void) options;
   (void) info;

   STATIC_ASSERT(36 == ARRAY_SIZE(v3d_nir_lower_algebraic_values));
   condition_flags[0] = true;
   condition_flags[1] = c && v3d_device_has_unpack_sat(c->devinfo);
   condition_flags[2] = c && v3d_device_has_unpack_max0(c->devinfo);

   nir_foreach_function_impl(impl, shader) {
     progress |= nir_algebraic_impl(impl, condition_flags, &v3d_nir_lower_algebraic_table);
   }

   return progress;
}

