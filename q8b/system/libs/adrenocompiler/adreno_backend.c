/*
 * adreno_backend.c - the NIR -> a6xx half of the q8b pglcompiler backend:
 * drives mesa's freedreno/ir3 compiler on the linked compute NIR the shared
 * frontend produced and snapshots the resulting ir3_shader_variant into the
 * fields adreno_prog_blob_t carries (see <adreno/adreno_prog.h> for the
 * field-by-field mapping and the UBO-0 / UAV contracts restated below).
 *
 * TARGET: Adreno 690 (SC8280XP, fd_dev_id {690, 0x060900ff}) - the single
 * GPU of the q8b machine, so the compiler is created once for that id and
 * never re-selected (pglc_set_version() is a no-op, like raspi5's V3D 7.1).
 *
 * WHAT THE DRIVER NORMALLY DOES THAT WE MUST DO HERE: ir3 is a backend; the
 * gallium freedreno / turnip drivers run a set of NIR lowerings before handing
 * it a shader.  The frontend (pglcompiler_frontend.cpp) covers the GL state
 * tracker part (uniform -> load_uniform with BYTE offsets, SSBO derefs ->
 * load/store_ssbo on the LINKED block index, samplers -> indices); this file
 * covers the rest for a compute kernel:
 *   - nir_lower_system_values + nir_lower_compute_system_values (ir3 reads
 *     local_invocation_id / workgroup_id natively and turns num_workgroups
 *     into a driver param; global_invocation_id is composed from them),
 *   - shared variables -> explicit 32-bit offsets (load/store_shared),
 *   - load_uniform -> load_ubo on UBO 0 with the same byte offset and range.
 *     The per-draw uniform blob IS UBO 0 (see adreno_prog.h): ir3's
 *     ir3_nir_analyze_ubo_ranges then pushes whatever ranges fit into the
 *     constant file (ubo_range[] below) and leaves the rest as ldc from the
 *     UBO-0 descriptor (num_ubos > 0).  nir_lower_uniforms_to_ubo is NOT
 *     used: it rescales the offsets by the dword multiplier, which would
 *     break the byte driver_locations the frontend's reflection reports.
 *
 * OWNERSHIP: ir3_shader_from_nir() takes the nir, ir3_finalize_nir() strips
 * the uniform variables from it and nir_sweep()s - so everything runs on a
 * nir_shader_clone() of the caller's NIR, which stays intact for
 * pglc_fe_build_uniform_bindings().  The variant is created with a NULL
 * mem_ctx, so it is not on shader->variants and is freed explicitly.
 *
 * NOTHING HERE IS HARDWARE-VALIDATED ON SC8280XP (QEMU models no Adreno);
 * the emitted a6xx binary is the same ir3 output turnip/gallium ship, but the
 * daemon-side dispatch (adreno_3d.c) that consumes these fields has only been
 * desk-checked against turnip's tu6_emit_cs_config / tu6_emit_xs path.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "compiler/glsl_types.h"

#include "freedreno_dev_info.h"
#include "ir3/ir3_compiler.h"
#include "ir3/ir3_shader.h"
#include "ir3/ir3_nir.h"

#include "adreno_backend.h"

/* adreno_prog.h's "no driver params" marker, mirrored so this TU does not
 * depend on the SDK staging order (adreno_blob.c is the one that includes
 * the daemon contract header). */
#define BE_NO_DRIVER_PARAMS 0xffffffffu

/* ---------------------------------------------------------------------------
 * Compiler singleton
 * ------------------------------------------------------------------------- */

/* ir3_compiler_create() keeps the dev_id POINTER (compiler->dev_id), so the
 * id must outlive the compiler: static. */
static const struct fd_dev_id s_a690_id = { .gpu_id = 690, .chip_id = 0x060900ffull };
static struct ir3_compiler *s_compiler;

static struct ir3_compiler *
get_compiler(void)
{
   if (s_compiler)
      return s_compiler;

   const struct fd_dev_info *info = fd_dev_info_raw(&s_a690_id);
   if (!info)
      return NULL;

   struct ir3_compiler_options opts;
   memset(&opts, 0, sizeof(opts));
   /* no util/disk_cache on device (ir3_disk_cache_* are stubs) */
   opts.disable_cache = true;
   /* GL-style: UBO ranges pushed by the driver per draw, no preamble upload */
   opts.push_ubo_with_preamble = false;
   /* no bindless framebuffer-read descriptor (turnip-only feature) */
   opts.bindless_fb_read_descriptor = -1;
   opts.bindless_fb_read_slot = -1;

   s_compiler = ir3_compiler_create(NULL, &s_a690_id, info, &opts);
   return s_compiler;
}

const struct nir_shader_compiler_options *
adreno_be_nir_options(void)
{
   struct ir3_compiler *c = get_compiler();
   return c ? ir3_get_compiler_options(c) : NULL;
}

/* ---------------------------------------------------------------------------
 * load_uniform -> load_ubo(0)
 * ------------------------------------------------------------------------- */

/* The frontend's nir_lower_io(nir_var_uniform, type_size_uniform_bytes) left
 * every default-block uniform read as
 *   load_uniform(offset) base=B range=R   (all in BYTES)
 * Rewrite it to
 *   load_ubo(block=0, B + offset) range_base=B range=R align_mul=4
 * so ir3 sees an ordinary GL UBO 0 whose bytes are the per-draw uniform blob
 * (dword index == byte / 4, exactly the pglc_uniform_binding reflection).
 *
 * ACCESS_CAN_SPECULATE: UBO 0 is always bound and covers every reflected
 * range, so hoisting a read out of control flow is safe - and it is also
 * REQUIRED here.  ir3_nir_lower_ubo_loads only looks at "prefetchable" loads
 * (top-level block or CAN_SPECULATE, ir3_nir_is_prefetchable) and, because we
 * run with push_ubo_with_preamble = false (the daemon pushes ubo_range[]),
 * it rewrites info.num_ubos from the loads it looked at.  An access=none
 * load inside an `if` would be skipped, num_ubos would drop to 0 and
 * nir_lower_amul asserts `idx < num_ubos` (caught by the host oracle).  ir3's
 * own driver-UBO loads (ir3_nir.c load_driver_ubo) set the same flag. */
static bool
lower_uniform_to_ubo0(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   (void)data;
   if (intr->intrinsic != nir_intrinsic_load_uniform)
      return false;

   b->cursor = nir_before_instr(&intr->instr);

   unsigned base = nir_intrinsic_base(intr);
   unsigned range = nir_intrinsic_range(intr);
   nir_def *off = nir_iadd_imm(b, intr->src[0].ssa, base);
   nir_def *ld = nir_load_ubo(b, intr->def.num_components, intr->def.bit_size,
                              nir_imm_int(b, 0), off,
                              .access = ACCESS_CAN_SPECULATE,
                              .align_mul = 4,
                              .align_offset = 0,
                              .range_base = base,
                              .range = range);
   nir_def_replace(&intr->def, ld);
   return true;
}

/* ---------------------------------------------------------------------------
 * Variant snapshot
 * ------------------------------------------------------------------------- */

static bool
copy_u32s(uint32_t **dst, const void *src, size_t bytes)
{
   *dst = NULL;
   if (bytes == 0)
      return true;
   *dst = (uint32_t *)malloc(bytes);
   if (!*dst)
      return false;
   memcpy(*dst, src, bytes);
   return true;
}

/* Fill res from a compiled variant.  Rejects (false + log) the variant
 * shapes the daemon contract does not carry rather than silently mis-
 * describing them. */
static bool
snapshot_variant(struct ir3_shader_variant *v, struct adreno_be_result *res,
                 char *log, size_t logsz)
{
   const struct ir3_const_state *cs = ir3_const_state(v);
   struct adreno_pglc_stage *st = &res->st;

   if (v->local_size_variable) {
      snprintf(log, logsz, "adreno: variable workgroup size is not supported "
               "(the blob carries a fixed local_size)");
      return false;
   }
   if (v->binning || v->info.size == 0 || !v->bin) {
      snprintf(log, logsz, "adreno: ir3 produced no binary");
      return false;
   }
   if (cs->driver_params_ubo.idx != -1) {
      snprintf(log, logsz, "adreno: ir3 placed driver params in a UBO (idx %d); "
               "the daemon only fills the const-file slot", cs->driver_params_ubo.idx);
      return false;
   }
   if (cs->ubo_state.num_enabled > ADRENO_PGLC_MAX_UBO_RANGES) {
      snprintf(log, logsz, "adreno: %u pushed UBO ranges exceed the blob's %u",
               cs->ubo_state.num_enabled, ADRENO_PGLC_MAX_UBO_RANGES);
      return false;
   }
   if (v->num_ssbos > ADRENO_PGLC_MAX_SSBO) {
      snprintf(log, logsz, "adreno: %u SSBOs exceed the blob's %u",
               v->num_ssbos, ADRENO_PGLC_MAX_SSBO);
      return false;
   }
   if (v->num_ssbos != ir3_shader_num_uavs(v)) {
      snprintf(log, logsz, "adreno: image UAVs are not supported (num_uavs %u, "
               "num_ssbos %u)", ir3_shader_num_uavs(v), v->num_ssbos);
      return false;
   }

   /* binary */
   res->bin_words = v->info.size / 4u;
   if (!copy_u32s(&res->bin, v->bin, (size_t)res->bin_words * 4u)) {
      snprintf(log, logsz, "adreno: out of memory copying the a6xx binary");
      return false;
   }

   st->instrlen      = v->instrlen;
   st->constlen      = v->constlen;
   st->shared_size   = v->shared_size;
   st->local_size[0] = v->local_size[0];
   st->local_size[1] = v->local_size[1];
   st->local_size[2] = v->local_size[2];

   st->max_reg           = (uint8_t)v->info.max_reg;
   st->max_half_reg      = (uint8_t)v->info.max_half_reg;
   st->double_threadsize = v->info.double_threadsize;
   st->mergedregs        = v->mergedregs;
   st->branchstack_hw    = ir3_shader_branchstack_hw(v);
   st->early_preamble    = v->early_preamble;
   st->round_robin_mode  = v->cs.round_robin_mode;

   st->pvtmem_size     = v->pvtmem_size;
   st->pvtmem_per_wave = v->pvtmem_per_wave;

   /* immediates: turnip pushes MIN2(DIV_ROUND_UP(count,4) + base, constlen) -
    * base vec4s at base; the daemon applies the same clamp. */
   st->imm_count = v->imm_state.count;
   st->imm_base  = cs->allocs.max_const_offset_vec4;
   if (!copy_u32s(&st->imm_values, v->imm_state.values,
                  (size_t)st->imm_count * 4u)) {
      snprintf(log, logsz, "adreno: out of memory copying immediates");
      return false;
   }

   /* constant data (appended to the binary BO by the daemon) */
   st->constant_data_size = v->constant_data_size;
   st->constant_data_off  = v->info.constant_data_offset;
   st->consts_ubo_idx     = cs->consts_ubo.idx;
   if (!copy_u32s(&st->constant_data, v->constant_data, st->constant_data_size)) {
      snprintf(log, logsz, "adreno: out of memory copying constant data");
      return false;
   }

   /* UBO 0 (per-draw uniform blob) */
   st->num_ubos = cs->num_ubos;
   st->num_ubo_ranges = 0;
   for (uint32_t i = 0; i < cs->ubo_state.num_enabled; i++) {
      const struct ir3_ubo_range *r = &cs->ubo_state.range[i];
      if (r->ubo.bindless || r->ubo.global || r->ubo.block != 0) {
         snprintf(log, logsz, "adreno: pushed UBO range %u is not UBO 0 "
                  "(block %u bindless %d global %d)", i, r->ubo.block,
                  r->ubo.bindless, r->ubo.global);
         return false;
      }
      struct adreno_pglc_ubo_range *o = &st->ubo_range[st->num_ubo_ranges++];
      o->src_byte   = r->start;
      o->dst_vec4   = r->offset / 16u;
      o->size_bytes = r->end - r->start;
   }

   /* driver params */
   if (ir3_const_can_upload(&cs->allocs, IR3_CONST_ALLOC_DRIVER_PARAMS, v->constlen)) {
      st->driver_params_vec4_off =
         cs->allocs.consts[IR3_CONST_ALLOC_DRIVER_PARAMS].offset_vec4;
      st->driver_params_dwords = cs->num_driver_params;
   } else {
      st->driver_params_vec4_off = BE_NO_DRIVER_PARAMS;
      st->driver_params_dwords = 0;
   }

   st->local_invocation_id_regid =
      ir3_find_sysval_regid(v, SYSTEM_VALUE_LOCAL_INVOCATION_ID);
   st->work_group_id_regid =
      ir3_find_sysval_regid(v, SYSTEM_VALUE_WORKGROUP_ID);

   /* UAV slot i == linked SSBO block i; the binding remap is applied by the
    * caller (adreno_pglc.cpp) which owns the frontend's block table. */
   st->num_ssbo = v->num_ssbos;
   for (uint32_t i = 0; i < st->num_ssbo; i++)
      st->ssbo_binding[i] = i;

   return true;
}

/* ---------------------------------------------------------------------------
 * Compile
 * ------------------------------------------------------------------------- */

bool
adreno_be_compile_compute(nir_shader *cs_in, struct adreno_be_result *res,
                          char *log, size_t logsz)
{
   struct ir3_compiler *compiler;
   struct ir3_shader *shader = NULL;
   struct ir3_shader_variant *v = NULL;
   nir_shader *nir;
   bool ok = false;

   memset(res, 0, sizeof(*res));
   if (logsz)
      log[0] = '\0';

   compiler = get_compiler();
   if (!compiler) {
      snprintf(log, logsz, "adreno: ir3_compiler_create failed for A690");
      return false;
   }
   if (!cs_in || cs_in->info.stage != MESA_SHADER_COMPUTE) {
      snprintf(log, logsz, "adreno: compute NIR required");
      return false;
   }

   nir = nir_shader_clone(NULL, cs_in);
   if (!nir) {
      snprintf(log, logsz, "adreno: out of memory cloning NIR");
      return false;
   }

   /* --- driver-side pre-lowering (what gallium freedreno's state tracker
    *     does before ir3 sees a compute shader) --- */
   {
      nir_lower_compute_system_values_options cs_opts;
      memset(&cs_opts, 0, sizeof(cs_opts));
      /* no base_* ids: glDispatchCompute only, workgroup ids start at 0 */
      (void)nir_lower_system_values(nir);
      (void)nir_lower_compute_system_values(nir, &cs_opts);
   }
   (void)nir_lower_vars_to_explicit_types(nir, nir_var_mem_shared,
                                          glsl_get_natural_size_align_bytes);
   (void)nir_lower_explicit_io(nir, nir_var_mem_shared,
                               nir_address_format_32bit_offset);

   (void)nir_shader_intrinsics_pass(nir, lower_uniform_to_ubo0,
                                    nir_metadata_control_flow, NULL);
   /* UBO 0 is the (GL-style) default uniform block: ir3_nir_lower_ubo_loads
    * keys its num_ubos bookkeeping off this flag. */
   nir->info.num_ubos = 1;
   nir->info.first_ubo_is_default_ubo = true;

   (void)nir_opt_constant_folding(nir);
   (void)nir_opt_dce(nir);

   /* --- ir3 --- */
   struct ir3_shader_options sopts;
   memset(&sopts, 0, sizeof(sopts));
   sopts.api_wavesize  = IR3_SINGLE_OR_DOUBLE;
   sopts.real_wavesize = IR3_SINGLE_OR_DOUBLE;
   sopts.push_consts_type = IR3_PUSH_CONSTS_NONE;

   ir3_finalize_nir(compiler, &sopts.nir_options, nir);

   shader = ir3_shader_from_nir(compiler, nir, &sopts);   /* owns nir now */
   nir = NULL;
   if (!shader) {
      snprintf(log, logsz, "adreno: ir3_shader_from_nir failed");
      goto out;
   }

   struct ir3_shader_key key;
   memset(&key, 0, sizeof(key));
   v = ir3_shader_create_variant(shader, &key, false);
   if (!v) {
      snprintf(log, logsz, "adreno: ir3 compile failed (see mesa log)");
      goto out;
   }

   ok = snapshot_variant(v, res, log, logsz);

out:
   if (v)
      ralloc_free(v);          /* NULL mem_ctx variant: not on shader->variants */
   if (shader)
      ir3_shader_destroy(shader);
   if (nir)
      ralloc_free(nir);
   if (!ok)
      adreno_be_result_free(res);
   return ok;
}

void
adreno_be_result_free(struct adreno_be_result *res)
{
   if (!res)
      return;
   free(res->bin);
   free(res->st.imm_values);
   free(res->st.constant_data);
   memset(res, 0, sizeof(*res));
}
