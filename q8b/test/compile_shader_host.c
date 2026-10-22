/* GLSL -> NIR -> Mesa ir3 -> VERSION 2 blob. No firmware handling. */
#include <pglcompiler.h>
#include <adreno/adreno_prog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <geom_raster_cs.h>
#include "geom_test.h"

static int geometry(const char *path) {
    struct pglc_program rs = {0}, xf = {0};
    struct pglc_blob_opts opts = {.geom_kind = 1, .geom_tpw = 256};
    q8b_geom_test_t meta = {.magic = Q8B_GEOM_TEST_MAGIC};
    const char *names[GT_COUNT] = {"uMVP", "uViewportWH", "uVertCount",
        "uViewportWH", "uFbStride", "uTriFirst", "uTriCount", "uBlendMode",
        "uDepthWrite", "uCullSign"};
    const unsigned counts[GT_COUNT] = {16,2,1,2,1,1,1,1,1,1};
    void *blob = NULL;
    int ok = 0;
    if(!pglc_compile_compute_raw(geom_xform_color_cs_glsl, &xf) ||
       !pglc_compile_compute_raw(geom_raster_cs_glsl, &rs)) {
        fprintf(stderr,"geometry compile: xform %s raster %s\n",xf.log,rs.log);
        goto done;
    }
    meta.split = xf.stage[PGLC_STAGE_COMPUTE].uniform_blob_dwords;
    meta.uniform_words = meta.split + rs.stage[PGLC_STAGE_COMPUTE].uniform_blob_dwords;
    for(unsigned i = 0; i < GT_COUNT; i++) {
        struct pglc_stage *st = &(i < GT_R_VIEWPORT ? xf : rs).stage[PGLC_STAGE_COMPUTE];
        unsigned j;
        for(j = 0; j < st->num_uniform_bindings; j++) {
            const struct pglc_uniform_binding *b = &st->uniform_bindings[j];
            if(strcmp(names[i],b->name)) continue;
            if(b->num_components != counts[i] ||
               b->dword_offset + counts[i] > st->uniform_blob_dwords) goto done;
            meta.offset[i] = b->dword_offset + (i < GT_R_VIEWPORT ? 0 : meta.split);
            printf("%s %s offset=%u count=%u\n",i < GT_R_VIEWPORT ? "xform":"raster",
                   names[i],meta.offset[i],counts[i]);
            break;
        }
        if(j == st->num_uniform_bindings) goto done;
    }
    meta.blob_bytes = pglc_program_serialize(&rs,&xf,&opts,NULL,0);
    blob = malloc(meta.blob_bytes);
    if(!blob || meta.blob_bytes < sizeof(adreno_prog_blob_t) ||
       pglc_program_serialize(&rs,&xf,&opts,blob,meta.blob_bytes) != meta.blob_bytes) goto done;
    const adreno_prog_blob_t *hdr = blob;
    for(unsigned i = 0; i < 2; i++)
        printf("kernel%u binary=%u local=%u,%u,%u ssbo=%u ubo=%u uniform=%u private=%u\n",
            i,hdr->kern[i].binary_words*4,hdr->kern[i].local_size[0],
            hdr->kern[i].local_size[1],hdr->kern[i].local_size[2],
            hdr->kern[i].num_ssbo,hdr->kern[i].num_ubos,
            hdr->kern[i].uniform_blob_dwords,hdr->kern[i].pvtmem_size);
    FILE *f = fopen(path,"wb");
    ok = f && fwrite(&meta,1,sizeof(meta),f) == sizeof(meta) &&
         fwrite(blob,1,meta.blob_bytes,f) == meta.blob_bytes;
    if(f && fclose(f)) ok = 0;
    printf("geometry blob=%u uniform=%u split=%u\n",meta.blob_bytes,meta.uniform_words,meta.split);
done:
    free(blob);
    pglc_program_free(&rs); pglc_program_free(&xf);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    struct pglc_program prog = {0};
    FILE *f;
    long len;
    char *source;
    void *blob;
    uint32_t size;
    if(argc == 3 && strcmp(argv[1],"--geometry") == 0) return geometry(argv[2]);
    if(argc != 3) {
        fprintf(stderr, "usage: %s input.comp output.bin | --geometry output.test\n", argv[0]);
        return 1;
    }
    f = fopen(argv[1], "rb");
    if(!f) return 1;
    if(fseek(f, 0, SEEK_END) || (len = ftell(f)) <= 0 || len > 1024*1024 ||
       fseek(f, 0, SEEK_SET)) { fclose(f); return 1; }
    source = calloc((size_t)len + 1, 1);
    if(!source) { fclose(f); return 1; }
    if(fread(source, 1, len, f) != (size_t)len) {
        free(source); fclose(f); return 1;
    }
    fclose(f);
    if(!pglc_compile_compute_raw(source, &prog)) {
        fprintf(stderr, "compile failed: %s\n", prog.log);
        free(source); pglc_program_free(&prog); return 1;
    }
    free(source);
    size = pglc_program_serialize(&prog, NULL, NULL, NULL, 0);
    blob = malloc(size);
    if(!blob || size < sizeof(adreno_prog_blob_t) ||
       pglc_program_serialize(&prog, NULL, NULL, blob, size) != size) {
        free(blob); pglc_program_free(&prog); return 1;
    }
    const adreno_prog_kernel_t *k = &((adreno_prog_blob_t *)blob)->kern[0];
    printf("blob=%u binary=%u instrlen=%u constlen=%u local=%u,%u,%u "
           "ssbo=%u ubo=%u uniform=%u private=%u\n", size, k->binary_words*4,
           k->instrlen, k->constlen, k->local_size[0], k->local_size[1],
           k->local_size[2], k->num_ssbo, k->num_ubos,
           k->uniform_blob_dwords, k->pvtmem_size);
    for(uint32_t i = 0; i < prog.stage[PGLC_STAGE_COMPUTE].num_uniform_bindings; i++) {
        const struct pglc_uniform_binding *b =
            &prog.stage[PGLC_STAGE_COMPUTE].uniform_bindings[i];
        printf("uniform %s offset=%u count=%u\n", b->name,
               b->dword_offset, b->num_components);
    }
    f = fopen(argv[2], "wb");
    int ok = f && fwrite(blob, 1, size, f) == size;
    if(f && fclose(f)) ok = 0;
    free(blob);
    pglc_program_free(&prog);
    return ok ? 0 : 1;
}
