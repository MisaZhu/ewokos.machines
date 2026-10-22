/* Two-pass geometry readback through the existing daemon. No CPU renderer. */
#include <g2dclient/g2dclient.h>
#include <adreno/adreno_prog.h>
#include <ewoksys/proc.h>
#include <stdio.h>
#include <string.h>
#include "../../../test/geom_test.h"

#define GUARD 0x13579bdfu
#define GUARD_WORDS 4096u
#define CLEAR 0xff204060u

static void put_float(uint32_t *dst, float f) { memcpy(dst,&f,4); }

/* Independent coverage expectation for screen vertices (w/4,h/4),
 * (3w/4,h/4), (w/4,3h/4), with an optional w/8 x translation. */
static int check(const char *name, volatile uint32_t *fb, volatile uint32_t *z,
                 unsigned w, unsigned h, unsigned shift, int culled,
                 uint32_t color, uint32_t depth) {
    unsigned pixels = 0, depths = 0, guards = 0, covered = 0;
    for(unsigned y = 0; y < h; y++) for(unsigned x = 0; x < w; x++) {
        int inside = !culled && x >= w/4+shift && y >= h/4 &&
                     x+y+1 <= w+shift;
        uint32_t want = inside ? color : CLEAR;
        uint32_t dz = inside ? depth : 0xffffffffu;
        uint32_t got = fb[y*w+x], gz = z[y*w+x];
        covered += inside;
        if(got != want && pixels++ == 0)
            printf("geometry %s pixel (%u,%u) %08x expected %08x\n",name,x,y,got,want);
        /* UNORM conversion and barycentric arithmetic can vary by a few LSBs. */
        unsigned delta = gz > dz ? gz-dz : dz-gz;
        if((inside ? delta > 2u : gz != dz) && depths++ == 0)
            printf("geometry %s depth (%u,%u) %08x expected %08x +/-2\n",name,x,y,gz,dz);
    }
    for(unsigned i = w*h; i < w*h+GUARD_WORDS; i++)
        if(fb[i] != GUARD || z[i] != GUARD) guards++;
    printf("geometry %s %ux%u: %s (%u covered, %u pixels, %u depth, %u guards)\n",
           name,w,h,pixels||depths||guards?"FAIL":"PASS",covered,pixels,depths,guards);
    return pixels || depths || guards;
}

int q8b_geometry_test(const char *path) {
    q8b_geom_test_t meta;
    const unsigned counts[GT_COUNT] = {16,2,1,2,1,1,1,1,1,1};
    const unsigned sizes[] = {64,320};
    uint32_t *blob = NULL, *unif = NULL, *fb = NULL, *z = NULL;
    uint32_t *pos = NULL, *col = NULL;
    int blob_id, unif_id, fb_id, z_id, pos_id, col_id, handle, ret = 1;
    uint32_t version = 0;
    ewokos_addr_t pa;
    FILE *f = NULL;
    if(g2d_get_version(&version) || version != 690) return 1;
    f = fopen(path,"rb");
    if(!f || fread(&meta,1,sizeof(meta),f) != sizeof(meta) ||
       meta.magic != Q8B_GEOM_TEST_MAGIC || meta.blob_bytes < sizeof(adreno_prog_blob_t) ||
       meta.blob_bytes > 64*1024 || (meta.blob_bytes & 3) ||
       !meta.split || meta.split >= meta.uniform_words || meta.uniform_words > 256) goto done;
    for(unsigned i = 0; i < GT_COUNT; i++) {
        unsigned begin = i < GT_R_VIEWPORT ? 0 : meta.split;
        unsigned end = i < GT_R_VIEWPORT ? meta.split : meta.uniform_words;
        if(meta.offset[i] < begin || meta.offset[i] > end ||
           counts[i] > end-meta.offset[i]) goto done;
        for(unsigned j = 0; j < i; j++)
            if(meta.offset[i] < meta.offset[j]+counts[j] &&
               meta.offset[j] < meta.offset[i]+counts[i]) goto done;
    }
    if(g2d_shm_alloc(meta.blob_bytes,&blob_id,&blob) ||
       fread(blob,1,meta.blob_bytes,f) != meta.blob_bytes || fgetc(f) != EOF) goto done;
    fclose(f); f = NULL;
    const adreno_prog_blob_t *hdr = (const adreno_prog_blob_t *)blob;
    if(hdr->magic != ADRENO_PROG_MAGIC || hdr->version != ADRENO_PROG_VERSION ||
       hdr->is_geom != 1 || hdr->geom_tpw != 256 || hdr->cs2_uniform_blob_dwords != meta.split ||
       hdr->kern[1].uniform_blob_dwords != meta.split ||
       hdr->kern[0].uniform_blob_dwords != meta.uniform_words-meta.split ||
       hdr->payload_bytes != meta.blob_bytes-sizeof(*hdr)) goto done;
    for(unsigned k = 0; k < 2; k++) {
        if(hdr->kern[k].num_ssbo != 3) goto done;
        for(unsigned b = 0; b < 3; b++)
            if(hdr->kern[k].ssbo_binding[b] != b) goto done;
    }
    g2d_pgl_program_req_t program;
    g2d_pgl_program_req_init(&program,g2d_canvas(blob_id,meta.blob_bytes,meta.blob_bytes/4,1,0));
    handle = g2d_pgl_program(&program);
    printf("geometry: program handle=%d blob=%u uniforms=%u split=%u\n",
           handle,meta.blob_bytes,meta.uniform_words,meta.split);
    if(handle < 0) goto done;
    if(g2d_shm_alloc_phy(meta.uniform_words*4,&unif_id,&unif,&pa) ||
       g2d_shm_alloc_phy(36+64,&pos_id,&pos,&pa) ||
       g2d_shm_alloc_phy(36+64,&col_id,&col,&pa)) goto done;
    for(unsigned i = 0; i < 25; i++) pos[i] = col[i] = GUARD;
    for(unsigned sz = 0; sz < sizeof(sizes)/sizeof(sizes[0]); sz++) {
        unsigned w = sizes[sz], h = w, bytes = (w*h+GUARD_WORDS)*4;
        if(g2d_shm_alloc_phy(bytes,&fb_id,&fb,&pa) ||
           g2d_shm_alloc_phy(bytes,&z_id,&z,&pa)) goto done;
        for(unsigned i = 0; i < w*h+GUARD_WORDS; i++) fb[i] = z[i] = GUARD;
        for(unsigned frame = 0; frame < 6; frame++) {
            static const char *names[] = {"first-red","far-reject","near-blue",
                "depth-readonly","cull-all","translated"};
            static const float zs[] = {0.0f,0.5f,-0.5f,-1.0f,0.0f,0.0f};
            static const uint32_t colors[] = {0xffff0000u,0xffff0000u,0xff0000ffu,
                0xff00ff00u,CLEAR,0xff00ff00u};
            static const uint32_t depths[] = {8388607u,8388607u,4194303u,
                4194303u,0xffffffffu,8388607u};
            static const float xy[] = {-0.5f,0.5f,0.5f,0.5f,-0.5f,-0.5f};
            uint32_t pos_copy[9], col_copy[9];
            memset(unif,0,meta.uniform_words*4);
            uint32_t *m = unif+meta.offset[GT_MVP];
            for(unsigned i = 0; i < 4; i++) put_float(m+i*5,1.0f);
            if(frame == 5) put_float(m+12,0.25f);
            for(unsigned i = 0; i < 3; i++) {
                put_float(pos+i*3,xy[i*2]); put_float(pos+i*3+1,xy[i*2+1]);
                put_float(pos+i*3+2,zs[frame]);
                for(unsigned c = 0; c < 3; c++)
                    put_float(col+i*3+c,(frame == 0 ? c == 0 : frame == 2 ? c == 2 : c == 1) ? 1.0f:0.0f);
            }
            memcpy(pos_copy,pos,36); memcpy(col_copy,col,36);
            put_float(unif+meta.offset[GT_X_VIEWPORT],(float)w);
            put_float(unif+meta.offset[GT_X_VIEWPORT]+1,(float)h);
            put_float(unif+meta.offset[GT_R_VIEWPORT],(float)w);
            put_float(unif+meta.offset[GT_R_VIEWPORT]+1,(float)h);
            unif[meta.offset[GT_VERT_COUNT]] = 3;
            unif[meta.offset[GT_FB_STRIDE]] = w;
            unif[meta.offset[GT_TRI_COUNT]] = 1;
            unif[meta.offset[GT_DEPTH_WRITE]] = frame != 3;
            unif[meta.offset[GT_CULL]] = frame == 4 ? 3 : 0;
            g2d_pgl_req_t draw;
            g2d_pgl_draw_req_init(&draw,g2d_canvas(fb_id,bytes,w,h,1),
                g2d_canvas(pos_id,36+64,3,3,1),g2d_canvas(col_id,36+64,3,3,1),
                G2D_PGL_MODE_TRIANGLES,3,G2D_PGL_FMT_RGBA8,w*4,0,0,0,
                0x0201,1,0,g2d_canvas(z_id,bytes,w,h,1),w*4,
                frame == 0 || frame >= 4,0xff604020u,1.0f);
            g2d_pgl_draw_req_set_shader(&draw,handle,unif_id,meta.uniform_words,frame == 0);
            int rc = g2d_pgl_draw(&draw);
            if(rc == G2D_ERR_TIMEOUT) {
                printf("geometry: timeout; retaining buffers until physical power cycle\n");
                while(1) proc_nsleep_precise(1000000000);
            }
            if(rc) { printf("geometry %s: draw ret=%d\n",names[frame],rc); goto done; }
            if(check(names[frame],fb,z,w,h,frame == 5 ? w/8 : 0,frame == 4,
                     colors[frame],depths[frame])) goto done;
            if(memcmp(pos,pos_copy,36) || memcmp(col,col_copy,36)) {
                printf("geometry: input changed\n"); goto done;
            }
            for(unsigned i = 9; i < 25; i++)
                if(pos[i] != GUARD || col[i] != GUARD) {
                    printf("geometry: input guard changed\n"); goto done;
                }
        }
        g2d_shm_free(fb); fb = NULL; g2d_shm_free(z); z = NULL;
    }
    ret = 0;
done:
    if(f) fclose(f);
    if(blob) g2d_shm_free(blob);
    if(unif) g2d_shm_free(unif);
    if(fb) g2d_shm_free(fb);
    if(z) g2d_shm_free(z);
    if(pos) g2d_shm_free(pos);
    if(col) g2d_shm_free(col);
    printf("geometry: %s\n",ret?"FAIL":"PASS");
    return ret;
}
