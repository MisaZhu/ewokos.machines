/* Host test: SSBO atomicMin through nv_be_compile_compute (ATOMG.E.MIN).
 * The Vulkan geom raster pass arbitrates depth with atomicMin; the emitter
 * encoding is transcribed from nvcc sm_87 output (pgl_geom.cubin).  Verify
 * the fixed field pattern + subop + 64-bit address bit against that sample:
 *   ATOMG.E.MIN.STRONG.GPU PT, R5, [R2.64], R5
 *   lo 0x00000005020579a8  hi 0x000ea800089ee1c4 (control aside) */
#include "compiler/nir/nir.h"
#include "compiler/nir/nir_builder.h"
#include "nv_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); \
} } while (0)

static uint32_t field(uint32_t w[4], unsigned lo, unsigned hi) {
    unsigned n = hi - lo;
    uint64_t v = ((uint64_t)w[1] << 32) | w[0];
    if (lo >= 64) {
        v = ((uint64_t)w[3] << 32) | w[2];
        lo -= 64;
    }
    return (uint32_t)((v >> lo) & ((1ULL << n) - 1));
}

int main(void) {
    nir_shader* s = nir_shader_create(NULL, MESA_SHADER_COMPUTE,
                                      nv_be_nir_options());
    s->info.workgroup_size[0] = 8;
    s->info.workgroup_size[1] = 1;
    s->info.workgroup_size[2] = 1;
    nir_function* fn = nir_function_create(s, "main");
    nir_function_impl* impl = nir_function_impl_create(fn);
    nir_builder b = nir_builder_create(impl);
    b.cursor = nir_before_block(nir_start_block(impl));

    /* tid.x * 4 byte offset into SSBO block 0 */
    nir_def* tid = nir_channel(&b, nir_load_local_invocation_id(&b), 0);
    nir_def* off = nir_ishl(&b, tid, nir_imm_int(&b, 2));
    nir_def* val = nir_imm_int(&b, 0x3f800000);
    nir_intrinsic_instr* at =
        nir_intrinsic_instr_create(s, nir_intrinsic_ssbo_atomic);
    at->num_components = 1;
    nir_def_init(&at->instr, &at->def, 1, 32);
    at->src[0] = nir_src_for_ssa(nir_imm_int(&b, 0));   /* block 0 */
    at->src[1] = nir_src_for_ssa(off);
    at->src[2] = nir_src_for_ssa(val);
    nir_intrinsic_set_atomic_op(at, nir_atomic_op_imin);
    nir_builder_instr_insert(&b, &at->instr);
    /* consume the old value so the def stays live */
    nir_intrinsic_instr* st =
        nir_intrinsic_instr_create(s, nir_intrinsic_store_ssbo);
    st->num_components = 1;
    st->src[0] = nir_src_for_ssa(&at->def);
    st->src[1] = nir_src_for_ssa(nir_imm_int(&b, 0));
    st->src[2] = nir_src_for_ssa(nir_imm_int(&b, 4));
    nir_intrinsic_set_write_mask(st, 1);
    nir_builder_instr_insert(&b, &st->instr);

    struct nv_be_result be;
    char log[1024] = {0};
    CK(nv_be_compile_compute(s, &be, 1, log, sizeof(log)));

    int found = 0;
    for (uint32_t i = 0; i + 3 < be.bin_words; i += 4) {
        uint32_t* w = be.bin + i;
        if (field(w, 0, 12) != 0x9a8)
            continue;
        found++;
        CK(field(w, 16, 24) != 255);          /* dst != RZ */
        CK(field(w, 24, 32) == 0);            /* addr = R0 (ssbo_addr_pair) */
        CK(field(w, 40, 64) == 0);            /* offset 0 */
        CK(field(w, 64, 72) == 0xc4);         /* no-uniform-base form */
        CK(field(w, 72, 73) == 1);
        CK(field(w, 76, 77) == 1);            /* .E */
        CK(field(w, 77, 81) == 7);            /* STRONG */
        CK(field(w, 81, 84) == 7);            /* pred dst PT */
        CK(field(w, 84, 87) == 1);
        CK(field(w, 87, 90) == 1);            /* subop MIN */
        CK(field(w, 91, 92) == 1);            /* addr64 */
        CK(field(w, 12, 16) == 7);            /* guard PT */
    }
    CK(found == 1);
    printf("ATOMG.MIN encode OK (%d instrs)\n", be.bin_words / 4);
    nv_be_result_free(&be);
    ralloc_free(s);
    return 0;
}