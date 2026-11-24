/* Host debug tool: print the lowered NIR the GA10B backend generates code
 * from, for the real ocean fragment shader. */
#include "ocean_glsl.h"
#include "pglcompiler.h"
#include "nv_backend.h"
#include <stdio.h>

static void hook(nir_shader *nir, const struct nv_be_result *res, void *user) {
    (void)res;
    (void)user;
    nir_print_shader(nir, stdout);
}

int main(void) {
    std::string fs = ocean_gen::build_ocean_fs(2, 3, 4);
    struct pglc_program res;
    nv_be_set_test_hook(hook, NULL);
    bool ok = pglc_compile_compute(fs.c_str(), &res);
    if (!ok)
        fprintf(stderr, "compile FAIL: %s\n", res.log);
    pglc_program_free(&res);
    return ok ? 0 : 1;
}
