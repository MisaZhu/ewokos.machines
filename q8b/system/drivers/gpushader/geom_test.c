/* Two-pass geometry readback through the existing daemon. No CPU renderer. */
#include <g2dclient/g2dclient.h>
#include <adreno/adreno_prog.h>
#include <ewoksys/proc.h>
#include <ewoksys/kernel_tic.h>
#include <stdio.h>
#include <stdlib.h>
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

int q8b_geometry_test(const char *path, int batch) {
    q8b_geom_test_t meta;
    const unsigned counts[GT_COUNT] = {16,2,1,2,1,1,1,1,1,1};
    const unsigned sizes[] = {64,320,720};
    uint32_t *blob = NULL, *unif = NULL, *fb = NULL, *z = NULL;
    uint32_t *pos = NULL, *col = NULL, *tex = NULL;
    uint32_t *input_copy = NULL;
    int blob_id, unif_id, fb_id, z_id, pos_id, col_id, tex_id, handle, ret = 1, texture;
    uint32_t version = 0;
    ewokos_addr_t pa;
    FILE *f = NULL;
    const char *stage = "envelope";
    if(g2d_get_version(&version) || version != 690) {
        printf("geometry: expected A690, got %u\n",version); return 1;
    }
    f = fopen(path,"rb");
    if(!f || fread(&meta,1,sizeof(meta),f) != sizeof(meta) ||
       (meta.magic != Q8B_GEOM_TEST_MAGIC && meta.magic != Q8B_GEOM_TEX_TEST_MAGIC) ||
       meta.blob_bytes < sizeof(adreno_prog_blob_t) ||
       meta.blob_bytes > 64*1024 || (meta.blob_bytes & 3) ||
       !meta.split || meta.split >= meta.uniform_words || meta.uniform_words > 256) goto done;
    texture = meta.magic == Q8B_GEOM_TEX_TEST_MAGIC;
    stage = "uniform reflection";
    unsigned max_vertices = batch ? 257*3 : 3;
    unsigned pos_words = max_vertices*(texture ? 6 : 3), col_words = max_vertices*3;
    unsigned pos_bytes = (pos_words+16)*4, col_bytes = (col_words+16)*4;
    for(unsigned i = 0; i < (texture ? GT_COUNT-1 : GT_COUNT); i++) {
        unsigned begin = i < GT_R_VIEWPORT ? 0 : meta.split;
        unsigned end = i < GT_R_VIEWPORT ? meta.split : meta.uniform_words;
        if(meta.offset[i] < begin || meta.offset[i] > end ||
           counts[i] > end-meta.offset[i]) goto done;
        for(unsigned j = 0; j < i; j++)
            if(meta.offset[i] < meta.offset[j]+counts[j] &&
               meta.offset[j] < meta.offset[i]+counts[i]) goto done;
    }
    stage = "program read/allocation";
    if(g2d_shm_alloc(meta.blob_bytes,&blob_id,&blob) ||
       fread(blob,1,meta.blob_bytes,f) != meta.blob_bytes || fgetc(f) != EOF) goto done;
    fclose(f); f = NULL;
    const adreno_prog_blob_t *hdr = (const adreno_prog_blob_t *)blob;
    stage = "program contract";
    if(hdr->magic != ADRENO_PROG_MAGIC ||
       (hdr->version != ADRENO_PROG_VERSION && hdr->version != ADRENO_PROG_PIXEL_VERSION) ||
       hdr->is_geom != (texture ? 2 : 1) || hdr->geom_tpw != (texture ? 0 : 256) ||
       hdr->cs2_uniform_blob_dwords != meta.split ||
       hdr->kern[1].uniform_blob_dwords != meta.split ||
       hdr->kern[0].uniform_blob_dwords != meta.uniform_words-meta.split ||
       hdr->payload_bytes != meta.blob_bytes-sizeof(*hdr)) goto done;
    for(unsigned k = 0; k < 2; k++) {
        unsigned bindings = texture ? (k ? 2 : 4) : 3;
        if(hdr->kern[k].num_ssbo != bindings) goto done;
        unsigned seen = 0;
        for(unsigned b = 0; b < bindings; b++) {
            /* Linked UAV slots can have any order. The compiler supplies
             * layout(binding), which the driver uses to select each BO. */
            unsigned binding = hdr->kern[k].ssbo_binding[b];
            if(binding >= bindings || (seen & (1u << binding))) {
                printf("geometry: kernel %u invalid SSBO slot %u binding %u\n",k,b,binding);
                goto done;
            }
            seen |= 1u << binding;
        }
    }
    stage = "program upload";
    g2d_pgl_program_req_t program;
    g2d_pgl_program_req_init(&program,g2d_canvas(blob_id,meta.blob_bytes,meta.blob_bytes/4,1,0));
    handle = g2d_pgl_program(&program);
    printf("geometry: program handle=%d blob=%u uniforms=%u split=%u\n",
           handle,meta.blob_bytes,meta.uniform_words,meta.split);
    if(handle < 0) goto done;
    stage = "input allocation";
    if(g2d_shm_alloc_phy(meta.uniform_words*4,&unif_id,&unif,&pa) ||
       g2d_shm_alloc_phy(pos_bytes,&pos_id,&pos,&pa) ||
       g2d_shm_alloc_phy(col_bytes,&col_id,&col,&pa)) goto done;
    input_copy = malloc((pos_words+col_words)*4);
    if(!input_copy) goto done;
    for(unsigned i = 0; i < pos_words+16; i++) pos[i] = GUARD;
    for(unsigned i = 0; i < col_words+16; i++) col[i] = GUARD;
    static const uint32_t texels[] = {0xffe02030u,0xff20d040u,0xff3040c0u,0xffb07020u};
    if(texture) {
        if(g2d_shm_alloc_phy(80,&tex_id,&tex,&pa)) goto done;
        memcpy(tex,texels,16);
        for(unsigned i = 4; i < 20; i++) tex[i] = GUARD;
    }
    for(unsigned sz = 0; sz < sizeof(sizes)/sizeof(sizes[0]); sz++) {
        unsigned w = sizes[sz], h = w, bytes = (w*h+GUARD_WORDS)*4;
        stage = "target allocation";
        if(g2d_shm_alloc_phy(bytes,&fb_id,&fb,&pa) ||
           g2d_shm_alloc_phy(bytes,&z_id,&z,&pa)) goto done;
        for(unsigned i = 0; i < w*h+GUARD_WORDS; i++) fb[i] = z[i] = GUARD;
        unsigned elapsed_total = 0, elapsed_max = 0, draws = 0;
        for(unsigned frame = 0; frame < (batch ? 6 : texture ? 4 : 12); frame++) {
            stage = "draw/readback";
            static const char *names[] = {"first-red","far-reject","near-blue",
                "depth-readonly","cull-all","translated","blend-alpha-one",
                "blend-add","cull-front-visible","cull-back-hidden","behind-eye","degenerate"};
            static const float zs[] = {0.0f,0.5f,-0.5f,-1.0f,0.0f,0.0f,0,0,0,0,0,0};
            static const uint32_t colors[] = {0xffff0000u,0xffff0000u,0xff0000ffu,
                0xff00ff00u,CLEAR,0xff00ff00u,0xff00ff00u,0xff20ff60u,
                0xff00ff00u,CLEAR,CLEAR,CLEAR};
            static const uint32_t depths[] = {8388607u,8388607u,4194303u,
                4194303u,0xffffffffu,8388607u,8388607u,8388607u,
                8388607u,0xffffffffu,0xffffffffu,0xffffffffu};
            static const float xy[] = {-0.5f,0.5f,0.5f,0.5f,-0.5f,-0.5f};
            static const char *tex_names[] = {"texture-vflip","texture-top-right",
                "texture-clamp-top-left","texture-clamp-bottom-right"};
            static const float uv[][2] = {{0.25f,0.25f},{0.75f,0.75f},{-1.0f,2.0f},{2.0f,-1.0f}};
            static const unsigned sample[] = {2,1,0,3};
            static const unsigned tri_counts[] = {2,2,65,65,257,257};
            unsigned triangles = batch ? tri_counts[frame] : 1;
            unsigned vertices = triangles*3;
            unsigned near_tri = frame & 1 ? triangles-1 : 0;
            char batch_name[64];
            snprintf(batch_name,sizeof(batch_name),"overlap-%u-near-%s",triangles,
                     frame & 1 ? "last" : "first");
            memset(unif,0,meta.uniform_words*4);
            uint32_t *m = unif+meta.offset[GT_MVP];
            for(unsigned i = 0; i < 4; i++) put_float(m+i*5,1.0f);
            if(!batch && !texture && frame == 5) put_float(m+12,0.25f);
            if(!batch && !texture && frame == 10) put_float(m+15,-1.0f);
            for(unsigned i = 0; i < vertices; i++) {
                unsigned off = i*(texture ? 6 : 3);
                unsigned v = i%3;
                if(!batch && !texture && frame == 11) v = 0;
                int nearest = i/3 == near_tri;
                put_float(pos+off,xy[v*2]); put_float(pos+off+1,xy[v*2+1]);
                put_float(pos+off+2,batch ? (nearest ? -0.5f : 0.5f) :
                          texture ? 0.0f : zs[frame]);
                if(texture) {
                    unsigned tex_case = batch ? (nearest ? 0 : 2) : frame;
                    put_float(pos+off+3,uv[tex_case][0]); put_float(pos+off+4,uv[tex_case][1]);
                    put_float(pos+off+5,0.0f);
                }
                for(unsigned c = 0; c < 3; c++)
                    put_float(col+i*3+c,(batch ? (nearest ? c == 2 : c == 0) :
                              frame == 0 ? c == 0 : frame == 2 ? c == 2 : c == 1) ? 1.0f:0.0f);
            }
            memcpy(input_copy,pos,pos_words*4); memcpy(input_copy+pos_words,col,col_words*4);
            put_float(unif+meta.offset[GT_X_VIEWPORT],(float)w);
            put_float(unif+meta.offset[GT_X_VIEWPORT]+1,(float)h);
            put_float(unif+meta.offset[GT_R_VIEWPORT],(float)w);
            put_float(unif+meta.offset[GT_R_VIEWPORT]+1,(float)h);
            unif[meta.offset[GT_VERT_COUNT]] = vertices;
            unif[meta.offset[GT_FB_STRIDE]] = w;
            unif[meta.offset[GT_TRI_COUNT]] = triangles;
            if(texture) {
                unif[meta.offset[GT_BLEND]] = 2; /* uTexW */
                unif[meta.offset[GT_DEPTH_WRITE]] = 2; /* uTexH */
            } else {
                unif[meta.offset[GT_DEPTH_WRITE]] = batch || frame != 3;
                unif[meta.offset[GT_BLEND]] = !batch && frame == 6 ? 1 :
                                             !batch && frame == 7 ? 2 : 0;
                unif[meta.offset[GT_CULL]] = !batch && frame == 4 ? 3 :
                                            !batch && frame == 8 ? 1 :
                                            !batch && frame == 9 ? 2 : 0;
            }
            g2d_pgl_req_t draw;
            g2d_pgl_draw_req_init(&draw,g2d_canvas(fb_id,bytes,w,h,1),
                g2d_canvas(pos_id,pos_bytes,vertices,texture ? 6 : 3,1),g2d_canvas(col_id,col_bytes,vertices,3,1),
                G2D_PGL_MODE_TRIANGLES,vertices,G2D_PGL_FMT_RGBA8,w*4,0,0,0,
                0x0201,1,0,g2d_canvas(z_id,bytes,w,h,1),w*4,
                batch || texture || frame == 0 || frame >= 4,0xff604020u,1.0f);
            if(texture) {
                draw.ctex[0] = g2d_canvas(tex_id,80,2,2,1);
                draw.ctex_w[0] = draw.ctex_h[0] = 2;
                draw.ctex_stride[0] = 8;
            }
            g2d_pgl_draw_req_set_shader(&draw,handle,unif_id,meta.uniform_words,frame == 0);
            uint32_t start, end;
            kernel_tic32(NULL,NULL,&start);
            int rc = g2d_pgl_draw(&draw);
            kernel_tic32(NULL,NULL,&end);
            unsigned elapsed = end-start;
            elapsed_total += elapsed;
            if(elapsed > elapsed_max) elapsed_max = elapsed;
            draws++;
            g2d_quiesce_on_fault(rc);
            if(rc) { printf("geometry frame %u: draw ret=%d\n",frame,rc); goto done; }
            if(check(batch ? batch_name : texture ? tex_names[frame] : names[frame],fb,z,w,h,
                     !batch && !texture && frame == 5 ? w/8 : 0,
                     !batch && !texture && (frame == 4 || frame >= 9),
                     batch ? (texture ? texels[2] : 0xff0000ffu) : texture ? texels[sample[frame]] : colors[frame],
                     batch ? 4194303u : texture ? 8388607u : depths[frame])) goto done;
            if(memcmp(pos,input_copy,pos_words*4) || memcmp(col,input_copy+pos_words,col_words*4) ||
               (texture && memcmp(tex,texels,16))) {
                printf("geometry: input changed\n"); goto done;
            }
            for(unsigned i = 0; i < 16; i++)
                if(pos[pos_words+i] != GUARD || col[col_words+i] != GUARD ||
                   (texture && tex[4+i] != GUARD)) {
                    printf("geometry: input guard changed\n"); goto done;
                }
        }
        printf("geometry %s %ux%u: %u draws, IPC+clear+transform+raster avg=%u max=%u us (tick clock)\n",
               batch ? "batch" : texture ? "texture" : "states",w,h,
               draws,elapsed_total/draws,elapsed_max);
        g2d_shm_free(fb); fb = NULL; g2d_shm_free(z); z = NULL;
    }
    ret = 0;
done:
    free(input_copy);
    if(f) fclose(f);
    if(blob) g2d_shm_free(blob);
    if(unif) g2d_shm_free(unif);
    if(fb) g2d_shm_free(fb);
    if(z) g2d_shm_free(z);
    if(pos) g2d_shm_free(pos);
    if(col) g2d_shm_free(col);
    if(tex) g2d_shm_free(tex);
    if(ret) printf("geometry: failed at %s\n",stage);
    printf("geometry: %s\n",ret?"FAIL":"PASS");
    return ret;
}
