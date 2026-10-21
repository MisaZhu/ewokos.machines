/* Copyright © 2021 Google, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include "freedreno_dev_info.h"
#include "util/u_debug.h"
#include "util/log.h"

#include <stdlib.h>

/* Map python to C: */
#define True true
#define False false

static const struct fd_dev_info __info0 = {.chip=2,.gmem_align_w=32,.gmem_align_h=32,.tile_align_w=32,.tile_align_h=32,.tile_max_w=512,.tile_max_h=-1,.num_vsc_pipes=8,.cs_shared_mem_size=0,.num_sp_cores=0,.wave_granularity=2,.fibers_per_sp=0,.threadsize_base=8,.max_waves=16,.highest_bank_bit=0,.ubwc_swizzle=7,.macrotile_mode=0,};
static const struct fd_dev_info __info1 = {.chip=3,.gmem_align_w=32,.gmem_align_h=32,.tile_align_w=32,.tile_align_h=32,.tile_max_w=992,.tile_max_h=480,.num_vsc_pipes=8,.cs_shared_mem_size=32768,.num_sp_cores=0,.wave_granularity=2,.fibers_per_sp=0,.threadsize_base=8,.max_waves=16,.highest_bank_bit=0,.ubwc_swizzle=7,.macrotile_mode=0,};
static const struct fd_dev_info __info2 = {.chip=4,.gmem_align_w=32,.gmem_align_h=32,.tile_align_w=32,.tile_align_h=32,.tile_max_w=1024,.tile_max_h=480,.num_vsc_pipes=8,.cs_shared_mem_size=32768,.num_sp_cores=0,.wave_granularity=2,.fibers_per_sp=0,.threadsize_base=32,.max_waves=16,.highest_bank_bit=0,.ubwc_swizzle=7,.macrotile_mode=0,};
static const struct fd_dev_info __info3 = {.chip=5,.gmem_align_w=64,.gmem_align_h=32,.tile_align_w=64,.tile_align_h=32,.tile_max_w=1024,.tile_max_h=4064,.num_vsc_pipes=16,.cs_shared_mem_size=32768,.num_sp_cores=1,.wave_granularity=2,.fibers_per_sp=1024,.threadsize_base=32,.max_waves=16,.highest_bank_bit=14,.ubwc_swizzle=7,.macrotile_mode=0,};
static const struct fd_dev_info __info4 = {.chip=5,.gmem_align_w=64,.gmem_align_h=32,.tile_align_w=64,.tile_align_h=32,.tile_max_w=1024,.tile_max_h=4064,.num_vsc_pipes=16,.cs_shared_mem_size=32768,.num_sp_cores=2,.wave_granularity=2,.fibers_per_sp=1024,.threadsize_base=32,.max_waves=16,.highest_bank_bit=14,.ubwc_swizzle=7,.macrotile_mode=0,};
static const struct fd_dev_info __info5 = {.chip=5,.gmem_align_w=64,.gmem_align_h=32,.tile_align_w=64,.tile_align_h=32,.tile_max_w=1024,.tile_max_h=4064,.num_vsc_pipes=16,.cs_shared_mem_size=32768,.num_sp_cores=4,.wave_granularity=2,.fibers_per_sp=1024,.threadsize_base=32,.max_waves=16,.highest_bank_bit=15,.ubwc_swizzle=7,.macrotile_mode=0,};
static const struct fd_dev_info __info6 = {.chip=6,.gmem_align_w=16,.gmem_align_h=4,.tile_align_w=32,.tile_align_h=16,.tile_max_w=1024,.tile_max_h=1024,.num_vsc_pipes=16,.cs_shared_mem_size=16384,.num_sp_cores=1,.wave_granularity=1,.fibers_per_sp=2048,.threadsize_base=64,.max_waves=16,.highest_bank_bit=13,.ubwc_swizzle=7,.macrotile_mode=0,.num_ccu=1,.num_slices=0,.props={.has_cp_reg_write=True,.has_8bpp_ubwc=True,.has_gmem_fast_clear=False,.has_hw_multiview=False,.has_fs_tex_prefetch=False,.has_sampler_minmax=False,.has_astc_hdr=False,.num_xfb_streams=1,.supports_double_threadsize=False,.sysmem_per_ccu_depth_cache_size=8192,.sysmem_per_ccu_color_cache_size=8192,.gmem_ccu_color_cache_fraction=1,.gmem_ccu_depth_cache_fraction=2,.prim_alloc_threshold=7,.vs_max_inputs_count=16,.max_sets=5,.line_width_min=1.0,.line_width_max=1.0,.mov_half_shared_quirk=True,.max_draw_states=32,.max_texel_buffer_range_elements=134217728,.max_storage_buffer_range_bytes=134217728,.reg_size_vec4=48,.instr_cache_size=64,.indirect_draw_wfm_quirk=True,.depth_bounds_require_depth_test_quirk=True,},.magic={.RB_DBG_ECO_CNTL=68157440,.RB_DBG_ECO_CNTL_blit=68157440,.RB_RBP_CNTL=1,},.magic_raw={{38916, 15}, {38917, 0}, {41208, 0}, {46592, 0}, {34304, 0}, {44547, 0}, {44544, 0}, {48644, 0}, {38400, 0}, {3602, 268435456}},};
static const struct fd_dev_info __info7 = {.chip=6,.gmem_align_w=16,.gmem_align_h=4,.tile_align_w=32,.tile_align_h=16,.tile_max_w=1024,.tile_max_h=1024,.num_vsc_pipes=16,.cs_shared_mem_size=16384,.num_sp_cores=1,.wave_granularity=1,.fibers_per_sp=2048,.threadsize_base=64,.max_waves=16,.highest_bank_bit=13,.ubwc_swizzle=7,.macrotile_mode=0,.num_ccu=1,.num_slices=0,.props={.has_cp_reg_write=True,.has_8bpp_ubwc=True,.has_gmem_fast_clear=False,.has_hw_multiview=False,.has_fs_tex_prefetch=False,.has_sampler_minmax=False,.has_astc_hdr=False,.num_xfb_streams=1,.supports_double_threadsize=False,.sysmem_per_ccu_depth_cache_size=8192,.sysmem_per_ccu_color_cache_size=8192,.gmem_ccu_color_cache_fraction=1,.gmem_ccu_depth_cache_fraction=2,.prim_alloc_threshold=7,.vs_max_inputs_count=16,.max_sets=5,.line_width_min=1.0,.line_width_max=1.0,.mov_half_shared_quirk=True,.max_draw_states=32,.max_texel_buffer_range_elements=134217728,.max_storage_buffer_range_bytes=134217728,.reg_size_vec4=32,.instr_cache_size=64,.indirect_draw_wfm_quirk=True,.depth_bounds_require_depth_test_quirk=True,},.magic={.RB_DBG_ECO_CNTL=68157440,.RB_DBG_ECO_CNTL_blit=68157440,.RB_RBP_CNTL=1,},.magic_raw={{38916, 15}, {38917, 0}, {41208, 0}, {46592, 0}, {34304, 0}, {44547, 0}, {44544, 0}, {48644, 0}, {38400, 0}, {3602, 268435456}},};
static const struct fd_dev_info __info8 = {.chip=6,.gmem_align_w=16,.gmem_align_h=4,.tile_align_w=32,.tile_align_h=32,.tile_max_w=1024,.tile_max_h=1024,.num_vsc_pipes=32,.cs_shared_mem_size=32768,.num_sp_cores=1,.wave_granularity=2,.fibers_per_sp=2048,.threadsize_base=64,.max_waves=16,.highest_bank_bit=14,.ubwc_swizzle=6,.macrotile_mode=0,.num_ccu=1,.num_slices=0,.props={.has_cp_reg_write=True,.has_8bpp_ubwc=True,.has_gmem_fast_clear=True,.has_hw_multiview=True,.has_fs_tex_prefetch=True,.has_sampler_minmax=True,.has_astc_hdr=True,.num_xfb_streams=4,.supports_double_threadsize=True,.sysmem_per_ccu_depth_cache_size=65536,.sysmem_per_ccu_color_cache_size=65536,.gmem_ccu_color_cache_fraction=2,.gmem_ccu_depth_cache_fraction=2,.prim_alloc_threshold=7,.vs_max_inputs_count=32,.max_sets=5,.line_width_min=1.0,.line_width_max=1.0,.mov_half_shared_quirk=True,.max_draw_states=32,.max_texel_buffer_range_elements=134217728,.max_storage_buffer_range_bytes=134217728,.reg_size_vec4=96,.instr_cache_size=64,.indirect_draw_wfm_quirk=True,.depth_bounds_require_depth_test_quirk=True,.blit_wfi_quirk=True,},.magic={.RB_DBG_ECO_CNTL=68157440,.RB_DBG_ECO_CNTL_blit=68157440,.RB_RBP_CNTL=1,},.magic_raw={{38916, 31}, {38917, 0}, {41208, 0}, {46592, 1081344}, {34304, 2176}, {44547, 1072}, {44544, 0}, {48644, 524288}, {38400, 0}, {3602, 1}},};
static const struct fd_dev_info __info9 = {.chip=6,.gmem_align_w=16,.gmem_align_h=4,.tile_align_w=32,.tile_align_h=16,.tile_max_w=1024,.tile_max_h=1024,.num_vsc_pipes=32,.cs_shared_mem_size=32768,.num_sp_cores=1,.wave_granularity=2,.fibers_per_sp=2048,.threadsize_base=64,.max_waves=16,.highest_bank_bit=15,.ubwc_swizzle=6,.macrotile_mode=1,.num_ccu=1,.num_slices=0,.props={.has_cp_reg_write=True,.has_8bpp_ubwc=True,.has_gmem_fast_clear=True,.has_hw_multiview=True,.has_fs_tex_prefetch=True,.has_sampler_minmax=True,.has_astc_hdr=True,.num_xfb_streams=4,.supports_double_threadsize=True,.sysmem_per_ccu_depth_cache_size=65536,.sysmem_per_ccu_color_cache_size=65536,.gmem_ccu_color_cache_fraction=2,.gmem_ccu_depth_cache_fraction=2,.prim_alloc_threshold=7,.vs_max_inputs_count=32,.max_sets=5,.line_width_min=1.0,.line_width_max=1.0,.mov_half_shared_quirk=True,.max_draw_states=32,.max_texel_buffer_range_elements=134217728,.max_storage_buffer_range_bytes=134217728,.reg_size_vec4=96,.instr_cache_size=64,.indirect_draw_wfm_quirk=True,.depth_bounds_require_depth_test_quirk=True,},.magic={.RB_DBG_ECO_CNTL=68157440,.RB_DBG_ECO_CNTL_blit=68157440,.RB_RBP_CNTL=0,},.magic_raw={{38916, 31}, {38917, 0}, {41208, 0}, {46592, 16809984}, {34304, 0}, {44547, 1024}, {44544, 16777216}, {48644, 0}, {38400, 33554432}, {3602, 1}},};
static const struct fd_dev_info __info10 = {.chip=6,.gmem_align_w=16,.gmem_align_h=4,.tile_align_w=96,.tile_align_h=16,.tile_max_w=1024,.tile_max_h=1024,.num_vsc_pipes=32,.cs_shared_mem_size=32768,.num_sp_cores=2,.wave_granularity=2,.fibers_per_sp=4096,.threadsize_base=64,.max_waves=16,.highest_bank_bit=15,.ubwc_swizzle=6,.macrotile_mode=1,.num_ccu=2,.num_slices=0,.props={.has_cp_reg_write=True,.has_8bpp_ubwc=False,.has_gmem_fast_clear=True,.has_hw_multiview=True,.has_fs_tex_prefetch=True,.has_sampler_minmax=True,.has_astc_hdr=True,.num_xfb_streams=4,.supports_double_threadsize=True,.sysmem_per_ccu_depth_cache_size=65536,.sysmem_per_ccu_color_cache_size=65536,.gmem_ccu_color_cache_fraction=2,.gmem_ccu_depth_cache_fraction=2,.prim_alloc_threshold=7,.vs_max_inputs_count=32,.max_sets=5,.line_width_min=1.0,.line_width_max=1.0,.mov_half_shared_quirk=True,.max_draw_states=32,.max_texel_buffer_range_elements=134217728,.max_storage_buffer_range_bytes=134217728,.reg_size_vec4=64,.instr_cache_size=127,.supports_multiview_mask=True,.has_z24uint_s8uint=True,.tess_use_shared=True,.storage_16bit=True,.has_tex_filter_cubic=True,.has_separate_chroma_filter=True,.has_sample_locations=True,.has_dp2acc=True,.has_lrz_dir_tracking=True,.enable_lrz_fast_clear=True,.lrz_track_quirk=False,.has_lrz_feedback=True,.has_per_view_viewport=True,.has_scalar_alu=True,.has_early_preamble=True,.prede_nop_quirk=True,.has_pred_bit=True,.has_pc_dgen_so_cntl=True,.round_robin_errata=True,},.magic={.RB_DBG_ECO_CNTL=68157440,.RB_DBG_ECO_CNTL_blit=68157440,.RB_RBP_CNTL=0,},.magic_raw={{38916, 31}, {38917, 0}, {41208, 0}, {46592, 16809984}, {34304, 0}, {44547, 5120}, {44544, 50331648}, {48644, 0}, {38400, 33554432}, {3602, 1}},};
static const struct fd_dev_info __info11 = {.chip=6,.gmem_align_w=16,.gmem_align_h=4,.tile_align_w=32,.tile_align_h=16,.tile_max_w=1024,.tile_max_h=1024,.num_vsc_pipes=32,.cs_shared_mem_size=32768,.num_sp_cores=2,.wave_granularity=2,.fibers_per_sp=2048,.threadsize_base=64,.max_waves=16,.highest_bank_bit=15,.ubwc_swizzle=6,.macrotile_mode=0,.num_ccu=2,.num_slices=0,.props={.has_cp_reg_write=True,.has_8bpp_ubwc=True,.has_gmem_fast_clear=True,.has_hw_multiview=True,.has_fs_tex_prefetch=True,.has_sampler_minmax=True,.has_astc_hdr=True,.num_xfb_streams=4,.supports_double_threadsize=True,.sysmem_per_ccu_depth_cache_size=65536,.sysmem_per_ccu_color_cache_size=65536,.gmem_ccu_color_cache_fraction=2,.gmem_ccu_depth_cache_fraction=2,.prim_alloc_threshold=7,.vs_max_inputs_count=32,.max_sets=5,.line_width_min=1.0,.line_width_max=1.0,.mov_half_shared_quirk=True,.max_draw_states=32,.max_texel_buffer_range_elements=134217728,.max_storage_buffer_range_bytes=134217728,.reg_size_vec4=96,.instr_cache_size=64,.indirect_draw_wfm_quirk=True,.depth_bounds_require_depth_test_quirk=True,},.magic={.RB_DBG_ECO_CNTL=68157440,.RB_DBG_ECO_CNTL_blit=84934656,.RB_RBP_CNTL=1,},.magic_raw={{38916, 31}, {38917, 1}, {41208, 1}, {46592, 1081344}, {34304, 2176}, {44547, 5168}, {44544, 0}, {48644, 524288}, {38400, 0}, {3602, 268435457}},};
static const struct fd_dev_info __info12 = {.chip=6,.gmem_align_w=16,.gmem_align_h=4,.tile_align_w=32,.tile_align_h=16,.tile_max_w=1024,.tile_max_h=1024,.num_vsc_pipes=32,.cs_shared_mem_size=32768,.num_sp_cores=2,.wave_granularity=2,.fibers_per_sp=8192,.threadsize_base=64,.max_waves=16,.highest_bank_bit=15,.ubwc_swizzle=6,.macrotile_mode=0,.num_ccu=2,.num_slices=0,.props={.has_cp_reg_write=True,.has_8bpp_ubwc=False,.has_gmem_fast_clear=True,.has_hw_multiview=True,.has_fs_tex_prefetch=True,.has_sampler_minmax=True,.has_astc_hdr=True,.num_xfb_streams=4,.supports_double_threadsize=True,.sysmem_per_ccu_depth_cache_size=65536,.sysmem_per_ccu_color_cache_size=65536,.gmem_ccu_color_cache_fraction=2,.gmem_ccu_depth_cache_fraction=2,.prim_alloc_threshold=7,.vs_max_inputs_count=32,.max_sets=5,.line_width_min=1.0,.line_width_max=1.0,.mov_half_shared_quirk=True,.max_draw_states=32,.max_texel_buffer_range_elements=134217728,.max_storage_buffer_range_bytes=134217728,.reg_size_vec4=96,.instr_cache_size=64,.supports_multiview_mask=True,.has_z24uint_s8uint=True,.indirect_draw_wfm_quirk=True,.depth_bounds_require_depth_test_quirk=True,.has_dp2acc=False,},.magic={.RB_DBG_ECO_CNTL=68157440,.RB_DBG_ECO_CNTL_blit=68157440,.RB_RBP_CNTL=1,},.magic_raw={{38916, 31}, {38917, 1}, {41208, 1}, {46592, 32768}, {34304, 0}, {44547, 1056}, {44544, 0}, {48644, 0}, {38400, 33554432}, {3602, 1}},};
static const struct fd_dev_info __info13 = {.chip=6,.gmem_align_w=16,.gmem_align_h=4,.tile_align_w=64,.tile_align_h=32,.tile_max_w=1024,.tile_max_h=1024,.num_vsc_pipes=32,.cs_shared_mem_size=32768,.num_sp_cores=4,.wave_granularity=2,.fibers_per_sp=8192,.threadsize_base=64,.max_waves=16,.highest_bank_bit=15,.ubwc_swizzle=6,.macrotile_mode=0,.num_ccu=4,.num_slices=0,.props={.has_cp_reg_write=True,.has_8bpp_ubwc=False,.has_gmem_fast_clear=True,.has_hw_multiview=True,.has_fs_tex_prefetch=True,.has_sampler_minmax=True,.has_astc_hdr=True,.num_xfb_streams=4,.supports_double_threadsize=True,.sysmem_per_ccu_depth_cache_size=65536,.sysmem_per_ccu_color_cache_size=65536,.gmem_ccu_color_cache_fraction=2,.gmem_ccu_depth_cache_fraction=2,.prim_alloc_threshold=7,.vs_max_inputs_count=32,.max_sets=5,.line_width_min=1.0,.line_width_max=1.0,.mov_half_shared_quirk=True,.max_draw_states=32,.max_texel_buffer_range_elements=134217728,.max_storage_buffer_range_bytes=134217728,.reg_size_vec4=96,.instr_cache_size=64,.supports_multiview_mask=True,.has_z24uint_s8uint=True,.indirect_draw_wfm_quirk=True,.depth_bounds_require_depth_test_quirk=True,.has_dp2acc=False,},.magic={.RB_DBG_ECO_CNTL=68157440,.RB_DBG_ECO_CNTL_blit=68157440,.RB_RBP_CNTL=1,},.magic_raw={{38916, 31}, {38917, 3}, {41208, 3}, {46592, 1081344}, {34304, 0}, {44547, 5168}, {44544, 0}, {48644, 0}, {38400, 33554432}, {3602, 1}},};
static const struct fd_dev_info __info14 = {.chip=6,.gmem_align_w=16,.gmem_align_h=4,.tile_align_w=96,.tile_align_h=16,.tile_max_w=1024,.tile_max_h=1024,.num_vsc_pipes=32,.cs_shared_mem_size=32768,.num_sp_cores=3,.wave_granularity=2,.fibers_per_sp=4096,.threadsize_base=64,.max_waves=16,.highest_bank_bit=16,.ubwc_swizzle=6,.macrotile_mode=1,.num_ccu=3,.num_slices=0,.props={.has_cp_reg_write=True,.has_8bpp_ubwc=False,.has_gmem_fast_clear=True,.has_hw_multiview=True,.has_fs_tex_prefetch=True,.has_sampler_minmax=True,.has_astc_hdr=True,.num_xfb_streams=4,.supports_double_threadsize=True,.sysmem_per_ccu_depth_cache_size=65536,.sysmem_per_ccu_color_cache_size=65536,.gmem_ccu_color_cache_fraction=2,.gmem_ccu_depth_cache_fraction=2,.prim_alloc_threshold=7,.vs_max_inputs_count=32,.max_sets=5,.line_width_min=1.0,.line_width_max=1.0,.mov_half_shared_quirk=True,.max_draw_states=32,.max_texel_buffer_range_elements=134217728,.max_storage_buffer_range_bytes=134217728,.reg_size_vec4=64,.instr_cache_size=127,.supports_multiview_mask=True,.has_z24uint_s8uint=True,.tess_use_shared=True,.storage_16bit=True,.has_tex_filter_cubic=True,.has_separate_chroma_filter=True,.has_sample_locations=True,.has_dp2acc=True,.has_lrz_dir_tracking=True,.enable_lrz_fast_clear=True,.lrz_track_quirk=True,.has_lrz_feedback=True,.has_per_view_viewport=True,.has_scalar_alu=True,.has_early_preamble=True,.prede_nop_quirk=True,.has_pred_bit=True,.has_pc_dgen_so_cntl=True,.round_robin_errata=True,},.magic={.RB_DBG_ECO_CNTL=68157440,.RB_DBG_ECO_CNTL_blit=68157440,.RB_RBP_CNTL=0,},.magic_raw={{38916, 31}, {38917, 2}, {41208, 2}, {46592, 1081344}, {34304, 0}, {44547, 5120}, {44544, 16777216}, {48644, 0}, {38400, 33554432}, {3602, 1}},};
static const struct fd_dev_info __info15 = {.chip=6,.gmem_align_w=16,.gmem_align_h=4,.tile_align_w=32,.tile_align_h=16,.tile_max_w=1024,.tile_max_h=1024,.num_vsc_pipes=32,.cs_shared_mem_size=32768,.num_sp_cores=2,.wave_granularity=2,.fibers_per_sp=4096,.threadsize_base=64,.max_waves=16,.highest_bank_bit=14,.ubwc_swizzle=6,.macrotile_mode=1,.num_ccu=2,.num_slices=0,.props={.has_cp_reg_write=False,.has_8bpp_ubwc=False,.has_gmem_fast_clear=True,.has_hw_multiview=True,.has_fs_tex_prefetch=True,.has_sampler_minmax=True,.has_astc_hdr=True,.num_xfb_streams=4,.supports_double_threadsize=True,.sysmem_per_ccu_depth_cache_size=65536,.sysmem_per_ccu_color_cache_size=65536,.gmem_ccu_color_cache_fraction=2,.gmem_ccu_depth_cache_fraction=2,.prim_alloc_threshold=7,.vs_max_inputs_count=32,.max_sets=5,.line_width_min=1.0,.line_width_max=1.0,.mov_half_shared_quirk=True,.max_draw_states=32,.max_texel_buffer_range_elements=134217728,.max_storage_buffer_range_bytes=134217728,.reg_size_vec4=64,.instr_cache_size=127,.supports_multiview_mask=True,.has_z24uint_s8uint=True,.tess_use_shared=True,.storage_16bit=True,.has_tex_filter_cubic=True,.has_separate_chroma_filter=True,.has_sample_locations=True,.has_lpac=True,.has_legacy_pipeline_shading_rate=True,.has_getfiberid=True,.has_movs=True,.has_dp2acc=True,.has_dp4acc=True,.enable_lrz_fast_clear=True,.has_lrz_dir_tracking=True,.has_lrz_feedback=True,.has_per_view_viewport=True,.has_scalar_alu=True,.has_isam_v=True,.has_ssbo_imm_offsets=True,.has_ubwc_linear_mipmap_fallback=True,.prede_nop_quirk=True,.predtf_nop_quirk=True,.has_sad=True,.has_sel_b_fneg=True,.has_pred_bit=True,.has_pc_dgen_so_cntl=True,.round_robin_errata=True,},.magic={.RB_DBG_ECO_CNTL=68157440,.RB_DBG_ECO_CNTL_blit=68157440,.RB_RBP_CNTL=0,},.magic_raw={{38916, 31}, {38917, 1}, {41208, 1}, {46592, 83918848}, {34304, 0}, {44547, 5120}, {44544, 6}, {48644, 0}, {38400, 33554432}, {3602, 1}},};
static const struct fd_dev_info __info16 = {.chip=6,.gmem_align_w=16,.gmem_align_h=4,.tile_align_w=96,.tile_align_h=16,.tile_max_w=1024,.tile_max_h=1024,.num_vsc_pipes=32,.cs_shared_mem_size=32768,.num_sp_cores=3,.wave_granularity=2,.fibers_per_sp=4096,.threadsize_base=64,.max_waves=16,.highest_bank_bit=16,.ubwc_swizzle=6,.macrotile_mode=1,.num_ccu=3,.num_slices=0,.props={.has_cp_reg_write=False,.has_8bpp_ubwc=False,.has_gmem_fast_clear=True,.has_hw_multiview=True,.has_fs_tex_prefetch=True,.has_sampler_minmax=True,.has_astc_hdr=True,.num_xfb_streams=4,.supports_double_threadsize=True,.sysmem_per_ccu_depth_cache_size=65536,.sysmem_per_ccu_color_cache_size=65536,.gmem_ccu_color_cache_fraction=2,.gmem_ccu_depth_cache_fraction=2,.prim_alloc_threshold=7,.vs_max_inputs_count=32,.max_sets=5,.line_width_min=1.0,.line_width_max=1.0,.mov_half_shared_quirk=True,.max_draw_states=32,.max_texel_buffer_range_elements=134217728,.max_storage_buffer_range_bytes=134217728,.reg_size_vec4=64,.instr_cache_size=127,.supports_multiview_mask=True,.has_z24uint_s8uint=True,.tess_use_shared=True,.storage_16bit=True,.has_tex_filter_cubic=True,.has_separate_chroma_filter=True,.has_sample_locations=True,.has_lpac=True,.has_legacy_pipeline_shading_rate=True,.has_getfiberid=True,.has_movs=True,.has_dp2acc=True,.has_dp4acc=True,.enable_lrz_fast_clear=True,.has_lrz_dir_tracking=True,.has_lrz_feedback=True,.has_per_view_viewport=True,.has_scalar_alu=True,.has_isam_v=True,.has_ssbo_imm_offsets=True,.has_ubwc_linear_mipmap_fallback=True,.prede_nop_quirk=True,.predtf_nop_quirk=True,.has_sad=True,.has_sel_b_fneg=True,.has_pred_bit=True,.has_pc_dgen_so_cntl=True,.round_robin_errata=True,},.magic={.RB_DBG_ECO_CNTL=68157440,.RB_DBG_ECO_CNTL_blit=68157440,.RB_RBP_CNTL=0,},.magic_raw={{38916, 31}, {38917, 2}, {41208, 2}, {46592, 83918848}, {34304, 0}, {44547, 5120}, {44544, 16777216}, {48644, 0}, {38400, 33554432}, {3602, 1}},};
static const struct fd_dev_info __info17 = {.chip=6,.gmem_align_w=16,.gmem_align_h=4,.tile_align_w=96,.tile_align_h=16,.tile_max_w=1024,.tile_max_h=1024,.num_vsc_pipes=32,.cs_shared_mem_size=32768,.num_sp_cores=3,.wave_granularity=2,.fibers_per_sp=8192,.threadsize_base=64,.max_waves=16,.highest_bank_bit=15,.ubwc_swizzle=6,.macrotile_mode=1,.num_ccu=3,.num_slices=0,.props={.has_cp_reg_write=False,.has_8bpp_ubwc=False,.has_gmem_fast_clear=True,.has_hw_multiview=True,.has_fs_tex_prefetch=True,.has_sampler_minmax=True,.has_astc_hdr=True,.num_xfb_streams=4,.supports_double_threadsize=True,.sysmem_per_ccu_depth_cache_size=65536,.sysmem_per_ccu_color_cache_size=65536,.gmem_ccu_color_cache_fraction=2,.gmem_ccu_depth_cache_fraction=2,.prim_alloc_threshold=7,.vs_max_inputs_count=32,.max_sets=5,.line_width_min=1.0,.line_width_max=1.0,.mov_half_shared_quirk=True,.max_draw_states=32,.max_texel_buffer_range_elements=134217728,.max_storage_buffer_range_bytes=134217728,.reg_size_vec4=64,.instr_cache_size=127,.supports_multiview_mask=True,.has_z24uint_s8uint=True,.tess_use_shared=True,.storage_16bit=True,.has_tex_filter_cubic=True,.has_separate_chroma_filter=True,.has_sample_locations=True,.has_lpac=True,.has_legacy_pipeline_shading_rate=True,.has_getfiberid=True,.has_movs=True,.has_dp2acc=True,.has_dp4acc=True,.enable_lrz_fast_clear=True,.has_lrz_dir_tracking=True,.has_lrz_feedback=True,.has_per_view_viewport=True,.has_scalar_alu=True,.has_isam_v=True,.has_ssbo_imm_offsets=True,.has_ubwc_linear_mipmap_fallback=True,.prede_nop_quirk=True,.predtf_nop_quirk=True,.has_sad=True,.has_sel_b_fneg=True,.has_pred_bit=True,.has_pc_dgen_so_cntl=True,.round_robin_errata=True,},.magic={.RB_DBG_ECO_CNTL=68157440,.RB_DBG_ECO_CNTL_blit=68157440,.RB_RBP_CNTL=0,},.magic_raw={{38916, 31}, {38917, 2}, {41208, 2}, {46592, 83918848}, {34304, 0}, {44547, 5120}, {44544, 6}, {48644, 0}, {38400, 33554432}, {3602, 1}},};
static const struct fd_dev_info __info18 = {.chip=6,.gmem_align_w=16,.gmem_align_h=4,.tile_align_w=64,.tile_align_h=32,.tile_max_w=1024,.tile_max_h=1024,.num_vsc_pipes=32,.cs_shared_mem_size=32768,.num_sp_cores=8,.wave_granularity=2,.fibers_per_sp=4096,.threadsize_base=64,.max_waves=16,.highest_bank_bit=16,.ubwc_swizzle=6,.macrotile_mode=1,.num_ccu=8,.num_slices=0,.props={.has_cp_reg_write=False,.has_8bpp_ubwc=False,.has_gmem_fast_clear=True,.has_hw_multiview=True,.has_fs_tex_prefetch=True,.has_sampler_minmax=True,.has_astc_hdr=True,.num_xfb_streams=4,.supports_double_threadsize=True,.sysmem_per_ccu_depth_cache_size=65536,.sysmem_per_ccu_color_cache_size=65536,.gmem_ccu_color_cache_fraction=2,.gmem_ccu_depth_cache_fraction=2,.prim_alloc_threshold=7,.vs_max_inputs_count=32,.max_sets=5,.line_width_min=1.0,.line_width_max=1.0,.mov_half_shared_quirk=True,.max_draw_states=32,.max_texel_buffer_range_elements=134217728,.max_storage_buffer_range_bytes=134217728,.reg_size_vec4=64,.instr_cache_size=127,.supports_multiview_mask=True,.has_z24uint_s8uint=True,.tess_use_shared=True,.storage_16bit=True,.has_tex_filter_cubic=True,.has_separate_chroma_filter=True,.has_sample_locations=True,.has_lpac=True,.has_legacy_pipeline_shading_rate=True,.has_getfiberid=True,.has_movs=True,.has_dp2acc=True,.has_dp4acc=True,.enable_lrz_fast_clear=True,.has_lrz_dir_tracking=True,.has_lrz_feedback=True,.has_per_view_viewport=True,.has_scalar_alu=True,.has_isam_v=True,.has_ssbo_imm_offsets=True,.has_ubwc_linear_mipmap_fallback=True,.prede_nop_quirk=True,.predtf_nop_quirk=True,.has_sad=True,.has_sel_b_fneg=True,.has_pred_bit=True,.has_pc_dgen_so_cntl=True,.round_robin_errata=True,.broken_ds_ubwc_quirk=True,},.magic={.RB_DBG_ECO_CNTL=1048576,.RB_DBG_ECO_CNTL_blit=1048576,.RB_RBP_CNTL=0,},.magic_raw={{43762, 12582912}, {38916, 31}, {38917, 7}, {41208, 7}, {46592, 79691776}, {34304, 0}, {44547, 5120}, {44544, 18874368}, {48644, 0}, {38400, 33555456}, {3602, 1}},};
static const struct fd_dev_info __info19 = {.chip=6,.gmem_align_w=16,.gmem_align_h=4,.tile_align_w=32,.tile_align_h=16,.tile_max_w=1024,.tile_max_h=1024,.num_vsc_pipes=16,.cs_shared_mem_size=16384,.num_sp_cores=1,.wave_granularity=1,.fibers_per_sp=2048,.threadsize_base=16,.max_waves=16,.highest_bank_bit=15,.ubwc_swizzle=6,.macrotile_mode=1,.num_ccu=1,.num_slices=0,.props={.has_cp_reg_write=False,.has_8bpp_ubwc=True,.has_gmem_fast_clear=True,.has_hw_multiview=False,.has_fs_tex_prefetch=False,.has_sampler_minmax=False,.has_astc_hdr=False,.num_xfb_streams=1,.supports_double_threadsize=True,.sysmem_per_ccu_depth_cache_size=8192,.sysmem_per_ccu_color_cache_size=8192,.gmem_ccu_color_cache_fraction=1,.gmem_ccu_depth_cache_fraction=2,.prim_alloc_threshold=1,.vs_max_inputs_count=16,.max_sets=5,.line_width_min=1.0,.line_width_max=1.0,.mov_half_shared_quirk=True,.max_draw_states=32,.max_texel_buffer_range_elements=134217728,.max_storage_buffer_range_bytes=134217728,.reg_size_vec4=64,.instr_cache_size=64,.indirect_draw_wfm_quirk=True,.depth_bounds_require_depth_test_quirk=True,.storage_16bit=True,.is_a702=True,},.magic={.RB_DBG_ECO_CNTL=1048576,.RB_DBG_ECO_CNTL_blit=1048576,.RB_RBP_CNTL=1,},.magic_raw={{38916, 15}, {38917, 0}, {41208, 0}, {46592, 32768}, {34304, 0}, {44547, 5120}, {44544, 0}, {48644, 33554432}, {38400, 0}, {3602, 1}},};
static const struct fd_dev_info __info20 = {.chip=7,.gmem_align_w=16,.gmem_align_h=4,.tile_align_w=64,.tile_align_h=16,.tile_max_w=1024,.tile_max_h=1024,.num_vsc_pipes=32,.cs_shared_mem_size=32768,.num_sp_cores=1,.wave_granularity=2,.fibers_per_sp=4096,.threadsize_base=64,.max_waves=16,.highest_bank_bit=15,.ubwc_swizzle=6,.macrotile_mode=1,.num_ccu=1,.num_slices=0,.props={.has_gmem_fast_clear=True,.has_hw_multiview=True,.has_fs_tex_prefetch=True,.has_sampler_minmax=True,.has_astc_hdr=True,.num_xfb_streams=4,.supports_double_threadsize=True,.sysmem_per_ccu_depth_cache_size=262144,.sysmem_per_ccu_color_cache_size=65536,.gmem_ccu_color_cache_fraction=3,.gmem_ccu_depth_cache_fraction=3,.prim_alloc_threshold=7,.vs_max_inputs_count=32,.max_sets=8,.reg_size_vec4=96,.instr_cache_size=127,.supports_multiview_mask=True,.has_z24uint_s8uint=True,.tess_use_shared=True,.storage_16bit=True,.storage_8bit=True,.has_tex_filter_cubic=True,.has_separate_chroma_filter=True,.has_sample_locations=True,.has_lpac=True,.has_getfiberid=True,.has_movs=True,.has_dp2acc=True,.has_dp4acc=True,.enable_lrz_fast_clear=True,.has_lrz_dir_tracking=True,.has_lrz_feedback=True,.has_per_view_viewport=True,.line_width_min=1.0,.line_width_max=127.5,.has_scalar_alu=True,.has_scalar_predicates=True,.has_coherent_ubwc_flag_caches=True,.has_isam_v=True,.has_ssbo_imm_offsets=True,.has_early_preamble=True,.has_attachment_shading_rate=True,.has_ubwc_linear_mipmap_fallback=True,.supports_linear_mipmap_threshold_in_blocks=True,.prede_nop_quirk=True,.predtf_nop_quirk=True,.has_sad=True,.has_bin_mask=True,.has_sel_b_fneg=True,.has_pred_bit=True,.has_pc_dgen_so_cntl=True,.has_eolm_eogm=True,.round_robin_errata=True,.max_texel_buffer_range_elements=134217728,.max_storage_buffer_range_bytes=134217728,.alias_mova_quirk=True,.alias_predication_quirk=True,.supports_uav_ubwc=True,.fs_must_have_non_zero_constlen_quirk=True,.enable_tp_ubwc_flag_hint=True,.reading_shading_rate_requires_smask_quirk=True,.cs_lock_unlock_quirk=True,},.magic={.RB_DBG_ECO_CNTL=0,.RB_DBG_ECO_CNTL_blit=0,.RB_RBP_CNTL=0,},.magic_raw={{3607, 8650756}, {46592, 16777216}, {46594, 263972}, {44547, 5120}, {44552, 4203520}, {44553, 0}, {44554, 0}, {3600, 0}, {3601, 64}, {44652, 32768}, {44544, 268435456}, {38916, 7967}, {40448, 537395200}, {40484, 570195712}, {42496, 0}, {44550, 0}, {44650, 0}, {44651, 128}, {44659, 0}, {43778, 0}, {43777, 0}, {43810, 0}, {45840, 0}, {3298, 0}, {3299, 0}, {3300, 0}, {3301, 0}, {3302, 0}, {3303, 0}, {32935, 0}, {34304, 2048}, {36473, 0}, {34969, 0}, {36358, 34078720}, {38400, 33554432}, {3602, 52428800}},};
static const struct fd_dev_info __info21 = {.chip=7,.gmem_align_w=16,.gmem_align_h=4,.tile_align_w=64,.tile_align_h=32,.tile_max_w=1024,.tile_max_h=1024,.num_vsc_pipes=32,.cs_shared_mem_size=32768,.num_sp_cores=4,.wave_granularity=2,.fibers_per_sp=4096,.threadsize_base=64,.max_waves=16,.highest_bank_bit=16,.ubwc_swizzle=6,.macrotile_mode=1,.num_ccu=4,.num_slices=0,.props={.has_gmem_fast_clear=True,.has_hw_multiview=True,.has_fs_tex_prefetch=True,.has_sampler_minmax=True,.has_astc_hdr=True,.num_xfb_streams=4,.supports_double_threadsize=True,.sysmem_per_ccu_depth_cache_size=262144,.sysmem_per_ccu_color_cache_size=65536,.gmem_ccu_color_cache_fraction=3,.gmem_ccu_depth_cache_fraction=3,.prim_alloc_threshold=7,.vs_max_inputs_count=32,.max_sets=8,.reg_size_vec4=96,.instr_cache_size=127,.supports_multiview_mask=True,.has_z24uint_s8uint=True,.tess_use_shared=True,.storage_16bit=True,.storage_8bit=True,.has_tex_filter_cubic=True,.has_separate_chroma_filter=True,.has_sample_locations=True,.has_lpac=True,.has_getfiberid=True,.has_movs=True,.has_dp2acc=True,.has_dp4acc=True,.enable_lrz_fast_clear=True,.has_lrz_dir_tracking=True,.has_lrz_feedback=True,.has_per_view_viewport=True,.line_width_min=1.0,.line_width_max=127.5,.has_scalar_alu=True,.has_scalar_predicates=True,.has_coherent_ubwc_flag_caches=True,.has_isam_v=True,.has_ssbo_imm_offsets=True,.has_early_preamble=True,.has_attachment_shading_rate=True,.has_ubwc_linear_mipmap_fallback=True,.supports_linear_mipmap_threshold_in_blocks=True,.prede_nop_quirk=True,.predtf_nop_quirk=True,.has_sad=True,.has_bin_mask=True,.has_sel_b_fneg=True,.has_pred_bit=True,.has_pc_dgen_so_cntl=True,.has_eolm_eogm=True,.round_robin_errata=True,.max_texel_buffer_range_elements=134217728,.max_storage_buffer_range_bytes=134217728,.alias_mova_quirk=True,.alias_predication_quirk=True,.supports_uav_ubwc=True,.fs_must_have_non_zero_constlen_quirk=True,.enable_tp_ubwc_flag_hint=True,.reading_shading_rate_requires_smask_quirk=True,.cs_lock_unlock_quirk=True,.cmdbuf_start_a725_quirk=True,},.magic={.RB_DBG_ECO_CNTL=0,.RB_DBG_ECO_CNTL_blit=0,.RB_RBP_CNTL=0,},.magic_raw={{3607, 8650756}, {46592, 16777216}, {46594, 263972}, {44547, 5120}, {44552, 4203520}, {44553, 0}, {44554, 0}, {3600, 0}, {3601, 64}, {44652, 32768}, {44544, 268435456}, {38916, 7967}, {40448, 537395200}, {40484, 570195712}, {42496, 0}, {44550, 0}, {44650, 0}, {44651, 128}, {44659, 0}, {43778, 0}, {43777, 0}, {43810, 0}, {45840, 0}, {3298, 0}, {3299, 0}, {3300, 0}, {3301, 0}, {3302, 0}, {3303, 0}, {32935, 0}, {34304, 2048}, {36473, 0}, {34969, 0}, {36358, 34078720}, {38400, 33554432}, {3602, 52428800}},};
static const struct fd_dev_info __info22 = {.chip=7,.gmem_align_w=16,.gmem_align_h=4,.tile_align_w=64,.tile_align_h=32,.tile_max_w=1024,.tile_max_h=1024,.num_vsc_pipes=32,.cs_shared_mem_size=32768,.num_sp_cores=4,.wave_granularity=2,.fibers_per_sp=4096,.threadsize_base=64,.max_waves=16,.highest_bank_bit=16,.ubwc_swizzle=6,.macrotile_mode=1,.num_ccu=4,.num_slices=0,.props={.has_gmem_fast_clear=True,.has_hw_multiview=True,.has_fs_tex_prefetch=True,.has_sampler_minmax=True,.has_astc_hdr=True,.num_xfb_streams=4,.supports_double_threadsize=True,.sysmem_per_ccu_depth_cache_size=262144,.sysmem_per_ccu_color_cache_size=65536,.gmem_ccu_color_cache_fraction=3,.gmem_ccu_depth_cache_fraction=3,.prim_alloc_threshold=7,.vs_max_inputs_count=32,.max_sets=8,.reg_size_vec4=96,.instr_cache_size=127,.supports_multiview_mask=True,.has_z24uint_s8uint=True,.tess_use_shared=True,.storage_16bit=True,.storage_8bit=True,.has_tex_filter_cubic=True,.has_separate_chroma_filter=True,.has_sample_locations=True,.has_lpac=True,.has_getfiberid=True,.has_movs=True,.has_dp2acc=True,.has_dp4acc=True,.enable_lrz_fast_clear=True,.has_lrz_dir_tracking=True,.has_lrz_feedback=True,.has_per_view_viewport=True,.line_width_min=1.0,.line_width_max=127.5,.has_scalar_alu=True,.has_scalar_predicates=True,.has_coherent_ubwc_flag_caches=True,.has_isam_v=True,.has_ssbo_imm_offsets=True,.has_early_preamble=True,.has_attachment_shading_rate=True,.has_ubwc_linear_mipmap_fallback=True,.supports_linear_mipmap_threshold_in_blocks=True,.prede_nop_quirk=True,.predtf_nop_quirk=True,.has_sad=True,.has_bin_mask=True,.has_sel_b_fneg=True,.has_pred_bit=True,.has_pc_dgen_so_cntl=True,.has_eolm_eogm=True,.round_robin_errata=True,.max_texel_buffer_range_elements=134217728,.max_storage_buffer_range_bytes=134217728,.alias_mova_quirk=True,.alias_predication_quirk=True,.supports_uav_ubwc=True,.fs_must_have_non_zero_constlen_quirk=True,.enable_tp_ubwc_flag_hint=True,.reading_shading_rate_requires_smask_quirk=True,.cs_lock_unlock_quirk=True,},.magic={.RB_DBG_ECO_CNTL=0,.RB_DBG_ECO_CNTL_blit=0,.RB_RBP_CNTL=0,},.magic_raw={{3607, 8650756}, {46592, 16777216}, {46594, 263972}, {44547, 5120}, {44552, 4203520}, {44553, 0}, {44554, 0}, {3600, 0}, {3601, 64}, {44652, 32768}, {44544, 268435456}, {38916, 7967}, {40448, 537395200}, {40484, 570195712}, {42496, 0}, {44550, 0}, {44650, 0}, {44651, 128}, {44659, 0}, {43778, 0}, {43777, 0}, {43810, 0}, {45840, 0}, {3298, 0}, {3299, 0}, {3300, 0}, {3301, 0}, {3302, 0}, {3303, 0}, {32935, 0}, {34304, 2048}, {36473, 0}, {34969, 0}, {36358, 34078720}, {38400, 33554432}, {3602, 52428800}},};
static const struct fd_dev_info __info23 = {.chip=7,.gmem_align_w=16,.gmem_align_h=4,.tile_align_w=96,.tile_align_h=32,.tile_max_w=2016,.tile_max_h=2032,.num_vsc_pipes=32,.cs_shared_mem_size=32768,.num_sp_cores=3,.wave_granularity=2,.fibers_per_sp=4096,.threadsize_base=64,.max_waves=16,.highest_bank_bit=15,.ubwc_swizzle=6,.macrotile_mode=1,.num_ccu=3,.num_slices=0,.props={.has_gmem_fast_clear=True,.has_hw_multiview=True,.has_fs_tex_prefetch=True,.has_sampler_minmax=True,.has_astc_hdr=True,.num_xfb_streams=4,.supports_double_threadsize=True,.sysmem_per_ccu_depth_cache_size=262144,.sysmem_per_ccu_color_cache_size=65536,.gmem_ccu_color_cache_fraction=3,.gmem_ccu_depth_cache_fraction=3,.prim_alloc_threshold=7,.vs_max_inputs_count=32,.max_sets=8,.reg_size_vec4=96,.instr_cache_size=127,.supports_multiview_mask=True,.has_z24uint_s8uint=True,.tess_use_shared=True,.storage_16bit=True,.storage_8bit=True,.has_tex_filter_cubic=True,.has_separate_chroma_filter=True,.has_sample_locations=True,.has_lpac=True,.has_getfiberid=True,.has_movs=True,.has_dp2acc=True,.has_dp4acc=True,.enable_lrz_fast_clear=True,.has_lrz_dir_tracking=True,.has_lrz_feedback=True,.has_per_view_viewport=True,.line_width_min=1.0,.line_width_max=127.5,.has_scalar_alu=True,.has_scalar_predicates=True,.has_coherent_ubwc_flag_caches=True,.has_isam_v=True,.has_ssbo_imm_offsets=True,.has_early_preamble=True,.has_attachment_shading_rate=True,.has_ubwc_linear_mipmap_fallback=True,.supports_linear_mipmap_threshold_in_blocks=True,.prede_nop_quirk=True,.predtf_nop_quirk=True,.has_sad=True,.has_bin_mask=True,.has_sel_b_fneg=True,.has_pred_bit=True,.has_pc_dgen_so_cntl=True,.has_eolm_eogm=True,.round_robin_errata=True,.max_texel_buffer_range_elements=134217728,.max_storage_buffer_range_bytes=134217728,.alias_mova_quirk=True,.alias_predication_quirk=True,.stsc_duplication_quirk=True,.has_event_write_sample_count=True,.ubwc_unorm_snorm_int_compatible=True,.supports_uav_ubwc=True,.fs_must_have_non_zero_constlen_quirk=True,.enable_tp_ubwc_flag_hint=True,.has_64b_ssbo_atomics=True,.has_primitive_shading_rate=True,.reading_shading_rate_requires_smask_quirk=True,.has_ray_intersection=True,.has_hw_bin_scaling=True,.has_image_processing=True,.has_64b_image_atomics=True,.has_implicit_fragface_fragcoord_ij_linear=True,},.magic={.RB_DBG_ECO_CNTL=1,.RB_DBG_ECO_CNTL_blit=1,.RB_RBP_CNTL=0,},.magic_raw={{3607, 0}, {46592, 286261248}, {46594, 263972}, {44547, 268440576}, {44552, 4195328}, {44553, 4392960}, {44554, 0}, {3600, 0}, {3601, 0}, {44652, 0}, {44544, 268435456}, {38916, 7967}, {40448, 1048576}, {40484, 22566400}, {42496, 32768}, {44550, 0}, {44650, 0}, {44651, 128}, {44659, 0}, {43778, 0}, {43777, 0}, {43810, 0}, {45840, 0}, {3298, 0}, {3299, 0}, {3300, 0}, {3301, 0}, {3302, 0}, {3303, 0}, {32935, 0}, {34304, 18432}, {36473, 0}, {34969, 0}, {36358, 34078720}, {38400, 33554432}, {3602, 0}},};
static const struct fd_dev_info __info24 = {.chip=7,.gmem_align_w=16,.gmem_align_h=4,.tile_align_w=96,.tile_align_h=32,.tile_max_w=2016,.tile_max_h=2032,.num_vsc_pipes=32,.cs_shared_mem_size=32768,.num_sp_cores=6,.wave_granularity=2,.fibers_per_sp=4096,.threadsize_base=64,.max_waves=16,.highest_bank_bit=16,.ubwc_swizzle=6,.macrotile_mode=1,.num_ccu=6,.num_slices=0,.props={.has_gmem_fast_clear=True,.has_hw_multiview=True,.has_fs_tex_prefetch=True,.has_sampler_minmax=True,.has_astc_hdr=True,.num_xfb_streams=4,.supports_double_threadsize=True,.sysmem_per_ccu_depth_cache_size=262144,.sysmem_per_ccu_color_cache_size=65536,.gmem_ccu_color_cache_fraction=3,.gmem_ccu_depth_cache_fraction=3,.prim_alloc_threshold=7,.vs_max_inputs_count=32,.max_sets=8,.reg_size_vec4=96,.instr_cache_size=127,.supports_multiview_mask=True,.has_z24uint_s8uint=True,.tess_use_shared=True,.storage_16bit=True,.storage_8bit=True,.has_tex_filter_cubic=True,.has_separate_chroma_filter=True,.has_sample_locations=True,.has_lpac=True,.has_getfiberid=True,.has_movs=True,.has_dp2acc=True,.has_dp4acc=True,.enable_lrz_fast_clear=True,.has_lrz_dir_tracking=True,.has_lrz_feedback=True,.has_per_view_viewport=True,.line_width_min=1.0,.line_width_max=127.5,.has_scalar_alu=True,.has_scalar_predicates=True,.has_coherent_ubwc_flag_caches=True,.has_isam_v=True,.has_ssbo_imm_offsets=True,.has_early_preamble=True,.has_attachment_shading_rate=True,.has_ubwc_linear_mipmap_fallback=True,.supports_linear_mipmap_threshold_in_blocks=True,.prede_nop_quirk=True,.predtf_nop_quirk=True,.has_sad=True,.has_bin_mask=True,.has_sel_b_fneg=True,.has_pred_bit=True,.has_pc_dgen_so_cntl=True,.has_eolm_eogm=True,.round_robin_errata=True,.max_texel_buffer_range_elements=134217728,.max_storage_buffer_range_bytes=134217728,.alias_mova_quirk=True,.alias_predication_quirk=True,.stsc_duplication_quirk=True,.has_event_write_sample_count=True,.ubwc_unorm_snorm_int_compatible=True,.supports_uav_ubwc=True,.fs_must_have_non_zero_constlen_quirk=True,.enable_tp_ubwc_flag_hint=False,.has_64b_ssbo_atomics=True,.has_primitive_shading_rate=True,.reading_shading_rate_requires_smask_quirk=True,.has_ray_intersection=True,.has_hw_bin_scaling=True,.has_image_processing=True,.has_64b_image_atomics=True,.has_implicit_fragface_fragcoord_ij_linear=True,},.magic={.RB_DBG_ECO_CNTL=0,.RB_DBG_ECO_CNTL_blit=0,.RB_RBP_CNTL=0,},.magic_raw={{3607, 262148}, {46592, 286261248}, {46594, 263972}, {44547, 268440576}, {44552, 4195328}, {44553, 4392960}, {44554, 0}, {3600, 0}, {3601, 0}, {44652, 0}, {44544, 268435456}, {38916, 7967}, {40448, 1048576}, {40484, 559437312}, {42496, 32768}, {44550, 0}, {44650, 0}, {44651, 128}, {44659, 0}, {43778, 0}, {43777, 0}, {43810, 0}, {45840, 0}, {3298, 0}, {3299, 0}, {3300, 0}, {3301, 0}, {3302, 0}, {3303, 0}, {32935, 0}, {34304, 18432}, {36473, 0}, {34969, 0}, {36358, 34078720}, {38400, 33554432}, {3602, 0}},};
static const struct fd_dev_info __info25 = {.chip=7,.gmem_align_w=16,.gmem_align_h=4,.tile_align_w=96,.tile_align_h=32,.tile_max_w=2016,.tile_max_h=2032,.num_vsc_pipes=32,.cs_shared_mem_size=32768,.num_sp_cores=6,.wave_granularity=2,.fibers_per_sp=4096,.threadsize_base=64,.max_waves=16,.highest_bank_bit=15,.ubwc_swizzle=6,.macrotile_mode=1,.num_ccu=6,.num_slices=0,.props={.has_gmem_fast_clear=True,.has_hw_multiview=True,.has_fs_tex_prefetch=True,.has_sampler_minmax=True,.has_astc_hdr=True,.num_xfb_streams=4,.supports_double_threadsize=True,.sysmem_per_ccu_depth_cache_size=262144,.sysmem_per_ccu_color_cache_size=65536,.gmem_ccu_color_cache_fraction=3,.gmem_ccu_depth_cache_fraction=3,.prim_alloc_threshold=7,.vs_max_inputs_count=32,.max_sets=8,.reg_size_vec4=96,.instr_cache_size=127,.supports_multiview_mask=True,.has_z24uint_s8uint=True,.tess_use_shared=True,.storage_16bit=True,.storage_8bit=True,.has_tex_filter_cubic=True,.has_separate_chroma_filter=True,.has_sample_locations=True,.has_lpac=True,.has_getfiberid=True,.has_movs=True,.has_dp2acc=True,.has_dp4acc=True,.enable_lrz_fast_clear=True,.has_lrz_dir_tracking=True,.has_lrz_feedback=True,.has_per_view_viewport=True,.line_width_min=1.0,.line_width_max=127.5,.has_scalar_alu=True,.has_scalar_predicates=True,.has_coherent_ubwc_flag_caches=True,.has_isam_v=True,.has_ssbo_imm_offsets=True,.has_early_preamble=True,.has_attachment_shading_rate=True,.has_ubwc_linear_mipmap_fallback=True,.supports_linear_mipmap_threshold_in_blocks=True,.prede_nop_quirk=True,.predtf_nop_quirk=True,.has_sad=True,.has_bin_mask=True,.has_sel_b_fneg=True,.has_pred_bit=True,.has_pc_dgen_so_cntl=True,.has_eolm_eogm=True,.round_robin_errata=True,.max_texel_buffer_range_elements=134217728,.max_storage_buffer_range_bytes=134217728,.alias_mova_quirk=True,.alias_predication_quirk=True,.stsc_duplication_quirk=True,.has_event_write_sample_count=True,.ubwc_unorm_snorm_int_compatible=True,.supports_uav_ubwc=True,.fs_must_have_non_zero_constlen_quirk=True,.enable_tp_ubwc_flag_hint=False,.has_64b_ssbo_atomics=True,.has_primitive_shading_rate=True,.reading_shading_rate_requires_smask_quirk=True,.has_ray_intersection=True,.has_hw_bin_scaling=True,.has_image_processing=True,.has_64b_image_atomics=True,.has_implicit_fragface_fragcoord_ij_linear=True,.cmdbuf_start_a725_quirk=True,},.magic={.RB_DBG_ECO_CNTL=0,.RB_DBG_ECO_CNTL_blit=0,.RB_RBP_CNTL=0,},.magic_raw={{3607, 262148}, {46592, 286261248}, {46594, 1792}, {44547, 268440576}, {44552, 4195328}, {44553, 4392992}, {44554, 0}, {3600, 0}, {3601, 128}, {44652, 0}, {44544, 268435456}, {38916, 7967}, {40448, 1048576}, {40484, 559437312}, {42496, 32768}, {44550, 0}, {44650, 0}, {44651, 128}, {44659, 0}, {43778, 0}, {43777, 0}, {43810, 0}, {45840, 0}, {3298, 0}, {3299, 0}, {3300, 0}, {3301, 0}, {3302, 0}, {3303, 0}, {32935, 0}, {34304, 18432}, {36473, 0}, {34969, 0}, {36358, 34078720}, {38400, 33554432}, {3602, 0}},};
static const struct fd_dev_info __info26 = {.chip=7,.gmem_align_w=16,.gmem_align_h=4,.tile_align_w=96,.tile_align_h=32,.tile_max_w=2016,.tile_max_h=2032,.num_vsc_pipes=32,.cs_shared_mem_size=32768,.num_sp_cores=6,.wave_granularity=2,.fibers_per_sp=4096,.threadsize_base=64,.max_waves=16,.highest_bank_bit=15,.ubwc_swizzle=6,.macrotile_mode=1,.num_ccu=6,.num_slices=0,.props={.has_gmem_fast_clear=True,.has_hw_multiview=True,.has_fs_tex_prefetch=True,.has_sampler_minmax=True,.has_astc_hdr=True,.num_xfb_streams=4,.supports_double_threadsize=True,.sysmem_per_ccu_depth_cache_size=262144,.sysmem_per_ccu_color_cache_size=65536,.gmem_ccu_color_cache_fraction=3,.gmem_ccu_depth_cache_fraction=3,.prim_alloc_threshold=7,.vs_max_inputs_count=32,.max_sets=8,.reg_size_vec4=96,.instr_cache_size=127,.supports_multiview_mask=True,.has_z24uint_s8uint=True,.tess_use_shared=True,.storage_16bit=True,.storage_8bit=True,.has_tex_filter_cubic=True,.has_separate_chroma_filter=True,.has_sample_locations=True,.has_lpac=True,.has_getfiberid=True,.has_movs=True,.has_dp2acc=True,.has_dp4acc=True,.enable_lrz_fast_clear=True,.has_lrz_dir_tracking=True,.has_lrz_feedback=True,.has_per_view_viewport=True,.line_width_min=1.0,.line_width_max=127.5,.has_scalar_alu=True,.has_scalar_predicates=True,.has_coherent_ubwc_flag_caches=True,.has_isam_v=True,.has_ssbo_imm_offsets=True,.has_early_preamble=True,.has_attachment_shading_rate=True,.has_ubwc_linear_mipmap_fallback=True,.supports_linear_mipmap_threshold_in_blocks=True,.prede_nop_quirk=True,.predtf_nop_quirk=True,.has_sad=True,.has_bin_mask=True,.has_sel_b_fneg=True,.has_pred_bit=True,.has_pc_dgen_so_cntl=True,.has_eolm_eogm=True,.round_robin_errata=True,.max_texel_buffer_range_elements=134217728,.max_storage_buffer_range_bytes=134217728,.alias_mova_quirk=True,.alias_predication_quirk=True,.stsc_duplication_quirk=True,.has_event_write_sample_count=True,.ubwc_unorm_snorm_int_compatible=True,.supports_uav_ubwc=True,.fs_must_have_non_zero_constlen_quirk=True,.enable_tp_ubwc_flag_hint=True,.has_64b_ssbo_atomics=True,.has_primitive_shading_rate=True,.reading_shading_rate_requires_smask_quirk=True,.has_ray_intersection=True,.has_hw_bin_scaling=True,.has_image_processing=True,.has_64b_image_atomics=True,.has_implicit_fragface_fragcoord_ij_linear=True,},.magic={.RB_DBG_ECO_CNTL=1,.RB_DBG_ECO_CNTL_blit=0,.RB_RBP_CNTL=0,},.magic_raw={{3607, 262148}, {46592, 286261248}, {46594, 263972}, {44547, 268440576}, {44552, 4195328}, {44553, 4392960}, {44554, 0}, {3600, 0}, {3601, 0}, {44652, 0}, {44544, 268435456}, {38916, 7967}, {40448, 1048576}, {40484, 559437312}, {42496, 32768}, {44550, 0}, {44650, 0}, {44651, 128}, {44659, 0}, {43778, 0}, {43777, 0}, {43810, 0}, {45840, 0}, {3298, 0}, {3299, 0}, {3300, 0}, {3301, 0}, {3302, 0}, {3303, 0}, {32935, 0}, {34304, 18432}, {36473, 0}, {34969, 0}, {36358, 34078720}, {38400, 33554432}, {3602, 0}},};
static const struct fd_dev_info __info27 = {.chip=7,.gmem_align_w=16,.gmem_align_h=4,.tile_align_w=96,.tile_align_h=32,.tile_max_w=2016,.tile_max_h=2032,.num_vsc_pipes=32,.cs_shared_mem_size=32768,.num_sp_cores=6,.wave_granularity=2,.fibers_per_sp=4096,.threadsize_base=64,.max_waves=16,.highest_bank_bit=16,.ubwc_swizzle=6,.macrotile_mode=1,.num_ccu=6,.num_slices=0,.props={.has_gmem_fast_clear=True,.has_hw_multiview=True,.has_fs_tex_prefetch=True,.has_sampler_minmax=True,.has_astc_hdr=True,.num_xfb_streams=4,.supports_double_threadsize=True,.sysmem_per_ccu_depth_cache_size=262144,.sysmem_per_ccu_color_cache_size=65536,.gmem_ccu_color_cache_fraction=3,.gmem_ccu_depth_cache_fraction=3,.prim_alloc_threshold=7,.vs_max_inputs_count=32,.max_sets=8,.reg_size_vec4=96,.instr_cache_size=127,.supports_multiview_mask=True,.has_z24uint_s8uint=True,.tess_use_shared=True,.storage_16bit=True,.storage_8bit=True,.has_tex_filter_cubic=True,.has_separate_chroma_filter=True,.has_sample_locations=True,.has_lpac=True,.has_getfiberid=True,.has_movs=True,.has_dp2acc=True,.has_dp4acc=True,.enable_lrz_fast_clear=True,.has_lrz_dir_tracking=True,.has_lrz_feedback=True,.has_per_view_viewport=True,.line_width_min=1.0,.line_width_max=127.5,.has_scalar_alu=True,.has_scalar_predicates=True,.has_coherent_ubwc_flag_caches=True,.has_isam_v=True,.has_ssbo_imm_offsets=True,.has_early_preamble=True,.has_attachment_shading_rate=True,.has_ubwc_linear_mipmap_fallback=True,.supports_linear_mipmap_threshold_in_blocks=True,.prede_nop_quirk=True,.predtf_nop_quirk=True,.has_sad=True,.has_bin_mask=True,.has_sel_b_fneg=True,.has_pred_bit=True,.has_pc_dgen_so_cntl=True,.has_eolm_eogm=True,.round_robin_errata=True,.max_texel_buffer_range_elements=134217728,.max_storage_buffer_range_bytes=134217728,.alias_mova_quirk=True,.alias_predication_quirk=True,.has_event_write_sample_count=True,.load_inline_uniforms_via_preamble_ldgk=True,.load_shader_consts_via_preamble=True,.has_gmem_vpc_attr_buf=True,.sysmem_vpc_attr_buf_size=131072,.gmem_vpc_attr_buf_size=49152,.ubwc_unorm_snorm_int_compatible=True,.supports_uav_ubwc=True,.has_generic_clear=True,.r8g8_faulty_fast_clear_quirk=True,.gs_vpc_adjacency_quirk=True,.ubwc_all_formats_compatible=True,.has_compliant_dp4acc=True,.ubwc_coherency_quirk=True,.has_persistent_counter=True,.has_64b_ssbo_atomics=True,.has_primitive_shading_rate=True,.has_ray_intersection=True,.has_sw_fuse=True,.has_rt_workaround=True,.has_alias_rt=True,.has_abs_bin_mask=True,.new_control_regs=True,.has_hw_bin_scaling=True,.has_image_processing=True,.max_draw_states=64,.has_64b_image_atomics=True,.has_implicit_fragface_fragcoord_ij_linear=True,},.magic={.RB_DBG_ECO_CNTL=1,.RB_DBG_ECO_CNTL_blit=1,.RB_RBP_CNTL=0,},.magic_raw={{3607, 0}, {3600, 0}, {3601, 128}, {44547, 268436480}, {44552, 4194304}, {44553, 4397056}, {44554, 8388608}, {44652, 0}, {44544, 268435456}, {38916, 16159}, {40448, 1048576}, {40484, 22566400}, {42496, 32768}, {44550, 0}, {44650, 0}, {44651, 128}, {44659, 0}, {43778, 0}, {43777, 0}, {43810, 0}, {45840, 0}, {3298, 0}, {3299, 0}, {3300, 0}, {3301, 0}, {3302, 0}, {3303, 0}, {32935, 0}, {34304, 18432}, {34969, 0}, {36358, 34086912}, {38410, 1}, {43522, 0}, {43523, 0}, {46592, 286261248}, {38400, 33554432}, {3602, 1073741824}},};
static const struct fd_dev_info __info28 = {.chip=8,.gmem_align_w=16,.gmem_align_h=4,.tile_align_w=32,.tile_align_h=16,.tile_max_w=16384,.tile_max_h=16384,.num_vsc_pipes=32,.cs_shared_mem_size=32768,.num_sp_cores=1,.wave_granularity=2,.fibers_per_sp=4096,.threadsize_base=64,.max_waves=16,.highest_bank_bit=15,.ubwc_swizzle=6,.macrotile_mode=1,.num_ccu=1,.num_slices=1,.props={.has_gmem_fast_clear=True,.has_hw_multiview=True,.has_fs_tex_prefetch=False,.has_sampler_minmax=True,.has_astc_hdr=True,.num_xfb_streams=4,.supports_double_threadsize=False,.sysmem_per_ccu_depth_cache_size=262144,.sysmem_per_ccu_color_cache_size=131072,.gmem_ccu_color_cache_fraction=3,.gmem_ccu_depth_cache_fraction=0,.prim_alloc_threshold=7,.vs_max_inputs_count=32,.max_sets=8,.reg_size_vec4=96,.instr_cache_size=127,.supports_multiview_mask=True,.has_z24uint_s8uint=True,.tess_use_shared=True,.storage_16bit=True,.storage_8bit=True,.has_tex_filter_cubic=True,.has_separate_chroma_filter=True,.has_sample_locations=True,.has_lpac=True,.has_getfiberid=True,.has_movs=True,.has_dp2acc=False,.has_dp4acc=True,.enable_lrz_fast_clear=True,.has_lrz_dir_tracking=True,.has_lrz_feedback=True,.has_per_view_viewport=True,.line_width_min=1.0,.line_width_max=127.5,.has_scalar_alu=True,.has_scalar_predicates=True,.has_coherent_ubwc_flag_caches=True,.has_isam_v=True,.has_ssbo_imm_offsets=True,.has_early_preamble=True,.has_attachment_shading_rate=True,.has_ubwc_linear_mipmap_fallback=True,.supports_linear_mipmap_threshold_in_blocks=True,.prede_nop_quirk=True,.predtf_nop_quirk=True,.has_sad=True,.has_bin_mask=True,.has_sel_b_fneg=True,.has_pred_bit=True,.has_pc_dgen_so_cntl=True,.has_eolm_eogm=True,.round_robin_errata=False,.max_texel_buffer_range_elements=536870911,.max_storage_buffer_range_bytes=2147483647,.alias_mova_quirk=False,.alias_predication_quirk=True,.has_event_write_sample_count=True,.load_inline_uniforms_via_preamble_ldgk=True,.load_shader_consts_via_preamble=True,.has_gmem_vpc_attr_buf=True,.sysmem_vpc_attr_buf_size=131072,.gmem_vpc_attr_buf_size=16384,.ubwc_unorm_snorm_int_compatible=True,.supports_uav_ubwc=True,.has_generic_clear=True,.r8g8_faulty_fast_clear_quirk=True,.gs_vpc_adjacency_quirk=True,.ubwc_all_formats_compatible=True,.has_compliant_dp4acc=True,.ubwc_coherency_quirk=True,.has_persistent_counter=True,.has_64b_ssbo_atomics=True,.has_primitive_shading_rate=True,.has_ray_intersection=True,.has_sw_fuse=True,.has_rt_workaround=False,.has_alias_rt=True,.has_abs_bin_mask=True,.new_control_regs=True,.has_hw_bin_scaling=True,.has_image_processing=True,.max_draw_states=64,.has_64b_image_atomics=True,.has_implicit_fragface_fragcoord_ij_linear=True,.has_dual_wave_dispatch=True,.sysmem_vpc_pos_buf_size=65536,.sysmem_vpc_bv_pos_buf_size=32768,.sysmem_ccu_color_cache_fraction=0,.sysmem_ccu_depth_cache_fraction=0,.gmem_vpc_pos_buf_size=12288,.gmem_vpc_bv_pos_buf_size=20480,.gmem_per_ccu_color_cache_size=16384,.gmem_per_ccu_depth_cache_size=262144,.has_salu_int_narrowing_quirk=True,},.magic={},.magic_raw={{33320, 0}, {33321, 0}, {33322, 0}, {33323, 0}, {33324, 0}, {33325, 0}, {34840, 0}, {34841, 0}, {34842, 0}, {34843, 0}, {34844, 0}, {34845, 0}, {34846, 0}, {34969, 0}, {35057, 1}, {43777, 0}, {43778, 0}, {43811, 0}, {45442, 4}, {45443, 0}, {38923, 8389248}, {39680, 16128}},};
static const struct fd_dev_info __info29 = {.chip=8,.gmem_align_w=16,.gmem_align_h=4,.tile_align_w=96,.tile_align_h=32,.tile_max_w=16416,.tile_max_h=16384,.num_vsc_pipes=32,.cs_shared_mem_size=32768,.num_sp_cores=6,.wave_granularity=2,.fibers_per_sp=4096,.threadsize_base=64,.max_waves=16,.highest_bank_bit=15,.ubwc_swizzle=6,.macrotile_mode=1,.num_ccu=6,.num_slices=3,.props={.has_gmem_fast_clear=True,.has_hw_multiview=True,.has_fs_tex_prefetch=True,.has_sampler_minmax=True,.has_astc_hdr=True,.num_xfb_streams=4,.supports_double_threadsize=False,.sysmem_per_ccu_depth_cache_size=262144,.sysmem_per_ccu_color_cache_size=131072,.gmem_ccu_color_cache_fraction=3,.gmem_ccu_depth_cache_fraction=0,.prim_alloc_threshold=7,.vs_max_inputs_count=32,.max_sets=8,.reg_size_vec4=96,.instr_cache_size=127,.supports_multiview_mask=True,.has_z24uint_s8uint=True,.tess_use_shared=True,.storage_16bit=True,.storage_8bit=True,.has_tex_filter_cubic=True,.has_separate_chroma_filter=True,.has_sample_locations=True,.has_lpac=True,.has_getfiberid=True,.has_movs=True,.has_dp2acc=False,.has_dp4acc=True,.enable_lrz_fast_clear=True,.has_lrz_dir_tracking=True,.has_lrz_feedback=True,.has_per_view_viewport=True,.line_width_min=1.0,.line_width_max=127.5,.has_scalar_alu=True,.has_scalar_predicates=True,.has_coherent_ubwc_flag_caches=True,.has_isam_v=True,.has_ssbo_imm_offsets=True,.has_early_preamble=True,.has_attachment_shading_rate=True,.has_ubwc_linear_mipmap_fallback=True,.supports_linear_mipmap_threshold_in_blocks=True,.prede_nop_quirk=True,.predtf_nop_quirk=True,.has_sad=True,.has_bin_mask=True,.has_sel_b_fneg=True,.has_pred_bit=True,.has_pc_dgen_so_cntl=True,.has_eolm_eogm=True,.round_robin_errata=False,.max_texel_buffer_range_elements=536870911,.max_storage_buffer_range_bytes=2147483647,.alias_mova_quirk=False,.alias_predication_quirk=True,.has_event_write_sample_count=True,.load_inline_uniforms_via_preamble_ldgk=True,.load_shader_consts_via_preamble=True,.has_gmem_vpc_attr_buf=True,.sysmem_vpc_attr_buf_size=131072,.gmem_vpc_attr_buf_size=49152,.ubwc_unorm_snorm_int_compatible=True,.supports_uav_ubwc=True,.has_generic_clear=True,.r8g8_faulty_fast_clear_quirk=True,.gs_vpc_adjacency_quirk=True,.ubwc_all_formats_compatible=True,.has_compliant_dp4acc=True,.ubwc_coherency_quirk=True,.has_persistent_counter=True,.has_64b_ssbo_atomics=True,.has_primitive_shading_rate=True,.has_ray_intersection=True,.has_sw_fuse=True,.has_rt_workaround=False,.has_alias_rt=True,.has_abs_bin_mask=True,.new_control_regs=True,.has_hw_bin_scaling=True,.has_image_processing=True,.max_draw_states=64,.has_64b_image_atomics=True,.has_implicit_fragface_fragcoord_ij_linear=True,.has_dual_wave_dispatch=True,.sysmem_vpc_pos_buf_size=65536,.sysmem_vpc_bv_pos_buf_size=32768,.sysmem_ccu_color_cache_fraction=0,.sysmem_ccu_depth_cache_fraction=0,.gmem_vpc_pos_buf_size=24576,.gmem_vpc_bv_pos_buf_size=32768,.gmem_per_ccu_color_cache_size=16384,.gmem_per_ccu_depth_cache_size=262144,.has_salu_int_narrowing_quirk=True,},.magic={},.magic_raw={{33320, 0}, {33321, 0}, {33322, 0}, {33323, 0}, {33324, 0}, {33325, 0}, {34840, 0}, {34841, 0}, {34842, 0}, {34843, 0}, {34844, 0}, {34845, 0}, {34846, 0}, {34969, 0}, {35057, 1}, {43777, 0}, {43778, 0}, {43811, 0}, {45442, 4}, {45443, 0}, {38923, 8389248}, {39680, 16128}},};
static const struct fd_dev_info __info30 = {.chip=8,.gmem_align_w=16,.gmem_align_h=4,.tile_align_w=64,.tile_align_h=32,.tile_max_w=16384,.tile_max_h=16384,.num_vsc_pipes=32,.cs_shared_mem_size=32768,.num_sp_cores=4,.wave_granularity=2,.fibers_per_sp=4096,.threadsize_base=64,.max_waves=16,.highest_bank_bit=15,.ubwc_swizzle=6,.macrotile_mode=1,.num_ccu=4,.num_slices=2,.props={.has_gmem_fast_clear=True,.has_hw_multiview=True,.has_fs_tex_prefetch=False,.has_sampler_minmax=True,.has_astc_hdr=True,.num_xfb_streams=4,.supports_double_threadsize=False,.sysmem_per_ccu_depth_cache_size=196608,.sysmem_per_ccu_color_cache_size=131072,.gmem_ccu_color_cache_fraction=3,.gmem_ccu_depth_cache_fraction=0,.prim_alloc_threshold=7,.vs_max_inputs_count=32,.max_sets=8,.reg_size_vec4=128,.instr_cache_size=127,.supports_multiview_mask=True,.has_z24uint_s8uint=True,.tess_use_shared=True,.storage_16bit=True,.storage_8bit=True,.has_tex_filter_cubic=True,.has_separate_chroma_filter=True,.has_sample_locations=True,.has_lpac=True,.has_getfiberid=True,.has_movs=True,.has_dp2acc=False,.has_dp4acc=True,.enable_lrz_fast_clear=True,.has_lrz_dir_tracking=True,.has_lrz_feedback=True,.has_per_view_viewport=True,.line_width_min=1.0,.line_width_max=127.5,.has_scalar_alu=True,.has_scalar_predicates=True,.has_coherent_ubwc_flag_caches=True,.has_isam_v=True,.has_ssbo_imm_offsets=True,.has_early_preamble=True,.has_attachment_shading_rate=True,.has_ubwc_linear_mipmap_fallback=True,.supports_linear_mipmap_threshold_in_blocks=True,.prede_nop_quirk=True,.predtf_nop_quirk=True,.has_sad=True,.has_bin_mask=True,.has_sel_b_fneg=True,.has_pred_bit=True,.has_pc_dgen_so_cntl=True,.has_eolm_eogm=True,.round_robin_errata=False,.max_texel_buffer_range_elements=536870911,.max_storage_buffer_range_bytes=2147483647,.alias_mova_quirk=False,.alias_predication_quirk=True,.has_event_write_sample_count=True,.load_inline_uniforms_via_preamble_ldgk=True,.load_shader_consts_via_preamble=True,.has_gmem_vpc_attr_buf=True,.sysmem_vpc_attr_buf_size=131072,.gmem_vpc_attr_buf_size=49152,.ubwc_unorm_snorm_int_compatible=True,.supports_uav_ubwc=True,.has_generic_clear=True,.r8g8_faulty_fast_clear_quirk=True,.gs_vpc_adjacency_quirk=True,.ubwc_all_formats_compatible=True,.has_compliant_dp4acc=True,.ubwc_coherency_quirk=True,.has_persistent_counter=True,.has_64b_ssbo_atomics=True,.has_primitive_shading_rate=True,.has_ray_intersection=True,.has_sw_fuse=True,.has_rt_workaround=False,.has_alias_rt=True,.has_abs_bin_mask=True,.new_control_regs=True,.has_hw_bin_scaling=True,.has_image_processing=True,.max_draw_states=64,.has_64b_image_atomics=True,.has_implicit_fragface_fragcoord_ij_linear=True,.has_dual_wave_dispatch=True,.sysmem_vpc_pos_buf_size=65536,.sysmem_vpc_bv_pos_buf_size=24576,.sysmem_ccu_color_cache_fraction=0,.sysmem_ccu_depth_cache_fraction=3,.gmem_vpc_pos_buf_size=24576,.gmem_vpc_bv_pos_buf_size=32768,.gmem_per_ccu_color_cache_size=16384,.gmem_per_ccu_depth_cache_size=262144,.has_salu_int_narrowing_quirk=True,.shading_rate_matches_vk=True,},.magic={},.magic_raw={{33320, 0}, {33321, 0}, {33322, 0}, {33323, 0}, {33324, 0}, {33325, 0}, {34840, 0}, {34841, 0}, {34842, 0}, {34843, 0}, {34844, 0}, {34845, 0}, {34846, 0}, {34969, 0}, {35057, 1}, {43777, 0}, {43778, 0}, {43811, 0}, {45442, 4}, {45443, 0}, {38923, 8389248}, {39680, 16128}},};
static const struct fd_dev_info __info31 = {.chip=8,.gmem_align_w=16,.gmem_align_h=4,.tile_align_w=96,.tile_align_h=32,.tile_max_w=16416,.tile_max_h=16384,.num_vsc_pipes=32,.cs_shared_mem_size=32768,.num_sp_cores=6,.wave_granularity=2,.fibers_per_sp=4096,.threadsize_base=64,.max_waves=16,.highest_bank_bit=15,.ubwc_swizzle=6,.macrotile_mode=1,.num_ccu=6,.num_slices=3,.props={.has_gmem_fast_clear=True,.has_hw_multiview=True,.has_fs_tex_prefetch=False,.has_sampler_minmax=True,.has_astc_hdr=True,.num_xfb_streams=4,.supports_double_threadsize=False,.sysmem_per_ccu_depth_cache_size=196608,.sysmem_per_ccu_color_cache_size=131072,.gmem_ccu_color_cache_fraction=3,.gmem_ccu_depth_cache_fraction=0,.prim_alloc_threshold=7,.vs_max_inputs_count=32,.max_sets=8,.reg_size_vec4=128,.instr_cache_size=127,.supports_multiview_mask=True,.has_z24uint_s8uint=True,.tess_use_shared=True,.storage_16bit=True,.storage_8bit=True,.has_tex_filter_cubic=True,.has_separate_chroma_filter=True,.has_sample_locations=True,.has_lpac=True,.has_getfiberid=True,.has_movs=True,.has_dp2acc=False,.has_dp4acc=True,.enable_lrz_fast_clear=True,.has_lrz_dir_tracking=True,.has_lrz_feedback=True,.has_per_view_viewport=True,.line_width_min=1.0,.line_width_max=127.5,.has_scalar_alu=True,.has_scalar_predicates=True,.has_coherent_ubwc_flag_caches=True,.has_isam_v=True,.has_ssbo_imm_offsets=True,.has_early_preamble=True,.has_attachment_shading_rate=True,.has_ubwc_linear_mipmap_fallback=True,.supports_linear_mipmap_threshold_in_blocks=True,.prede_nop_quirk=True,.predtf_nop_quirk=True,.has_sad=True,.has_bin_mask=True,.has_sel_b_fneg=True,.has_pred_bit=True,.has_pc_dgen_so_cntl=True,.has_eolm_eogm=True,.round_robin_errata=False,.max_texel_buffer_range_elements=536870911,.max_storage_buffer_range_bytes=2147483647,.alias_mova_quirk=False,.alias_predication_quirk=True,.has_event_write_sample_count=True,.load_inline_uniforms_via_preamble_ldgk=True,.load_shader_consts_via_preamble=True,.has_gmem_vpc_attr_buf=True,.sysmem_vpc_attr_buf_size=131072,.gmem_vpc_attr_buf_size=49152,.ubwc_unorm_snorm_int_compatible=True,.supports_uav_ubwc=True,.has_generic_clear=True,.r8g8_faulty_fast_clear_quirk=True,.gs_vpc_adjacency_quirk=True,.ubwc_all_formats_compatible=True,.has_compliant_dp4acc=True,.ubwc_coherency_quirk=True,.has_persistent_counter=True,.has_64b_ssbo_atomics=True,.has_primitive_shading_rate=True,.has_ray_intersection=True,.has_sw_fuse=True,.has_rt_workaround=False,.has_alias_rt=True,.has_abs_bin_mask=True,.new_control_regs=True,.has_hw_bin_scaling=True,.has_image_processing=True,.max_draw_states=64,.has_64b_image_atomics=True,.has_implicit_fragface_fragcoord_ij_linear=True,.has_dual_wave_dispatch=True,.sysmem_vpc_pos_buf_size=65536,.sysmem_vpc_bv_pos_buf_size=32768,.sysmem_ccu_color_cache_fraction=0,.sysmem_ccu_depth_cache_fraction=3,.gmem_vpc_pos_buf_size=24576,.gmem_vpc_bv_pos_buf_size=32768,.gmem_per_ccu_color_cache_size=16384,.gmem_per_ccu_depth_cache_size=262144,.has_salu_int_narrowing_quirk=True,.shading_rate_matches_vk=True,},.magic={},.magic_raw={{33320, 0}, {33321, 0}, {33322, 0}, {33323, 0}, {33324, 0}, {33325, 0}, {34840, 0}, {34841, 0}, {34842, 0}, {34843, 0}, {34844, 0}, {34845, 0}, {34846, 0}, {34969, 0}, {35057, 1}, {43777, 0}, {43778, 0}, {43811, 0}, {45442, 4}, {45443, 0}, {38923, 8389248}, {39680, 16128}},};
static const struct fd_dev_info __info32 = {.chip=8,.gmem_align_w=16,.gmem_align_h=4,.tile_align_w=96,.tile_align_h=32,.tile_max_w=16416,.tile_max_h=16384,.num_vsc_pipes=32,.cs_shared_mem_size=32768,.num_sp_cores=6,.wave_granularity=2,.fibers_per_sp=4096,.threadsize_base=64,.max_waves=16,.highest_bank_bit=15,.ubwc_swizzle=6,.macrotile_mode=1,.num_ccu=6,.num_slices=3,.props={.has_gmem_fast_clear=True,.has_hw_multiview=True,.has_fs_tex_prefetch=False,.has_sampler_minmax=True,.has_astc_hdr=True,.num_xfb_streams=4,.supports_double_threadsize=False,.sysmem_per_ccu_depth_cache_size=196608,.sysmem_per_ccu_color_cache_size=131072,.gmem_ccu_color_cache_fraction=3,.gmem_ccu_depth_cache_fraction=0,.prim_alloc_threshold=7,.vs_max_inputs_count=32,.max_sets=8,.reg_size_vec4=128,.instr_cache_size=127,.supports_multiview_mask=True,.has_z24uint_s8uint=True,.tess_use_shared=True,.storage_16bit=True,.storage_8bit=True,.has_tex_filter_cubic=True,.has_separate_chroma_filter=True,.has_sample_locations=True,.has_lpac=True,.has_getfiberid=True,.has_movs=True,.has_dp2acc=False,.has_dp4acc=True,.enable_lrz_fast_clear=True,.has_lrz_dir_tracking=True,.has_lrz_feedback=True,.has_per_view_viewport=True,.line_width_min=1.0,.line_width_max=127.5,.has_scalar_alu=True,.has_scalar_predicates=True,.has_coherent_ubwc_flag_caches=True,.has_isam_v=True,.has_ssbo_imm_offsets=True,.has_early_preamble=True,.has_attachment_shading_rate=True,.has_ubwc_linear_mipmap_fallback=True,.supports_linear_mipmap_threshold_in_blocks=True,.prede_nop_quirk=True,.predtf_nop_quirk=True,.has_sad=True,.has_bin_mask=True,.has_sel_b_fneg=True,.has_pred_bit=True,.has_pc_dgen_so_cntl=True,.has_eolm_eogm=True,.round_robin_errata=False,.max_texel_buffer_range_elements=536870911,.max_storage_buffer_range_bytes=2147483647,.alias_mova_quirk=False,.alias_predication_quirk=True,.has_event_write_sample_count=True,.load_inline_uniforms_via_preamble_ldgk=True,.load_shader_consts_via_preamble=True,.has_gmem_vpc_attr_buf=True,.sysmem_vpc_attr_buf_size=131072,.gmem_vpc_attr_buf_size=49152,.ubwc_unorm_snorm_int_compatible=True,.supports_uav_ubwc=True,.has_generic_clear=True,.r8g8_faulty_fast_clear_quirk=True,.gs_vpc_adjacency_quirk=True,.ubwc_all_formats_compatible=True,.has_compliant_dp4acc=True,.ubwc_coherency_quirk=True,.has_persistent_counter=True,.has_64b_ssbo_atomics=True,.has_primitive_shading_rate=True,.has_ray_intersection=True,.has_sw_fuse=True,.has_rt_workaround=False,.has_alias_rt=True,.has_abs_bin_mask=True,.new_control_regs=True,.has_hw_bin_scaling=True,.has_image_processing=True,.max_draw_states=64,.has_64b_image_atomics=True,.has_implicit_fragface_fragcoord_ij_linear=True,.has_dual_wave_dispatch=True,.sysmem_vpc_pos_buf_size=65536,.sysmem_vpc_bv_pos_buf_size=32768,.sysmem_ccu_color_cache_fraction=0,.sysmem_ccu_depth_cache_fraction=3,.gmem_vpc_pos_buf_size=24576,.gmem_vpc_bv_pos_buf_size=32768,.gmem_per_ccu_color_cache_size=16384,.gmem_per_ccu_depth_cache_size=262144,.has_salu_int_narrowing_quirk=True,},.magic={},.magic_raw={{33320, 0}, {33321, 0}, {33322, 0}, {33323, 0}, {33324, 0}, {33325, 0}, {34840, 0}, {34841, 0}, {34842, 0}, {34843, 0}, {34844, 0}, {34845, 0}, {34846, 0}, {34969, 0}, {35057, 1}, {43777, 0}, {43778, 0}, {43811, 0}, {45442, 4}, {45443, 0}, {38923, 8389248}, {39680, 16128}},};
static const struct fd_dev_info __info33 = {.chip=8,.gmem_align_w=16,.gmem_align_h=4,.tile_align_w=64,.tile_align_h=64,.tile_max_w=16384,.tile_max_h=16384,.num_vsc_pipes=32,.cs_shared_mem_size=32768,.num_sp_cores=8,.wave_granularity=2,.fibers_per_sp=4096,.threadsize_base=64,.max_waves=16,.highest_bank_bit=15,.ubwc_swizzle=6,.macrotile_mode=1,.num_ccu=8,.num_slices=4,.props={.has_gmem_fast_clear=True,.has_hw_multiview=True,.has_fs_tex_prefetch=False,.has_sampler_minmax=True,.has_astc_hdr=True,.num_xfb_streams=4,.supports_double_threadsize=False,.sysmem_per_ccu_depth_cache_size=196608,.sysmem_per_ccu_color_cache_size=131072,.gmem_ccu_color_cache_fraction=3,.gmem_ccu_depth_cache_fraction=0,.prim_alloc_threshold=7,.vs_max_inputs_count=32,.max_sets=8,.reg_size_vec4=128,.instr_cache_size=127,.supports_multiview_mask=True,.has_z24uint_s8uint=True,.tess_use_shared=True,.storage_16bit=True,.storage_8bit=True,.has_tex_filter_cubic=True,.has_separate_chroma_filter=True,.has_sample_locations=True,.has_lpac=True,.has_getfiberid=True,.has_movs=True,.has_dp2acc=False,.has_dp4acc=True,.enable_lrz_fast_clear=True,.has_lrz_dir_tracking=True,.has_lrz_feedback=True,.has_per_view_viewport=True,.line_width_min=1.0,.line_width_max=127.5,.has_scalar_alu=True,.has_scalar_predicates=True,.has_coherent_ubwc_flag_caches=True,.has_isam_v=True,.has_ssbo_imm_offsets=True,.has_early_preamble=True,.has_attachment_shading_rate=True,.has_ubwc_linear_mipmap_fallback=True,.supports_linear_mipmap_threshold_in_blocks=True,.prede_nop_quirk=True,.predtf_nop_quirk=True,.has_sad=True,.has_bin_mask=True,.has_sel_b_fneg=True,.has_pred_bit=True,.has_pc_dgen_so_cntl=True,.has_eolm_eogm=True,.round_robin_errata=False,.max_texel_buffer_range_elements=536870911,.max_storage_buffer_range_bytes=2147483647,.alias_mova_quirk=False,.alias_predication_quirk=True,.has_event_write_sample_count=True,.load_inline_uniforms_via_preamble_ldgk=True,.load_shader_consts_via_preamble=True,.has_gmem_vpc_attr_buf=True,.sysmem_vpc_attr_buf_size=131072,.gmem_vpc_attr_buf_size=49152,.ubwc_unorm_snorm_int_compatible=True,.supports_uav_ubwc=True,.has_generic_clear=True,.r8g8_faulty_fast_clear_quirk=True,.gs_vpc_adjacency_quirk=True,.ubwc_all_formats_compatible=True,.has_compliant_dp4acc=True,.ubwc_coherency_quirk=True,.has_persistent_counter=True,.has_64b_ssbo_atomics=True,.has_primitive_shading_rate=True,.has_ray_intersection=True,.has_sw_fuse=True,.has_rt_workaround=False,.has_alias_rt=True,.has_abs_bin_mask=True,.new_control_regs=True,.has_hw_bin_scaling=True,.has_image_processing=True,.max_draw_states=64,.has_64b_image_atomics=True,.has_implicit_fragface_fragcoord_ij_linear=True,.has_dual_wave_dispatch=True,.sysmem_vpc_pos_buf_size=65536,.sysmem_vpc_bv_pos_buf_size=32768,.sysmem_ccu_color_cache_fraction=0,.sysmem_ccu_depth_cache_fraction=3,.gmem_vpc_pos_buf_size=24576,.gmem_vpc_bv_pos_buf_size=32768,.gmem_per_ccu_color_cache_size=16384,.gmem_per_ccu_depth_cache_size=262144,.has_salu_int_narrowing_quirk=True,},.magic={},.magic_raw={{33320, 0}, {33321, 0}, {33322, 0}, {33323, 0}, {33324, 0}, {33325, 0}, {34840, 0}, {34841, 0}, {34842, 0}, {34843, 0}, {34844, 0}, {34845, 0}, {34846, 0}, {34969, 0}, {35057, 1}, {43777, 0}, {43778, 0}, {43811, 0}, {45442, 4}, {45443, 0}, {38923, 8389248}, {39680, 16128}},};

static const struct fd_dev_rec fd_dev_recs[] = {
   { {200, 0x20000ff}, "FD200", &__info0 },
   { {201, 0x20001ff}, "FD201", &__info0 },
   { {205, 0x20005ff}, "FD205", &__info0 },
   { {220, 0x20200ff}, "FD220", &__info0 },
   { {225, 0x20205ff}, "FD225", &__info0 },
   { {305, 0x30005ff}, "FD305", &__info1 },
   { {307, 0x30007ff}, "FD307", &__info1 },
   { {320, 0x30200ff}, "FD320", &__info1 },
   { {330, 0x30300ff}, "FD330", &__info1 },
   { {0, 0x3000512}, "FD305B", &__info1 },
   { {0, 0x3000620}, "FD306A", &__info1 },
   { {405, 0x40005ff}, "FD405", &__info2 },
   { {420, 0x40200ff}, "FD420", &__info2 },
   { {430, 0x40300ff}, "FD430", &__info2 },
   { {505, 0x50005ff}, "FD505", &__info3 },
   { {506, 0x50006ff}, "FD506", &__info3 },
   { {508, 0x50008ff}, "FD508", &__info3 },
   { {509, 0x50009ff}, "FD509", &__info3 },
   { {510, 0x50100ff}, "FD510", &__info4 },
   { {512, 0x50102ff}, "FD512", &__info4 },
   { {530, 0x50300ff}, "FD530", &__info5 },
   { {540, 0x50400ff}, "FD540", &__info5 },
   { {605, 0x60005ff}, "FD605", &__info6 },
   { {610, 0x60100ff}, "FD610", &__info6 },
   { {608, 0x60008ff}, "FD608", &__info7 },
   { {612, 0x60102ff}, "FD612", &__info7 },
   { {613, 0x60103ff}, "FD613", &__info7 },
   { {615, 0x60105ff}, "FD615", &__info8 },
   { {616, 0x60106ff}, "FD616", &__info8 },
   { {618, 0x60108ff}, "FD618", &__info8 },
   { {619, 0x60109ff}, "FD619", &__info8 },
   { {620, 0x60200ff}, "FD620", &__info9 },
   { {0, 0xffff06020100}, "FD621", &__info10 },
   { {0, 0xffff06020300}, "Adreno623", &__info10 },
   { {630, 0x60300ff}, "FD630", &__info11 },
   { {640, 0x60400ff}, "FD640", &__info12 },
   { {680, 0x60800ff}, "FD680", &__info13 },
   { {650, 0x60500ff}, "FD650", &__info14 },
   { {0, 0xbe06030500}, "Adreno 8c Gen 3", &__info15 },
   { {0, 0x7506030500}, "Adreno 7c+ Gen 3", &__info15 },
   { {0, 0x6006030500}, "Adreno 7c+ Gen 3 Lite", &__info15 },
   { {0, 0xac06030500}, "FD643", &__info15 },
   { {0, 0xffff06030500}, "Adreno 7c+ Gen 3", &__info15 },
   { {660, 0x60600ff}, "FD660", &__info16 },
   { {0, 0x6060201}, "FD644", &__info17 },
   { {0, 0xffff06060300}, "FD663", &__info17 },
   { {690, 0x60900ff}, "FD690", &__info18 },
   { {0, 0xffff06090000}, "FD690", &__info18 },
   { {702, 0x70002ff}, "FD702", &__info19 },
   { {0, 0xb207002000}, "FD702", &__info19 },
   { {0, 0xffff07002000}, "FD702", &__info19 },
   { {0, 0xffff07000400}, "Adreno (TM) 704", &__info19 },
   { {0, 0x43020100}, "Adreno (TM) 722", &__info20 },
   { {0, 0xffff43020100}, "Adreno (TM) 722", &__info20 },
   { {0, 0x7030002}, "FD725", &__info21 },
   { {0, 0xffff07030002}, "FD725", &__info21 },
   { {0, 0x7030001}, "FD730", &__info22 },
   { {0, 0xffff07030001}, "FD730", &__info22 },
   { {0, 0xffff43030c00}, "Adreno X1-45", &__info23 },
   { {0, 0x43030b00}, "FD735", &__info23 },
   { {740, 0x70400ff}, "FD740", &__info24 },
   { {0, 0x43050a01}, "FD740", &__info24 },
   { {0, 0xffff43050a01}, "FD740", &__info24 },
   { {0, 0xffff43050c01}, "Adreno X1-85", &__info24 },
   { {0, 0x43050a00}, "FDA32", &__info25 },
   { {0, 0xffff43050a00}, "FDA32", &__info25 },
   { {0, 0x43050b00}, "FD740v3", &__info26 },
   { {0, 0xffff43050b00}, "FD740v3", &__info26 },
   { {0, 0x43051401}, "FD750", &__info27 },
   { {0, 0xffff43051401}, "FD750", &__info27 },
   { {0, 0xffff44010000}, "Adreno (TM) 810", &__info28 },
   { {0, 0xffff44050000}, "Adreno (TM) 830", &__info29 },
   { {0, 0xffff44050001}, "Adreno (TM) 830v1", &__info29 },
   { {0, 0x44050001}, "Adreno (TM) 830", &__info29 },
   { {0, 0x44030a20}, "Adreno (TM) 829", &__info30 },
   { {0, 0xffff44050a31}, "Adreno (TM) 840", &__info31 },
   { {0, 0xffff44070031}, "Adreno (TM) X2-85", &__info32 },
   { {0, 0xffff44070041}, "Adreno (TM) X2-90", &__info33 },
};

void
fd_dev_info_apply_dbg_options(struct fd_dev_info *info)
{
    const char *env = debug_get_option("FD_DEV_FEATURES", NULL);
    if (!env || !*env)
        return;

    char *features = strdup(env);
    char *feature, *feature_end;
    feature = strtok_r(features, ":", &feature_end);
    while (feature != NULL) {
        char *name, *name_end;
        name = strtok_r(feature, "=", &name_end);

        if (!name) {
            mesa_loge("Invalid feature \"%s\" in FD_DEV_FEATURES", feature);
            exit(1);
        }

        char *value = strtok_r(NULL, "=", &name_end);

        feature = strtok_r(NULL, ":", &feature_end);

  
        if (strcmp(name, "has_cp_reg_write") == 0) {
            info->props.has_cp_reg_write = debug_parse_bool_option(value, info->props.has_cp_reg_write);
            continue;
        }
  
        if (strcmp(name, "has_8bpp_ubwc") == 0) {
            info->props.has_8bpp_ubwc = debug_parse_bool_option(value, info->props.has_8bpp_ubwc);
            continue;
        }
  
        if (strcmp(name, "has_gmem_fast_clear") == 0) {
            info->props.has_gmem_fast_clear = debug_parse_bool_option(value, info->props.has_gmem_fast_clear);
            continue;
        }
  
        if (strcmp(name, "has_hw_multiview") == 0) {
            info->props.has_hw_multiview = debug_parse_bool_option(value, info->props.has_hw_multiview);
            continue;
        }
  
        if (strcmp(name, "has_fs_tex_prefetch") == 0) {
            info->props.has_fs_tex_prefetch = debug_parse_bool_option(value, info->props.has_fs_tex_prefetch);
            continue;
        }
  
        if (strcmp(name, "has_sampler_minmax") == 0) {
            info->props.has_sampler_minmax = debug_parse_bool_option(value, info->props.has_sampler_minmax);
            continue;
        }
  
        if (strcmp(name, "has_astc_hdr") == 0) {
            info->props.has_astc_hdr = debug_parse_bool_option(value, info->props.has_astc_hdr);
            continue;
        }
  
        if (strcmp(name, "num_xfb_streams") == 0) {
            info->props.num_xfb_streams = debug_parse_num_option(value, info->props.num_xfb_streams);
            continue;
        }
  
        if (strcmp(name, "supports_double_threadsize") == 0) {
            info->props.supports_double_threadsize = debug_parse_bool_option(value, info->props.supports_double_threadsize);
            continue;
        }
  
        if (strcmp(name, "sysmem_per_ccu_depth_cache_size") == 0) {
            info->props.sysmem_per_ccu_depth_cache_size = debug_parse_num_option(value, info->props.sysmem_per_ccu_depth_cache_size);
            continue;
        }
  
        if (strcmp(name, "sysmem_per_ccu_color_cache_size") == 0) {
            info->props.sysmem_per_ccu_color_cache_size = debug_parse_num_option(value, info->props.sysmem_per_ccu_color_cache_size);
            continue;
        }
  
        if (strcmp(name, "gmem_ccu_color_cache_fraction") == 0) {
            info->props.gmem_ccu_color_cache_fraction = debug_parse_num_option(value, info->props.gmem_ccu_color_cache_fraction);
            continue;
        }
  
        if (strcmp(name, "gmem_ccu_depth_cache_fraction") == 0) {
            info->props.gmem_ccu_depth_cache_fraction = debug_parse_num_option(value, info->props.gmem_ccu_depth_cache_fraction);
            continue;
        }
  
        if (strcmp(name, "prim_alloc_threshold") == 0) {
            info->props.prim_alloc_threshold = debug_parse_num_option(value, info->props.prim_alloc_threshold);
            continue;
        }
  
        if (strcmp(name, "vs_max_inputs_count") == 0) {
            info->props.vs_max_inputs_count = debug_parse_num_option(value, info->props.vs_max_inputs_count);
            continue;
        }
  
        if (strcmp(name, "max_sets") == 0) {
            info->props.max_sets = debug_parse_num_option(value, info->props.max_sets);
            continue;
        }
  
        if (strcmp(name, "line_width_min") == 0) {
            info->props.line_width_min = debug_parse_num_option(value, info->props.line_width_min);
            continue;
        }
  
        if (strcmp(name, "line_width_max") == 0) {
            info->props.line_width_max = debug_parse_num_option(value, info->props.line_width_max);
            continue;
        }
  
        if (strcmp(name, "mov_half_shared_quirk") == 0) {
            info->props.mov_half_shared_quirk = debug_parse_bool_option(value, info->props.mov_half_shared_quirk);
            continue;
        }
  
        if (strcmp(name, "max_draw_states") == 0) {
            info->props.max_draw_states = debug_parse_num_option(value, info->props.max_draw_states);
            continue;
        }
  
        if (strcmp(name, "max_texel_buffer_range_elements") == 0) {
            info->props.max_texel_buffer_range_elements = debug_parse_num_option(value, info->props.max_texel_buffer_range_elements);
            continue;
        }
  
        if (strcmp(name, "max_storage_buffer_range_bytes") == 0) {
            info->props.max_storage_buffer_range_bytes = debug_parse_num_option(value, info->props.max_storage_buffer_range_bytes);
            continue;
        }
  
        if (strcmp(name, "reg_size_vec4") == 0) {
            info->props.reg_size_vec4 = debug_parse_num_option(value, info->props.reg_size_vec4);
            continue;
        }
  
        if (strcmp(name, "instr_cache_size") == 0) {
            info->props.instr_cache_size = debug_parse_num_option(value, info->props.instr_cache_size);
            continue;
        }
  
        if (strcmp(name, "indirect_draw_wfm_quirk") == 0) {
            info->props.indirect_draw_wfm_quirk = debug_parse_bool_option(value, info->props.indirect_draw_wfm_quirk);
            continue;
        }
  
        if (strcmp(name, "depth_bounds_require_depth_test_quirk") == 0) {
            info->props.depth_bounds_require_depth_test_quirk = debug_parse_bool_option(value, info->props.depth_bounds_require_depth_test_quirk);
            continue;
        }
  
        if (strcmp(name, "blit_wfi_quirk") == 0) {
            info->props.blit_wfi_quirk = debug_parse_bool_option(value, info->props.blit_wfi_quirk);
            continue;
        }
  
        if (strcmp(name, "supports_multiview_mask") == 0) {
            info->props.supports_multiview_mask = debug_parse_bool_option(value, info->props.supports_multiview_mask);
            continue;
        }
  
        if (strcmp(name, "has_z24uint_s8uint") == 0) {
            info->props.has_z24uint_s8uint = debug_parse_bool_option(value, info->props.has_z24uint_s8uint);
            continue;
        }
  
        if (strcmp(name, "tess_use_shared") == 0) {
            info->props.tess_use_shared = debug_parse_bool_option(value, info->props.tess_use_shared);
            continue;
        }
  
        if (strcmp(name, "storage_16bit") == 0) {
            info->props.storage_16bit = debug_parse_bool_option(value, info->props.storage_16bit);
            continue;
        }
  
        if (strcmp(name, "has_tex_filter_cubic") == 0) {
            info->props.has_tex_filter_cubic = debug_parse_bool_option(value, info->props.has_tex_filter_cubic);
            continue;
        }
  
        if (strcmp(name, "has_separate_chroma_filter") == 0) {
            info->props.has_separate_chroma_filter = debug_parse_bool_option(value, info->props.has_separate_chroma_filter);
            continue;
        }
  
        if (strcmp(name, "has_sample_locations") == 0) {
            info->props.has_sample_locations = debug_parse_bool_option(value, info->props.has_sample_locations);
            continue;
        }
  
        if (strcmp(name, "has_dp2acc") == 0) {
            info->props.has_dp2acc = debug_parse_bool_option(value, info->props.has_dp2acc);
            continue;
        }
  
        if (strcmp(name, "has_lrz_dir_tracking") == 0) {
            info->props.has_lrz_dir_tracking = debug_parse_bool_option(value, info->props.has_lrz_dir_tracking);
            continue;
        }
  
        if (strcmp(name, "enable_lrz_fast_clear") == 0) {
            info->props.enable_lrz_fast_clear = debug_parse_bool_option(value, info->props.enable_lrz_fast_clear);
            continue;
        }
  
        if (strcmp(name, "lrz_track_quirk") == 0) {
            info->props.lrz_track_quirk = debug_parse_bool_option(value, info->props.lrz_track_quirk);
            continue;
        }
  
        if (strcmp(name, "has_lrz_feedback") == 0) {
            info->props.has_lrz_feedback = debug_parse_bool_option(value, info->props.has_lrz_feedback);
            continue;
        }
  
        if (strcmp(name, "has_per_view_viewport") == 0) {
            info->props.has_per_view_viewport = debug_parse_bool_option(value, info->props.has_per_view_viewport);
            continue;
        }
  
        if (strcmp(name, "has_scalar_alu") == 0) {
            info->props.has_scalar_alu = debug_parse_bool_option(value, info->props.has_scalar_alu);
            continue;
        }
  
        if (strcmp(name, "has_early_preamble") == 0) {
            info->props.has_early_preamble = debug_parse_bool_option(value, info->props.has_early_preamble);
            continue;
        }
  
        if (strcmp(name, "prede_nop_quirk") == 0) {
            info->props.prede_nop_quirk = debug_parse_bool_option(value, info->props.prede_nop_quirk);
            continue;
        }
  
        if (strcmp(name, "has_pred_bit") == 0) {
            info->props.has_pred_bit = debug_parse_bool_option(value, info->props.has_pred_bit);
            continue;
        }
  
        if (strcmp(name, "has_pc_dgen_so_cntl") == 0) {
            info->props.has_pc_dgen_so_cntl = debug_parse_bool_option(value, info->props.has_pc_dgen_so_cntl);
            continue;
        }
  
        if (strcmp(name, "round_robin_errata") == 0) {
            info->props.round_robin_errata = debug_parse_bool_option(value, info->props.round_robin_errata);
            continue;
        }
  
        if (strcmp(name, "has_lpac") == 0) {
            info->props.has_lpac = debug_parse_bool_option(value, info->props.has_lpac);
            continue;
        }
  
        if (strcmp(name, "has_legacy_pipeline_shading_rate") == 0) {
            info->props.has_legacy_pipeline_shading_rate = debug_parse_bool_option(value, info->props.has_legacy_pipeline_shading_rate);
            continue;
        }
  
        if (strcmp(name, "has_getfiberid") == 0) {
            info->props.has_getfiberid = debug_parse_bool_option(value, info->props.has_getfiberid);
            continue;
        }
  
        if (strcmp(name, "has_movs") == 0) {
            info->props.has_movs = debug_parse_bool_option(value, info->props.has_movs);
            continue;
        }
  
        if (strcmp(name, "has_dp4acc") == 0) {
            info->props.has_dp4acc = debug_parse_bool_option(value, info->props.has_dp4acc);
            continue;
        }
  
        if (strcmp(name, "has_isam_v") == 0) {
            info->props.has_isam_v = debug_parse_bool_option(value, info->props.has_isam_v);
            continue;
        }
  
        if (strcmp(name, "has_ssbo_imm_offsets") == 0) {
            info->props.has_ssbo_imm_offsets = debug_parse_bool_option(value, info->props.has_ssbo_imm_offsets);
            continue;
        }
  
        if (strcmp(name, "has_ubwc_linear_mipmap_fallback") == 0) {
            info->props.has_ubwc_linear_mipmap_fallback = debug_parse_bool_option(value, info->props.has_ubwc_linear_mipmap_fallback);
            continue;
        }
  
        if (strcmp(name, "predtf_nop_quirk") == 0) {
            info->props.predtf_nop_quirk = debug_parse_bool_option(value, info->props.predtf_nop_quirk);
            continue;
        }
  
        if (strcmp(name, "has_sad") == 0) {
            info->props.has_sad = debug_parse_bool_option(value, info->props.has_sad);
            continue;
        }
  
        if (strcmp(name, "has_sel_b_fneg") == 0) {
            info->props.has_sel_b_fneg = debug_parse_bool_option(value, info->props.has_sel_b_fneg);
            continue;
        }
  
        if (strcmp(name, "broken_ds_ubwc_quirk") == 0) {
            info->props.broken_ds_ubwc_quirk = debug_parse_bool_option(value, info->props.broken_ds_ubwc_quirk);
            continue;
        }
  
        if (strcmp(name, "is_a702") == 0) {
            info->props.is_a702 = debug_parse_bool_option(value, info->props.is_a702);
            continue;
        }
  
        if (strcmp(name, "storage_8bit") == 0) {
            info->props.storage_8bit = debug_parse_bool_option(value, info->props.storage_8bit);
            continue;
        }
  
        if (strcmp(name, "has_scalar_predicates") == 0) {
            info->props.has_scalar_predicates = debug_parse_bool_option(value, info->props.has_scalar_predicates);
            continue;
        }
  
        if (strcmp(name, "has_coherent_ubwc_flag_caches") == 0) {
            info->props.has_coherent_ubwc_flag_caches = debug_parse_bool_option(value, info->props.has_coherent_ubwc_flag_caches);
            continue;
        }
  
        if (strcmp(name, "has_attachment_shading_rate") == 0) {
            info->props.has_attachment_shading_rate = debug_parse_bool_option(value, info->props.has_attachment_shading_rate);
            continue;
        }
  
        if (strcmp(name, "supports_linear_mipmap_threshold_in_blocks") == 0) {
            info->props.supports_linear_mipmap_threshold_in_blocks = debug_parse_bool_option(value, info->props.supports_linear_mipmap_threshold_in_blocks);
            continue;
        }
  
        if (strcmp(name, "has_bin_mask") == 0) {
            info->props.has_bin_mask = debug_parse_bool_option(value, info->props.has_bin_mask);
            continue;
        }
  
        if (strcmp(name, "has_eolm_eogm") == 0) {
            info->props.has_eolm_eogm = debug_parse_bool_option(value, info->props.has_eolm_eogm);
            continue;
        }
  
        if (strcmp(name, "alias_mova_quirk") == 0) {
            info->props.alias_mova_quirk = debug_parse_bool_option(value, info->props.alias_mova_quirk);
            continue;
        }
  
        if (strcmp(name, "alias_predication_quirk") == 0) {
            info->props.alias_predication_quirk = debug_parse_bool_option(value, info->props.alias_predication_quirk);
            continue;
        }
  
        if (strcmp(name, "supports_uav_ubwc") == 0) {
            info->props.supports_uav_ubwc = debug_parse_bool_option(value, info->props.supports_uav_ubwc);
            continue;
        }
  
        if (strcmp(name, "fs_must_have_non_zero_constlen_quirk") == 0) {
            info->props.fs_must_have_non_zero_constlen_quirk = debug_parse_bool_option(value, info->props.fs_must_have_non_zero_constlen_quirk);
            continue;
        }
  
        if (strcmp(name, "enable_tp_ubwc_flag_hint") == 0) {
            info->props.enable_tp_ubwc_flag_hint = debug_parse_bool_option(value, info->props.enable_tp_ubwc_flag_hint);
            continue;
        }
  
        if (strcmp(name, "reading_shading_rate_requires_smask_quirk") == 0) {
            info->props.reading_shading_rate_requires_smask_quirk = debug_parse_bool_option(value, info->props.reading_shading_rate_requires_smask_quirk);
            continue;
        }
  
        if (strcmp(name, "cs_lock_unlock_quirk") == 0) {
            info->props.cs_lock_unlock_quirk = debug_parse_bool_option(value, info->props.cs_lock_unlock_quirk);
            continue;
        }
  
        if (strcmp(name, "cmdbuf_start_a725_quirk") == 0) {
            info->props.cmdbuf_start_a725_quirk = debug_parse_bool_option(value, info->props.cmdbuf_start_a725_quirk);
            continue;
        }
  
        if (strcmp(name, "stsc_duplication_quirk") == 0) {
            info->props.stsc_duplication_quirk = debug_parse_bool_option(value, info->props.stsc_duplication_quirk);
            continue;
        }
  
        if (strcmp(name, "has_event_write_sample_count") == 0) {
            info->props.has_event_write_sample_count = debug_parse_bool_option(value, info->props.has_event_write_sample_count);
            continue;
        }
  
        if (strcmp(name, "ubwc_unorm_snorm_int_compatible") == 0) {
            info->props.ubwc_unorm_snorm_int_compatible = debug_parse_bool_option(value, info->props.ubwc_unorm_snorm_int_compatible);
            continue;
        }
  
        if (strcmp(name, "has_64b_ssbo_atomics") == 0) {
            info->props.has_64b_ssbo_atomics = debug_parse_bool_option(value, info->props.has_64b_ssbo_atomics);
            continue;
        }
  
        if (strcmp(name, "has_primitive_shading_rate") == 0) {
            info->props.has_primitive_shading_rate = debug_parse_bool_option(value, info->props.has_primitive_shading_rate);
            continue;
        }
  
        if (strcmp(name, "has_ray_intersection") == 0) {
            info->props.has_ray_intersection = debug_parse_bool_option(value, info->props.has_ray_intersection);
            continue;
        }
  
        if (strcmp(name, "has_hw_bin_scaling") == 0) {
            info->props.has_hw_bin_scaling = debug_parse_bool_option(value, info->props.has_hw_bin_scaling);
            continue;
        }
  
        if (strcmp(name, "has_image_processing") == 0) {
            info->props.has_image_processing = debug_parse_bool_option(value, info->props.has_image_processing);
            continue;
        }
  
        if (strcmp(name, "has_64b_image_atomics") == 0) {
            info->props.has_64b_image_atomics = debug_parse_bool_option(value, info->props.has_64b_image_atomics);
            continue;
        }
  
        if (strcmp(name, "has_implicit_fragface_fragcoord_ij_linear") == 0) {
            info->props.has_implicit_fragface_fragcoord_ij_linear = debug_parse_bool_option(value, info->props.has_implicit_fragface_fragcoord_ij_linear);
            continue;
        }
  
        if (strcmp(name, "load_inline_uniforms_via_preamble_ldgk") == 0) {
            info->props.load_inline_uniforms_via_preamble_ldgk = debug_parse_bool_option(value, info->props.load_inline_uniforms_via_preamble_ldgk);
            continue;
        }
  
        if (strcmp(name, "load_shader_consts_via_preamble") == 0) {
            info->props.load_shader_consts_via_preamble = debug_parse_bool_option(value, info->props.load_shader_consts_via_preamble);
            continue;
        }
  
        if (strcmp(name, "has_gmem_vpc_attr_buf") == 0) {
            info->props.has_gmem_vpc_attr_buf = debug_parse_bool_option(value, info->props.has_gmem_vpc_attr_buf);
            continue;
        }
  
        if (strcmp(name, "sysmem_vpc_attr_buf_size") == 0) {
            info->props.sysmem_vpc_attr_buf_size = debug_parse_num_option(value, info->props.sysmem_vpc_attr_buf_size);
            continue;
        }
  
        if (strcmp(name, "gmem_vpc_attr_buf_size") == 0) {
            info->props.gmem_vpc_attr_buf_size = debug_parse_num_option(value, info->props.gmem_vpc_attr_buf_size);
            continue;
        }
  
        if (strcmp(name, "has_generic_clear") == 0) {
            info->props.has_generic_clear = debug_parse_bool_option(value, info->props.has_generic_clear);
            continue;
        }
  
        if (strcmp(name, "r8g8_faulty_fast_clear_quirk") == 0) {
            info->props.r8g8_faulty_fast_clear_quirk = debug_parse_bool_option(value, info->props.r8g8_faulty_fast_clear_quirk);
            continue;
        }
  
        if (strcmp(name, "gs_vpc_adjacency_quirk") == 0) {
            info->props.gs_vpc_adjacency_quirk = debug_parse_bool_option(value, info->props.gs_vpc_adjacency_quirk);
            continue;
        }
  
        if (strcmp(name, "ubwc_all_formats_compatible") == 0) {
            info->props.ubwc_all_formats_compatible = debug_parse_bool_option(value, info->props.ubwc_all_formats_compatible);
            continue;
        }
  
        if (strcmp(name, "has_compliant_dp4acc") == 0) {
            info->props.has_compliant_dp4acc = debug_parse_bool_option(value, info->props.has_compliant_dp4acc);
            continue;
        }
  
        if (strcmp(name, "ubwc_coherency_quirk") == 0) {
            info->props.ubwc_coherency_quirk = debug_parse_bool_option(value, info->props.ubwc_coherency_quirk);
            continue;
        }
  
        if (strcmp(name, "has_persistent_counter") == 0) {
            info->props.has_persistent_counter = debug_parse_bool_option(value, info->props.has_persistent_counter);
            continue;
        }
  
        if (strcmp(name, "has_sw_fuse") == 0) {
            info->props.has_sw_fuse = debug_parse_bool_option(value, info->props.has_sw_fuse);
            continue;
        }
  
        if (strcmp(name, "has_rt_workaround") == 0) {
            info->props.has_rt_workaround = debug_parse_bool_option(value, info->props.has_rt_workaround);
            continue;
        }
  
        if (strcmp(name, "has_alias_rt") == 0) {
            info->props.has_alias_rt = debug_parse_bool_option(value, info->props.has_alias_rt);
            continue;
        }
  
        if (strcmp(name, "has_abs_bin_mask") == 0) {
            info->props.has_abs_bin_mask = debug_parse_bool_option(value, info->props.has_abs_bin_mask);
            continue;
        }
  
        if (strcmp(name, "new_control_regs") == 0) {
            info->props.new_control_regs = debug_parse_bool_option(value, info->props.new_control_regs);
            continue;
        }
  
        if (strcmp(name, "has_dual_wave_dispatch") == 0) {
            info->props.has_dual_wave_dispatch = debug_parse_bool_option(value, info->props.has_dual_wave_dispatch);
            continue;
        }
  
        if (strcmp(name, "sysmem_vpc_pos_buf_size") == 0) {
            info->props.sysmem_vpc_pos_buf_size = debug_parse_num_option(value, info->props.sysmem_vpc_pos_buf_size);
            continue;
        }
  
        if (strcmp(name, "sysmem_vpc_bv_pos_buf_size") == 0) {
            info->props.sysmem_vpc_bv_pos_buf_size = debug_parse_num_option(value, info->props.sysmem_vpc_bv_pos_buf_size);
            continue;
        }
  
        if (strcmp(name, "sysmem_ccu_color_cache_fraction") == 0) {
            info->props.sysmem_ccu_color_cache_fraction = debug_parse_num_option(value, info->props.sysmem_ccu_color_cache_fraction);
            continue;
        }
  
        if (strcmp(name, "sysmem_ccu_depth_cache_fraction") == 0) {
            info->props.sysmem_ccu_depth_cache_fraction = debug_parse_num_option(value, info->props.sysmem_ccu_depth_cache_fraction);
            continue;
        }
  
        if (strcmp(name, "gmem_vpc_pos_buf_size") == 0) {
            info->props.gmem_vpc_pos_buf_size = debug_parse_num_option(value, info->props.gmem_vpc_pos_buf_size);
            continue;
        }
  
        if (strcmp(name, "gmem_vpc_bv_pos_buf_size") == 0) {
            info->props.gmem_vpc_bv_pos_buf_size = debug_parse_num_option(value, info->props.gmem_vpc_bv_pos_buf_size);
            continue;
        }
  
        if (strcmp(name, "gmem_per_ccu_color_cache_size") == 0) {
            info->props.gmem_per_ccu_color_cache_size = debug_parse_num_option(value, info->props.gmem_per_ccu_color_cache_size);
            continue;
        }
  
        if (strcmp(name, "gmem_per_ccu_depth_cache_size") == 0) {
            info->props.gmem_per_ccu_depth_cache_size = debug_parse_num_option(value, info->props.gmem_per_ccu_depth_cache_size);
            continue;
        }
  
        if (strcmp(name, "has_salu_int_narrowing_quirk") == 0) {
            info->props.has_salu_int_narrowing_quirk = debug_parse_bool_option(value, info->props.has_salu_int_narrowing_quirk);
            continue;
        }
  
        if (strcmp(name, "shading_rate_matches_vk") == 0) {
            info->props.shading_rate_matches_vk = debug_parse_bool_option(value, info->props.shading_rate_matches_vk);
            continue;
        }

        mesa_loge("Invalid feature \"%s\" in FD_DEV_FEATURES", name);
        exit(1);
    }

    free(features);
}

