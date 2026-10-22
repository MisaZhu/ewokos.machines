/* Bounded compute-shader IPC test. Uses the existing daemon, never
 * initializes the GPU and has no CPU rendering fallback. Input must be
 * the blob compiled from test/cs_smoke.comp for a 64x64 target. */
#include <g2dclient/g2dclient.h>
#include <adreno/adreno_prog.h>
#include <ewoksys/proc.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "../../../test/geom_test.h"

#define W 64u
#define H 64u
#define GUARD 0x13579bdfu
#define GUARD_BYTES 0x4000u

static void retain_on_timeout(int ret) {
    if(ret == G2D_ERR_TIMEOUT) {
        printf("gpushader: retaining buffers until physical power cycle\n");
        while(1) proc_nsleep_precise(1000000000);
    }
}

static uint32_t pattern(unsigned x, unsigned y, unsigned seed) {
    return 0xff000000u | (((3*x+7*y+seed)&255u)<<16) |
           (((5*x+y+3*seed)&255u)<<8) | ((x+11*y+7*seed)&255u);
}

static int check_strided(const char *name, volatile uint32_t *p,
                          unsigned w, unsigned h, unsigned pitch,
                          unsigned seed, const uint32_t *solid) {
    unsigned errors = 0, padding = 0, guards = 0;
    for(unsigned y = 0; y < h; y++) {
        for(unsigned x = 0; x < pitch; x++) {
            uint32_t want = x >= w ? GUARD : solid ? *solid : pattern(x,y,seed);
            uint32_t got = p[y*pitch+x];
            if(got != want) {
                if(x >= w) padding++;
                else if(errors++ == 0)
                    printf("%s mismatch (%u,%u): %08x expected %08x\n",name,x,y,got,want);
            }
        }
    }
    for(unsigned i = pitch*h; i < pitch*h + GUARD_BYTES/4; i++)
        if(p[i] != GUARD) guards++;
    printf("%s %ux%u stride=%u seed=%u: %s (%u pixels, %u padding, %u guards)\n",
           name,w,h,pitch*4,seed,errors||padding||guards?"FAIL":"PASS",
           errors,padding,guards);
    return errors || padding || guards;
}

static int uniform_tests(int handle) {
    static const unsigned sizes[][2] = {{64,64},{65,37},{320,240},{720,720}};
    uint32_t *unif = NULL, *pos = NULL, *dst = NULL;
    int unif_id, pos_id, dst_id, ret = 1;
    ewokos_addr_t pa;
    g2d_canvas_t empty = {0};
    if(g2d_shm_alloc_phy(16, &unif_id, &unif, &pa) ||
       g2d_shm_alloc_phy(36, &pos_id, &pos, &pa)) goto done;
    memset(pos, 0, 36);
    for(unsigned sz = 0; sz < sizeof(sizes)/sizeof(sizes[0]); sz++) {
        unsigned w = sizes[sz][0], h = sizes[sz][1], pitch = (w+15u)&~15u;
        unsigned count = pitch*h + GUARD_BYTES/4;
        if(g2d_shm_alloc_phy(count*4, &dst_id, &dst, &pa)) goto done;
        for(unsigned i = 0; i < count; i++) dst[i] = GUARD;
        for(unsigned frame = 0; frame < 4; frame++) {
            g2d_fill_req_t fill;
            g2d_pgl_req_t draw;
            uint32_t color = 0xff204060u + frame*0x010203u;
            unsigned seed = sz*53 + frame*17 + 1;
            /* A2D uses canvas width as pitch; CS uses its explicit stride. */
            g2d_fill_req_init(&fill, g2d_canvas(dst_id,count*4,pitch,h,1),
                             g2d_rect(0,0,w,h),color);
            int rc = g2d_fill_rect(&fill);
            retain_on_timeout(rc);
            if(rc || check_strided("A2D before CS",dst,w,h,pitch,seed,&color)) goto done;
            unif[0] = w; unif[1] = h; unif[2] = pitch; unif[3] = seed;
            g2d_pgl_draw_req_init(&draw,
                g2d_canvas(dst_id,count*4,w,h,1), g2d_canvas(pos_id,36,3,3,1),
                empty,G2D_PGL_MODE_TRIANGLES,3,G2D_PGL_FMT_RGBA8,pitch*4,
                0,0,0,0,0,0,empty,0,0,0,1.0f);
            g2d_pgl_draw_req_set_shader(&draw,handle,unif_id,4,1);
            rc = g2d_pgl_draw(&draw);
            retain_on_timeout(rc);
            if(rc) { printf("gpushader: uniform draw ret=%d\n",rc); goto done; }
            if(check_strided("uniform CS",dst,w,h,pitch,seed,NULL)) goto done;
        }
        g2d_shm_free(dst); dst = NULL;
    }
    ret = 0;
done:
    if(dst) g2d_shm_free(dst);
    if(pos) g2d_shm_free(pos);
    if(unif) g2d_shm_free(unif);
    return ret;
}

#include "raster_tests.h"

int main(int argc, char **argv) {
    FILE *f;
    long size;
    uint32_t *blob = NULL, *pixels = NULL, *pos = NULL;
    int blob_id, dst_id, pos_id, handle, ret = 1, upload_only, uniforms, raster;
    ewokos_addr_t pa;
    uint32_t version = 0;
    g2d_pgl_program_req_t program;
    g2d_pgl_req_t draw;
    g2d_canvas_t empty = {0};
    const unsigned count = W * H + GUARD_BYTES / 4;

    if(argc == 3 && strcmp(argv[2], "--geometry") == 0)
        return q8b_geometry_test(argv[1]);
    upload_only = argc == 3 && strcmp(argv[2], "--upload-only") == 0;
    uniforms = argc == 3 && strcmp(argv[2], "--uniforms") == 0;
    raster = argc == 3 && strcmp(argv[2], "--raster") == 0;
    if(argc < 2 || argc > 3 || (argc == 3 && !upload_only && !uniforms && !raster)) {
        printf("usage: gpushader shader.bin [--upload-only|--uniforms|--raster|--geometry]\n");
        return 1;
    }
    if(g2d_get_version(&version) || version != 690) {
        printf("gpushader: expected A690, got %u\n", version); return 1;
    }
    f = fopen(argv[1], "rb");
    if(!f) { printf("gpushader: cannot open blob\n"); return 1; }
    if(fseek(f, 0, SEEK_END) || (size = ftell(f)) < (long)sizeof(adreno_prog_blob_t) ||
       size > 64*1024 || (size & 3) || fseek(f, 0, SEEK_SET)) {
        fclose(f); return 1;
    }
    if(g2d_shm_alloc(size, &blob_id, &blob)) { fclose(f); return 1; }
    if(fread(blob, 1, size, f) != (unsigned)size) {
        fclose(f); goto done;
    }
    fclose(f);
    const adreno_prog_blob_t *hdr = (const adreno_prog_blob_t *)blob;
    if(hdr->magic != ADRENO_PROG_MAGIC || hdr->version != ADRENO_PROG_VERSION ||
       hdr->is_geom || hdr->payload_bytes != size - sizeof(*hdr) ||
       hdr->kern[0].num_ssbo != (raster ? 2u : 1u) || hdr->kern[0].ssbo_binding[0] != 0 ||
       (raster && hdr->kern[0].ssbo_binding[1] != 0) ||
       hdr->kern[0].uniform_blob_dwords != (raster ? 12u : uniforms ? 4u : 0u) ||
       hdr->kern[1].binary_words) {
        printf("gpushader: unexpected smoke blob contract\n"); goto done;
    }
    g2d_pgl_program_req_init(&program, g2d_canvas(blob_id, size, size/4, 1, 0));
    handle = g2d_pgl_program(&program);
    printf("gpushader: A690 program upload handle=%d (%ld bytes)\n", handle, size);
    if(handle < 0) goto done;
    if(upload_only) { ret = 0; goto done; }
    if(raster) {
        ret = raster_tests(handle);
        printf("gpushader raster payload: %s\n",ret?"FAIL":"PASS");
        goto done;
    }
    if(uniforms) {
        ret = uniform_tests(handle);
        printf("gpushader uniforms: %s\n",ret?"FAIL":"PASS");
        goto done;
    }

    if(g2d_shm_alloc_phy(count*4, &dst_id, &pixels, &pa) ||
       g2d_shm_alloc_phy(36, &pos_id, &pos, &pa)) goto done;
    for(unsigned i = 0; i < count; i++) pixels[i] = GUARD;
    memset(pos, 0, 36); /* Required transport vertices, unused by this CS. */
    g2d_pgl_draw_req_init(&draw,
        g2d_canvas(dst_id, count*4, W, H, 1),
        g2d_canvas(pos_id, 36, 3, 3, 1), empty,
        G2D_PGL_MODE_TRIANGLES, 3, G2D_PGL_FMT_RGBA8, W*4,
        0, 0, 0, 0, 0, 0, empty, 0, 0, 0, 1.0f);
    g2d_pgl_draw_req_set_shader(&draw, handle, 0, 0, 0);
    ret = g2d_pgl_draw(&draw);
    printf("gpushader: compute draw ret=%d\n", ret);
    /* An unretired GPU may still reference the client's mappings. */
    retain_on_timeout(ret);
    if(ret) { ret = 1; goto done; }
    unsigned errors = 0, guards = 0;
    volatile uint32_t *readback = pixels;
    for(unsigned y = 0; y < H; y++) {
        for(unsigned x = 0; x < W; x++) {
            uint32_t want = pattern(x,y,0);
            uint32_t got = readback[y*W+x];
            if(got != want && errors++ == 0)
                printf("first mismatch (%u,%u): %08x expected %08x\n", x,y,got,want);
        }
    }
    for(unsigned i = W*H; i < count; i++)
        if(readback[i] != GUARD) guards++;
    printf("gpushader: %s (64x64, %u pixel errors, %u guard errors)\n",
           errors || guards ? "FAIL" : "PASS", errors, guards);
    ret = errors || guards;
done:
    if(pos) g2d_shm_free(pos);
    if(pixels) g2d_shm_free(pixels);
    if(blob) g2d_shm_free(blob);
    return ret;
}
