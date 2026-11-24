/* Host oracle driver: compile the real ocean fragment shader through the
 * shared frontend + GA10B backend (pglc_compile_compute), report the result,
 * and dump the synthesized cubin for nvdisasm. */
#include "ocean_glsl.h"
#include "pglcompiler.h"
#include "nv_pglc.h"
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    std::string fs = ocean_gen::build_ocean_fs(2, 3, 4);
    struct pglc_program res;
    bool ok = pglc_compile_compute(fs.c_str(), &res);
    if (!ok) {
        printf("ocean compile FAIL: %s\n", res.log);
        pglc_program_free(&res);
        return 1;
    }
    struct pglc_stage *st = &res.stage[PGLC_STAGE_COMPUTE];
    struct nv_pglc_stage *ab = (struct nv_pglc_stage *)st->backend;
    printf("ocean compile OK: %u SASS words (%u instrs), %u uniforms, "
           "local %ux%ux%u, cubin %u bytes\n",
           st->code_words, st->code_words / 4, st->num_uniform_bindings,
           st->local_size[0], st->local_size[1], st->local_size[2],
           ab ? ab->cubin_size : 0);
    if (ab && ab->cubin) {
        FILE *f = fopen("/tmp/nv-host/ocean.cubin", "wb");
        if (f) { fwrite(ab->cubin, 1, ab->cubin_size, f); fclose(f); }
    }
    pglc_program_free(&res);
    return 0;
}