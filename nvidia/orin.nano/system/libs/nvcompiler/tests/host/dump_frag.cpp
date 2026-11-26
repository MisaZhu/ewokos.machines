/* Host debug tool: print the frontend NIR (pre-backend) of any fragment
 * shader: dump_frag <shader.frag> */
#include "pglcompiler.h"
#include "pglcompiler_frontend.h"
#include "nv_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: dump_frag <shader.frag>\n"); return 1; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    std::string fs;
    char buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) fs.append(buf, got);
    fclose(f);
    char* cs = pglc_fe_fragment_to_compute_source(fs.c_str());
    if (!cs) { fprintf(stderr, "fragment_to_compute failed\n"); return 1; }
    struct pglc_fe_shaders* sh = NULL;
    char log[4096] = "";
    if (!pglc_fe_compile_compute(cs, nv_be_nir_options(), &sh, log, sizeof(log))) {
        fprintf(stderr, "frontend FAIL: %s\n", log);
        return 1;
    }
    nir_shader* nir = pglc_fe_nir(sh, PGLC_STAGE_COMPUTE);
    nir_print_shader(nir, stdout);
    pglc_fe_shaders_free(sh);
    free(cs);
    return 0;
}