/*
 * adreno_stubs.c - link stubs for the ir3 tooling TUs the device build leaves
 * out (see IR3_EXCLUDE in the Makefile).  Every function here is reached only
 * from a debug/developer path that the EwokOS app-side compiler never takes;
 * each stub is the "feature absent" answer that path already handles:
 *
 *   ir3_disk_cache_*         ir3_disk_cache.c needs util/disk_cache (zlib /
 *                            zstd / a writable cache dir).  retrieve() -> false
 *                            means "cache miss, compile"; the other three are
 *                            no-ops, exactly what upstream does when
 *                            compiler->disk_cache is NULL.
 *   ir3_shader_bisect_*      the IR3_SHADER_BISECT developer knob; "no bisect
 *                            configured" (init no-op, every select false).
 *   ir3_parse                the textual ir3 assembler (bison/flex ir3_parser /
 *                            ir3_lexer), reached only from IR3_SHADER_OVERRIDE_PATH
 *                            or the IR3_DBG_ASM_ROUNDTRIP debug flag, both off:
 *                            returning NULL is the parse-failure path.
 *   nir_opt_reassociate_matrix_mul
 *                            an optional NIR pass ir3_optimize_loop calls that
 *                            the shared frontend's vendored nir subset does not
 *                            ship (it lives behind a feature the GLSL->NIR
 *                            frontend never needed).  "No progress" is a
 *                            correct, if unoptimised, answer.
 */
#include <stdbool.h>
#include <stdio.h>

#include "ir3/ir3_compiler.h"
#include "ir3/ir3_shader.h"
#include "ir3/ir3_assembler.h"

/* ---- ir3_disk_cache.c ---------------------------------------------------- */
void
ir3_disk_cache_init(struct ir3_compiler *compiler)
{
   (void)compiler;
}

void
ir3_disk_cache_init_shader_key(struct ir3_compiler *compiler,
                               struct ir3_shader *shader)
{
   (void)compiler;
   (void)shader;
}

bool
ir3_disk_cache_retrieve(struct ir3_shader *shader, struct ir3_shader_variant *v)
{
   (void)shader;
   (void)v;
   return false;
}

void
ir3_disk_cache_store(struct ir3_shader *shader, struct ir3_shader_variant *v)
{
   (void)shader;
   (void)v;
}

/* ---- ir3_shader_bisect.c ------------------------------------------------- */
void
ir3_shader_bisect_init(void)
{
}

bool
ir3_shader_bisect_need_shader_key(void)
{
   return false;
}

void
ir3_shader_bisect_dump_id(struct ir3_shader_variant *v)
{
   (void)v;
}

bool
ir3_shader_bisect_select(struct ir3_shader_variant *v)
{
   (void)v;
   return false;
}

bool
ir3_shader_bisect_disasm_select(struct ir3_shader_variant *v)
{
   (void)v;
   return false;
}

/* ---- ir3_parser.y -------------------------------------------------------- */
struct ir3 *
ir3_parse(struct ir3_shader_variant *v, struct ir3_kernel_info *k, FILE *f)
{
   (void)v;
   (void)k;
   (void)f;
   return NULL;
}

/* ---- compiler/nir (not in the vendored frontend subset) ------------------ */
struct nir_shader;
bool
nir_opt_reassociate_matrix_mul(struct nir_shader *shader)
{
   (void)shader;
   return false;
}
