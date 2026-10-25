/* V3D 7.1 backend driver (C bridge) of the machine v3dcompiler backend library.
 *
 * This is the C half of the on-device compiler: it takes the linked VS+FS NIR
 * produced by the shared C++ GLSL frontend (via v3d_pglc.cpp) and runs mesa's
 * broadcom NIR->VIR->QPU backend (v3d_compiler_init / v3d_compile), returning
 * the packed QPU instruction streams plus the small slice of prog_data the
 * public API currently exposes.
 *
 * WHY THE DEVICE INFO IS HAND-BUILT
 * ---------------------------------
 * The real v3d_get_device_info() (common/v3d_device_info.c) discovers the chip
 * through DRM_IOCTL_V3D_GET_PARAM on the /dev/dri v3d node. Only the g2dd root
 * daemon may open that node - an ordinary app process (where this compiler
 * runs) cannot. So the V3D 7.1 (BCM2712 / VideoCore VII, as in the Pi5) device
 * info is filled in by hand below with exactly the values v3d_get_device_info()
 * would derive from the IDENT registers for ver 71. That source file is
 * deliberately NOT built (see Makefile); this is its compile-time equivalent.
 *
 * Platform-INDEPENDENT compiler code. Upstream pin
 * f45825a0c724d06f231a1210484fe9af71a3ffab. See VENDORED.txt.
 */
#include "v3d_backend.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/ralloc.h"
#include "util/u_memory.h"
#include "util/format/u_format.h"   /* enum pipe_format + PIPE_FORMAT_* */

#include "compiler/v3d_compiler.h"
#include "common/v3d_device_info.h"
#include "common/v3d_limits.h"

/* The v3d_be_stage / public v3d_compiled_stage attribute + varying arrays are
 * sized with the literal 64 (V3DCOMPILER_MAX_VS_INPUTS / _FS_INPUTS). Fail the
 * build if a mesa re-pin ever changes the real limits, so the copies below can
 * never overflow. */
_Static_assert(V3D_MAX_VS_INPUTS == 64, "V3D_MAX_VS_INPUTS != 64: resize v3d_be_stage/v3d_compiled_stage attr arrays");
_Static_assert(V3D_MAX_FS_INPUTS == 64, "V3D_MAX_FS_INPUTS != 64: resize v3d_be_stage/v3d_compiled_stage input_slots");

/* glsl.cpp deliberately never includes v3d_compiler.h (see its header note), so
 * it cannot name QUNIFORM_SSBO_OFFSET itself; export the value so the frontend
 * can remap SSBO_OFFSET slots after v3d_be_compile_compute(). */
uint32_t v3d_be_quniform_ssbo_offset(void)
{
   return (uint32_t)QUNIFORM_SSBO_OFFSET;
}

/* V3D 7.1 device info, mirroring v3d_get_device_info() for ver == 71:
 *   ver            = major*10 + minor = 7*10 + 1
 *   has_accumulators = (ver < 71) -> false for 71
 *   page_size      = OS page size (4096 on EwokOS/Linux aarch64)
 *   max_render_targets / max_framebuffer_size = the V3D_MAX_*(71) limits
 *   clipper_xy_granularity = 64.0f, cle_readahead = 1024  (the ver-71 case)
 *   vpm_size       = (IDENT1 VPM nibble) * 8192. BCM2712 reports nibble 4 =>
 *                    32768. The backend only uses vpm_size for the VCM cache
 *                    sector math (assert vpm_output_batches >= 2, and a
 *                    vcm_cache_size clamped to 2..4), never in the emitted QPU,
 *                    so this value is functionally safe; the g2dd daemon (t7e)
 *                    may re-check it against the live IDENT1 when it programs
 *                    VCM_CACHE_SIZE.
 *   rev/compat_rev = 0: the conservative baseline. rev only gates optional
 *                    newer-hw paths (draw_index at rev>=10, unpack_max0 at
 *                    rev>=7); leaving them off is always correct, just not
 *                    maximally optimal. qpu_count is driver-only (the backend
 *                    never reads it) so it stays 0.
 *
 * Stored as a file-static const: v3d_compiler_init() keeps the POINTER
 * (compiler->devinfo = devinfo), so it must outlive every v3d_compile() call. */
static const struct v3d_device_info v3d71_devinfo = {
   .ver                     = 71,
   .rev                     = 0,
   .compat_rev              = 0,
   .max_perfcnt             = 0,
   .vpm_size                = 32768,
   .qpu_count               = 0,
   .has_accumulators        = false,
   .has_reset_counter       = false,
   .clipper_xy_granularity  = 64.0f,
   .cle_readahead           = 1024,
   .page_size               = 4096,
   .max_framebuffer_size    = V3D_MAX_FRAMEBUFFER_SIZE(71),
   .max_render_targets      = V3D_MAX_RENDER_TARGETS(71),
};

/* The device info every v3d_compiler_init() below is handed.  This backend is
 * built only for the raspi5 machine, whose one chip is the Pi5 / BCM2712
 * VideoCore VII (V3D 7.1), so the compile target is fixed at ver 71 and is
 * never selected at runtime.  (The v3d42_devinfo / v3d_be_set_version() pair
 * this file once carried existed only to serve a shared binary spanning several
 * V3D generations; it has no meaning for a single-chip raspi5 build and has
 * been removed.) */
static const struct v3d_device_info *const v3d_be_devinfo = &v3d71_devinfo;

/* v3d_compile() invokes c->debug_output unconditionally on some paths (e.g. the
 * shader-db dump at the tail of v3d_compile), so a NULL callback would fault.
 * Provide a no-op sink; the GLSL/link diagnostics the caller sees come from the
 * frontend, not from here. */
static void
v3d_be_debug_sink(const char *msg, void *data)
{
   (void)msg;
   (void)data;
}

static void
be_log(char *log, size_t logsz, const char *fmt, ...)
{
   if (!log || logsz == 0)
      return;
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(log, logsz, fmt, ap);
   va_end(ap);
}

void
v3d_be_result_free(struct v3d_be_result *res)
{
   if (!res)
      return;
   free(res->vs.qpu);
   free(res->fs.qpu);
   free(res->vs.uniform_contents);
   free(res->vs.uniform_data);
   free(res->fs.uniform_contents);
   free(res->fs.uniform_data);
   free(res->coord.qpu);
   free(res->coord.uniform_contents);
   free(res->coord.uniform_data);
   free(res->cs.qpu);
   free(res->cs.uniform_contents);
   free(res->cs.uniform_data);
   res->vs.qpu = NULL;
   res->fs.qpu = NULL;
   res->vs.uniform_contents = NULL;
   res->vs.uniform_data = NULL;
   res->fs.uniform_contents = NULL;
   res->fs.uniform_data = NULL;
   res->coord.qpu = NULL;
   res->coord.uniform_contents = NULL;
   res->coord.uniform_data = NULL;
   res->cs.qpu = NULL;
   res->cs.uniform_contents = NULL;
   res->cs.uniform_data = NULL;
}

/* Deep-copy a stage's uniform stream out of the ralloc'd prog_data into two
 * malloc()ed u32 arrays (the prog_data is freed before v3d_be_compile_program
 * returns). contents[] holds the enum quniform_contents values, data[] the
 * 32-bit arguments. Returns true on success (including the count == 0 no-op);
 * on allocation failure frees any partial copy and returns false. */
static bool
be_copy_uniforms(const struct v3d_prog_data *pd, struct v3d_be_stage *st)
{
   uint32_t n = pd->uniforms.count;

   st->num_uniforms     = n;
   st->uniform_contents = NULL;
   st->uniform_data     = NULL;
   if (n == 0)
      return true;

   st->uniform_contents = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
   st->uniform_data     = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
   if (!st->uniform_contents || !st->uniform_data) {
      free(st->uniform_contents);
      free(st->uniform_data);
      st->uniform_contents = NULL;
      st->uniform_data     = NULL;
      st->num_uniforms     = 0;
      return false;
   }
   for (uint32_t i = 0; i < n; i++)
      st->uniform_contents[i] = (uint32_t)pd->uniforms.contents[i];
   memcpy(st->uniform_data, pd->uniforms.data, (size_t)n * sizeof(uint32_t));
   return true;
}

bool
v3d_be_compile_program(nir_shader *vs, nir_shader *fs,
                       struct v3d_be_result *res,
                       char *log, size_t logsz)
{
   if (!res)
      return false;
   memset(res, 0, sizeof(*res));

   if (!vs || !fs) {
      be_log(log, logsz, "v3d backend: a vertex AND fragment shader are both "
             "required (vs=%p fs=%p)", (void *)vs, (void *)fs);
      return false;
   }

   const struct v3d_compiler *compiler = v3d_compiler_init(v3d_be_devinfo, 0);
   if (!compiler) {
      be_log(log, logsz, "v3d backend: v3d_compiler_init failed");
      return false;
   }

   bool ok = false;
   struct v3d_prog_data *fs_prog_data = NULL;
   struct v3d_prog_data *vs_prog_data = NULL;
   struct v3d_prog_data *coord_prog_data = NULL;
   uint64_t *fs_qpu = NULL;
   uint64_t *vs_qpu = NULL;
   uint64_t *coord_qpu = NULL;

   /* --- Fragment stage first -------------------------------------------
    * The FS key is the single-RT, no-blend, no-logic-op, RGBA8 default that
    * fits the shadertoy fullscreen-quad draw:
    *   cbufs = 0x1            -> RT0 present; without this bit the backend
    *                             emits no TLB colour write at all.
    *   logicop_func = COPY    -> v3d_nir_lower_logic_ops() early-returns.
    *   software_blend = false -> v3d_nir_lower_blend() early-returns, so the
    *                             blend[]/color_fmt[] state is never read for
    *                             codegen (color_fmt is still set to a valid
    *                             RGBA8 for safety).
    *   sampler_is_32b = ~0    -> force 32-bit TMU returns for EVERY sampler.
    *                             The 16-bit packing path (bit clear) makes
    *                             nir_lower_tex emit nir_extract_bits(...,16),
    *                             i.e. unpack_32_2x16_split_x/y, and this
    *                             backend's ntq_emit_alu has NO case for those
    *                             (it only handles pack_half_2x16_split ->
    *                             VFPACK); lower_bit_size_cb won't widen them
    *                             either because their source is already 32-bit,
    *                             so any texture sample abort()s the compiler.
    *                             Setting the bit makes v3d_vir_emit_tex program
    *                             return_words_of_texture_data = components & 0xf
    *                             (one fp32 word per channel, v3d_tex.c:251) and
    *                             lower_tex_packing_cb return packing_none, so no
    *                             pack/unpack is generated.  This is the only
    *                             texture-return mode the vendored backend can
    *                             codegen; unused sampler bits are never read.
    *                             NOTE: the TMU config word this bakes into the
    *                             QPU stream must match how g2dd sets up the
    *                             texture at draw time (device-verified).
    * These are the state-dependent fields mesa would normally key FS *variants*
    * on; for the single fixed shadertoy pipeline one default variant suffices.
    */
   struct v3d_fs_key fs_key;
   memset(&fs_key, 0, sizeof(fs_key));
   fs_key.cbufs = 0x1;
   fs_key.logicop_func = PIPE_LOGICOP_COPY;
   fs_key.software_blend = false;
   /* See the sampler_is_32b note above: the vendored backend cannot codegen the
    * 16-bit texture-return path (unpack_32_2x16_split), so every sampler must
    * use 32-bit TMU returns.  Bits for samplers the FS never references are
    * never read by lower_tex_packing_cb / v3d_vir_emit_tex. */
   fs_key.base.sampler_is_32b = ~0u;
   fs_key.color_fmt[0].format = PIPE_FORMAT_R8G8B8A8_UNORM;
   fs_key.color_fmt[0].swizzle[0] = 0;
   fs_key.color_fmt[0].swizzle[1] = 1;
   fs_key.color_fmt[0].swizzle[2] = 2;
   fs_key.color_fmt[0].swizzle[3] = 3;

   uint32_t fs_asm_size = 0;
   fs_qpu = v3d_compile(compiler, &fs_key.base, &fs_prog_data, fs,
                        v3d_be_debug_sink, NULL, 0 /*program_id*/,
                        0 /*variant_id*/, &fs_asm_size);
   if (!fs_qpu) {
      be_log(log, logsz, "v3d backend: fragment shader QPU generation failed");
      goto done;
   }

   struct v3d_fs_prog_data *fs_pd = (struct v3d_fs_prog_data *)fs_prog_data;

   /* --- Vertex stage ----------------------------------------------------
    * used_outputs MUST equal the FS input_slots (same order) so the VS VPM
    * varying offsets line up with what the FS LDVARY reads. gl_Position
    * (VARYING_SLOT_POS) and point-coord/primitive-id are carried by the
    * fixed-function VPM header, not through used_outputs, so the FS
    * input_slots list (which already excludes them) is copied verbatim. */
   struct v3d_vs_key vs_key;
   memset(&vs_key, 0, sizeof(vs_key));
   vs_key.base.is_last_geometry_stage = true; /* no GS/TES in this pipeline */
   vs_key.is_coord = false;
   vs_key.per_vertex_point_size = false;
   vs_key.clamp_color = false;

   uint8_t num_used = fs_pd->num_inputs;
   if (num_used > ARRAY_SIZE(vs_key.used_outputs))
      num_used = ARRAY_SIZE(vs_key.used_outputs);
   for (uint8_t i = 0; i < num_used; i++)
      vs_key.used_outputs[i] = fs_pd->input_slots[i];
   vs_key.num_used_outputs = num_used;

   uint32_t vs_asm_size = 0;
   vs_qpu = v3d_compile(compiler, &vs_key.base, &vs_prog_data, vs,
                        v3d_be_debug_sink, NULL, 0 /*program_id*/,
                        0 /*variant_id*/, &vs_asm_size);
   if (!vs_qpu) {
      be_log(log, logsz, "v3d backend: vertex shader QPU generation failed");
      free(fs_qpu);
      fs_qpu = NULL;
      goto done;
   }

   /* --- Publish the results -------------------------------------------- */
   res->fs.qpu             = fs_qpu;
   res->fs.qpu_instr_count = fs_asm_size / (uint32_t)sizeof(uint64_t);
   res->fs.num_outputs     = 1; /* single colour RT (cbufs == 0x1) */
   res->fs.needs_coords    = fs_pd->uses_center_w ||
      BITSET_TEST(fs->info.system_values_read, SYSTEM_VALUE_FRAG_COORD);
   res->fs.threads         = fs_pd->base.threads;
   res->fs.single_seg      = fs_pd->base.single_seg;
   /* FS varying-input surface (num_inputs / input_slots) and the depth /
    * pixel-centre flags the shader-state record reads. */
   res->fs.num_inputs      = fs_pd->num_inputs;
   for (uint8_t i = 0; i < fs_pd->num_inputs && i < ARRAY_SIZE(res->fs.input_slots); i++)
      res->fs.input_slots[i] = (uint32_t)fs_pd->input_slots[i].slot_and_component;
   res->fs.uses_center_w   = fs_pd->uses_center_w;
   res->fs.writes_z        = fs_pd->writes_z;
   res->fs.disable_ez      = fs_pd->disable_ez;
   res->fs.uses_implicit_point_line_varyings =
      fs_pd->uses_implicit_point_line_varyings;
   if (!be_copy_uniforms(&fs_pd->base, &res->fs)) {
      be_log(log, logsz, "v3d backend: out of memory copying FS uniforms");
      /* res->fs.qpu already owns fs_qpu; the caller releases it through
       * v3d_be_result_free(), so just drop the local alias. */
      fs_qpu = NULL;
      goto done;
   }

   struct v3d_vs_prog_data *vs_pd = (struct v3d_vs_prog_data *)vs_prog_data;
   res->vs.qpu             = vs_qpu;
   res->vs.qpu_instr_count = vs_asm_size / (uint32_t)sizeof(uint64_t);
   res->vs.num_outputs     = num_used; /* varyings written for the FS */
   res->vs.needs_coords    = false;    /* VS reads attributes, not frag coord */
   res->vs.threads         = vs_pd->base.threads;
   res->vs.single_seg      = vs_pd->base.single_seg;
   /* VS VPM layout + attribute map for the shader/attribute records. */
   res->vs.vpm_input_size  = vs_pd->vpm_input_size;
   res->vs.vpm_output_size = vs_pd->vpm_output_size;
   res->vs.separate_segments = vs_pd->separate_segments;
   res->vs.vcm_cache_size  = vs_pd->vcm_cache_size;
   memcpy(res->vs.vattr_sizes, vs_pd->vattr_sizes, sizeof(res->vs.vattr_sizes));
   memcpy(res->vs.driver_location_map, vs_pd->driver_location_map,
          sizeof(res->vs.driver_location_map));
   if (!be_copy_uniforms(&vs_pd->base, &res->vs)) {
      be_log(log, logsz, "v3d backend: out of memory copying VS uniforms");
      /* res->fs.qpu / res->vs.qpu already own the buffers; the caller releases
       * them through v3d_be_result_free(), so just drop the local aliases. */
      fs_qpu = NULL;
      vs_qpu = NULL;
      goto done;
   }

   /* --- Coordinate shader (binning) variant ----------------------------
    * The V3D binner runs a coordinate shader to compute each vertex's
    * clip-space position for tile binning, separately from the render VS.
    * mesa emits it as the SAME vertex NIR compiled with v3d_vs_key.is_coord
    * = true (only gl_Position is written; varyings are dropped -- see the
    * is_coord paths in v3d_nir_lower_io.c / nir_to_vir.c / vir.c).
    * Arbitrary-geometry draws (the teapot mesh) REQUIRE this: the fixed
    * pass-through coord shader in v3d_3d.c applies no model/view/projection
    * transform, so it can only bin pre-transformed full-screen-quad positions.
    * v3d_compile clones the NIR, so `vs` is unchanged and the coord variant
    * shares the VS's uniform driver_locations -- the QUNIFORM_UNIFORM dword
    * indices match the VS reflection, letting one per-draw blob feed both the
    * coord and render-VS uniform expansion. */
   struct v3d_vs_key coord_key;
   memcpy(&coord_key, &vs_key, sizeof(coord_key));
   coord_key.is_coord = true;
   coord_key.num_used_outputs = 0;   /* a coord shader writes no varyings */

   uint32_t coord_asm_size = 0;
   coord_qpu = v3d_compile(compiler, &coord_key.base, &coord_prog_data, vs,
                           v3d_be_debug_sink, NULL, 0 /*program_id*/,
                           0 /*variant_id*/, &coord_asm_size);
   if (!coord_qpu) {
      be_log(log, logsz, "v3d backend: coordinate shader QPU generation failed");
      /* res->fs.qpu / res->vs.qpu already own their buffers; drop the aliases
       * so done: does not double-free what v3d_be_result_free() will release. */
      fs_qpu = NULL;
      vs_qpu = NULL;
      goto done;
   }

   struct v3d_vs_prog_data *coord_pd =
      (struct v3d_vs_prog_data *)coord_prog_data;
   res->coord.qpu             = coord_qpu;
   res->coord.qpu_instr_count = coord_asm_size / (uint32_t)sizeof(uint64_t);
   res->coord.num_outputs     = 0;    /* coord shader writes only gl_Position */
   res->coord.needs_coords    = false;
   res->coord.threads         = coord_pd->base.threads;
   res->coord.single_seg      = coord_pd->base.single_seg;
   /* The coord shader's own VPM segment sizes: the shader-state record packs
    * these (not the render VS's) for the coordinate stage. */
   res->coord.vpm_input_size    = coord_pd->vpm_input_size;
   res->coord.vpm_output_size   = coord_pd->vpm_output_size;
   res->coord.separate_segments = coord_pd->separate_segments;
   res->coord.vcm_cache_size    = coord_pd->vcm_cache_size;
   if (!be_copy_uniforms(&coord_pd->base, &res->coord)) {
      be_log(log, logsz, "v3d backend: out of memory copying coord uniforms");
      fs_qpu = NULL;
      vs_qpu = NULL;
      coord_qpu = NULL;   /* res->coord.qpu owns it now */
      goto done;
   }

   /* Ownership of fs_qpu/vs_qpu/coord_qpu transfers to res on success. */
   fs_qpu = NULL;
   vs_qpu = NULL;
   coord_qpu = NULL;
   res->success = true;
   ok = true;

done:
   if (fs_qpu)
      free(fs_qpu);
   if (vs_qpu)
      free(vs_qpu);
   if (coord_qpu)
      free(coord_qpu);
   /* prog_data is rzalloc'd with a NULL parent by v3d_compile; free it here
    * after copying out the fields we need. */
   if (fs_prog_data)
      ralloc_free(fs_prog_data);
   if (vs_prog_data)
      ralloc_free(vs_prog_data);
   if (coord_prog_data)
      ralloc_free(coord_prog_data);
   v3d_compiler_free(compiler);
   return ok;
}

bool
v3d_be_compile_compute(nir_shader *cs,
                       struct v3d_be_result *res,
                       char *log, size_t logsz)
{
   if (!res)
      return false;
   memset(res, 0, sizeof(*res));

   if (!cs || cs->info.stage != MESA_SHADER_COMPUTE) {
      be_log(log, logsz, "v3d backend: a compute shader is required (cs=%p)",
             (void *)cs);
      return false;
   }

   const struct v3d_compiler *compiler = v3d_compiler_init(v3d_be_devinfo, 0);
   if (!compiler) {
      be_log(log, logsz, "v3d backend: v3d_compiler_init failed");
      return false;
   }

   bool ok = false;
   struct v3d_prog_data *cs_prog_data = NULL;
   uint64_t *cs_qpu = NULL;

   /* --- Compute stage ---------------------------------------------------
    * The compute key is the bare v3d_key: no colour/varying/viewport state
    * applies (v3d_compile() sizes prog_data from s->info.stage, and the
    * MESA_SHADER_COMPUTE paths in nir_to_vir.c read only the base key).
    * sampler_is_32b = ~0 for the same reason as the fragment path: the
    * vendored backend can only codegen the 32-bit TMU-return mode (see the
    * note in v3d_be_compile_program).  Per-thread workgroup / local ids are
    * not uniforms at all -- nir_to_vir.c loads them from the CSD payload
    * registers (r3 / r2 on ver >= 71), so nothing here depends on the
    * dispatch grid; the CSD dispatcher supplies QUNIFORM_NUM_WORK_GROUPS /
    * WORK_GROUP_SIZE / SHARED_* at dispatch time through the uniform stream. */
   struct v3d_key cs_key;
   memset(&cs_key, 0, sizeof(cs_key));
   cs_key.sampler_is_32b = ~0u;

   /* Pre-lower the compute system values here, with explicit options.
    * v3d_compile() (vir.c) runs nir_lower_system_values followed by
    * nir_lower_compute_system_values(.., NULL); the first rewrites
    * gl_GlobalInvocationID as load_global_invocation_id +
    * load_base_global_invocation_id, and with NULL options the second leaves
    * load_base_global_invocation_id in place (it is only folded to zero when
    * options say the hardware has no base id).  nir_to_vir.c has no case for
    * that intrinsic and aborts ("Unknown intrinsic").  Upstream drivers lower
    * this in the frontend before handing the shader to v3d_compile; do the
    * same: no dispatch base offsets, and local ids derived from the
    * local_invocation_index the CSD payload carries (r2 on ver >= 71). */
   {
      nir_lower_compute_system_values_options cs_sysval_opts;
      memset(&cs_sysval_opts, 0, sizeof(cs_sysval_opts));
      cs_sysval_opts.has_base_global_invocation_id = false;
      cs_sysval_opts.has_base_workgroup_id = false;
      cs_sysval_opts.lower_cs_local_id_to_index = true;
      /* Run the passes directly rather than through the NIR_PASS() macro.  That
       * macro wraps every pass in a should_print_nir() debug path calling
       * printf() / nir_print_shader(nir, stdout), and `stdout` expands to
       * newlib's _impure_ptr->_stdout.  _impure_ptr is defined by NONE of the
       * EwokOS libc/gloss/ewoksys archives, and this backend is merged into
       * libGLESv2.a and linked into opengl APPS (which pull no BSP), so keeping
       * that reference fails every app link with "undefined reference to
       * `_impure_ptr'".  The print path is dead at runtime (the
       * nir_debug_print_shader[] flags default all-false) and NIR_PASS' progress
       * result is discarded here anyway, so the direct calls are codegen-
       * identical minus the un-linkable print/validate scaffolding. */
      (void)nir_lower_system_values(cs);
      (void)nir_lower_compute_system_values(cs, &cs_sysval_opts);
      (void)nir_opt_constant_folding(cs);
      (void)nir_opt_dce(cs);
   }

   uint32_t cs_asm_size = 0;
   cs_qpu = v3d_compile(compiler, &cs_key, &cs_prog_data, cs,
                        v3d_be_debug_sink, NULL, 0 /*program_id*/,
                        0 /*variant_id*/, &cs_asm_size);
   if (!cs_qpu) {
      be_log(log, logsz, "v3d backend: compute shader QPU generation failed");
      goto done;
   }

   struct v3d_compute_prog_data *cs_pd =
      (struct v3d_compute_prog_data *)cs_prog_data;
   res->cs.qpu             = cs_qpu;
   res->cs.qpu_instr_count = cs_asm_size / (uint32_t)sizeof(uint64_t);
   res->cs.num_outputs     = 0;      /* colour goes to memory, not the TLB */
   res->cs.needs_coords    = false;
   res->cs.threads         = cs_pd->base.threads;
   res->cs.single_seg      = cs_pd->base.single_seg;
   res->cs.local_size[0]   = cs_pd->local_size[0];
   res->cs.local_size[1]   = cs_pd->local_size[1];
   res->cs.local_size[2]   = cs_pd->local_size[2];
   res->cs.shared_size     = cs_pd->shared_size;
   res->cs.spill_size      = cs_pd->base.spill_size;   /* register-spill scratch bytes/thread */
   if (!be_copy_uniforms(&cs_pd->base, &res->cs)) {
      be_log(log, logsz, "v3d backend: out of memory copying CS uniforms");
      cs_qpu = NULL;   /* res->cs.qpu owns it; v3d_be_result_free releases */
      goto done;
   }

   cs_qpu = NULL;   /* ownership transferred to res */
   res->success = true;
   ok = true;

done:
   if (cs_qpu)
      free(cs_qpu);
   if (cs_prog_data)
      ralloc_free(cs_prog_data);
   v3d_compiler_free(compiler);
   return ok;
}
