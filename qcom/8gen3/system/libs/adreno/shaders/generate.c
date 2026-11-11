/* Compile built-ins with the same frontend, ir3 backend and serializer as apps. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <adreno/adreno_prog.h>
#include "pglcompiler.h"

int main(int argc, char **argv) {
    if (argc != 4 || (strcmp(argv[1], "blit") && strcmp(argv[1], "raster"))) {
        fprintf(stderr, "usage: %s blit|raster input.comp output.h\n", argv[0]);
        return 1;
    }
    int blit = !strcmp(argv[1], "blit");
    FILE *in = fopen(argv[2], "rb");
    if (!in) { perror(argv[2]); return 1; }
    fseek(in, 0, SEEK_END);
    long length = ftell(in);
    rewind(in);
    if (length < 0 || length > 1024*1024) { fclose(in); return 1; }
    char *source = calloc((size_t)length + 1, 1);
    if (!source || fread(source, 1, length, in) != (size_t)length) {
        fclose(in); free(source); return 1;
    }
    fclose(in);
    struct pglc_program program = {0};
    pglc_set_version(690);
    if (!pglc_compile_compute_raw(source, &program)) {
        fprintf(stderr, "%s: %s\n", argv[2], program.log);
        free(source); pglc_program_free(&program); return 1;
    }
    free(source);
    const struct pglc_stage *stage = &program.stage[PGLC_STAGE_COMPUTE];
    const char *names[3] = {blit ? "uPitchOrigin" : "uPitchShape", "uDestination",
        blit ? "uStep" : "uOperation"};
    /* Driver packs twelve words in this exact order, independent of names. */
    int uniforms_ok = stage->num_uniform_bindings == 3;
    for (unsigned i = 0; i < 3; ++i) {
        int found = 0;
        for (unsigned j = 0; j < stage->num_uniform_bindings; ++j) {
            const struct pglc_uniform_binding *u = &stage->uniform_bindings[j];
            if (!strcmp(names[i], u->name) && u->dword_offset == i*4 && u->num_components == 4)
                found = 1;
        }
        uniforms_ok &= found;
    }
    uint32_t bytes = pglc_program_serialize(&program, NULL, NULL, NULL, 0);
    unsigned char *data = bytes >= sizeof(adreno_prog_blob_t) ? malloc(bytes) : NULL;
    if (!uniforms_ok || !data || pglc_program_serialize(&program, NULL, NULL, data, bytes) != bytes) {
        fprintf(stderr, "%s: serialization/uniform layout failed\n", argv[2]);
        free(data); pglc_program_free(&program); return 1;
    }
    const adreno_prog_blob_t *blob = (const void *)data;
    const adreno_prog_kernel_t *k = &blob->kern[0];
    if (blob->version != ADRENO_PROG_VERSION || blob->magic != ADRENO_PROG_MAGIC ||
            bytes != sizeof(*blob) + blob->payload_bytes || k->uniform_blob_dwords != 12 ||
            k->num_ssbo != 2 || k->pvtmem_size || k->num_samp ||
            k->local_size[0] != (blit ? 64u : 16u) ||
            k->local_size[1] != (blit ? 2u : 8u) || k->local_size[2] != 1) {
        fprintf(stderr, "%s: incompatible built-in kernel\n", argv[2]);
        free(data); pglc_program_free(&program); return 1;
    }
    FILE *out = fopen(argv[3], "w");
    if (!out) { perror(argv[3]); free(data); pglc_program_free(&program); return 1; }
    const char *symbol = blit ? "q8b_2d_blob" : "q8b_raster_blob";
    const char *prefix = blit ? "Q8B_2D_BLOB" : "Q8B_RASTER_BLOB";
    fprintf(out, "/* Generated from %s.comp; make builtin-shaders in libadreno. */\n", argv[1]);
    fprintf(out, "#define %s_VERSION %uu\n#define %s_HEADER_BYTES %zuu\n", prefix, blob->version, prefix, sizeof(*blob));
    fprintf(out, "static const unsigned char %s[] __attribute__((aligned(8))) = {\n", symbol);
    for (uint32_t i = 0; i < bytes; ++i) {
        if (i % 12 == 0) fprintf(out, "  ");
        fprintf(out, "0x%02x,%s", data[i], i % 12 == 11 || i + 1 == bytes ? "\n" : " ");
    }
    fprintf(out, "};\nstatic const unsigned int %s_len = %uu;\n", symbol, bytes);
    int failed = ferror(out);
    failed |= fclose(out) != 0;
    printf("%s: version=%u header=%zu bytes=%u\n", argv[3], blob->version, sizeof(*blob), bytes);
    free(data); pglc_program_free(&program);
    return failed ? 1 : 0;
}
