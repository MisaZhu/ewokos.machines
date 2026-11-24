/*
 * portablegl geometry rasterizer, daemon half: the two-pass compute
 * rasterizer the other GPU back ends run (vertex transform into a `vout`
 * record buffer, then one thread per triangle sweeping its bounding box),
 * driven with the precompiled kernels in kernels/pgl_geom.cu.
 *
 * Client buffers arrive as physically contiguous shm; they are mapped into
 * the GPU VM on first sight and addressed in place (zero copy).
 */
#include <pthread.h>
#include <string.h>
#include <ewoksys/klog.h>
#include <ewoksys/kernel_tic.h>

#include <ga10b/ga10b_dev.h>
#include <ga10b/ga10b_pgl.h>

#include "kernels/pgl_geom_cubin.h"

#define PGL_MAX_VERTS   32768u            /* G2D_PGL_MAX_VERTS */
#define PGL_VOUT_BYTES  (PGL_MAX_VERTS * 48u)   /* widest record: 12 floats */
#define PGL_MAX_PROGS   64u               /* g2dd owner table size */
#define PGL_UNIF_WORDS  1024u
/* tile rasterizer state (kernels/pgl_geom.cu) */
#define PGL_TREC_BYTES  ((PGL_MAX_VERTS / 3u + 1u) * 160u)
#define PGL_MAX_TILES   65536u
#define PGL_LIST_WORDS  (1u << 20)        /* 4MB shared by all tiles */
#define PGL_CAP_MAX     255u
#define PGL_CAP_MIN     8u

static struct {
    int state;                            /* 0 unprobed, 1 ready, -1 failed */
    ga10b_module_t* mod;
    int k_fill, k_setup, k_sort, k_raster;
    int k_xform[GA10B_PGL_K_RASTER_TEXCOL + 1];   /* by GA10B_PGL_K_* id */
    void* vout_va;      uint64_t vout_gva;
    uint32_t* xu_va;    uint64_t xu_gva;  /* xform uniform block */
    uint32_t* ru_va;    uint64_t ru_gva;  /* raster uniform block */
    void* trec_va;      uint64_t trec_gva;   /* per-triangle records */
    uint32_t* cnt_va;   uint64_t cnt_gva;    /* per-tile list length */
    void* list_va;      uint64_t list_gva;   /* per-tile triangle lists */
    uint32_t* bp_va;    uint64_t bp_gva;     /* tile-pass parameters */
    struct {
        int used;
        ga10b_prog_blob_t b;
        /* fullscreen compute program (geom_kind == 0) */
        int is_compute;
        ga10b_module_t* mod;
        int kernel;
        uint32_t local_size[3];
        uint32_t unif_dwords;
    } prog[PGL_MAX_PROGS];
} pgl;

/* per-stage wall time per program, logged every PROF_EVERY of its draws */
#define PROF_EVERY 30u
static struct {
    int on;
    struct {
        uint32_t draws, prep_us, xform_us, setup_us, sort_us, raster_us;
        uint32_t flush_us;
    } p[PGL_MAX_PROGS];
} prof;

void ga10b_pgl_set_profile(int on) {
    prof.on = on;
}

static uint32_t prof_now(void) {
    uint64_t nsec = 0;
    if (prof.on)
        kernel_tic_nsec(&nsec);
    return (uint32_t)(nsec / 1000u);
}

static int pgl_do_init(void) {
    if (pgl.state != 0)
        return pgl.state == 1 ? 0 : -1;
    pgl.state = -1;
    if (ga10b_dev_open(NULL) != 0)
        return -1;
    pgl.mod = ga10b_dev_module(pgl_geom_cubin, sizeof(pgl_geom_cubin));
    if (pgl.mod == NULL)
        return -1;
    static const struct { uint32_t id; const char* name; } xf[] = {
        { GA10B_PGL_K_XFORM_COLOR, "pgl_xform_color" },
        { GA10B_PGL_K_XFORM_LIT, "pgl_xform_lit" },
        { GA10B_PGL_K_XFORM_TEX, "pgl_xform_tex" },
        { GA10B_PGL_K_XFORM_TEXCOL, "pgl_xform_texcol" },
        { GA10B_PGL_K_XFORM_INST, "pgl_xform_inst" },
        { GA10B_PGL_K_XFORM_INSTLIT, "pgl_xform_instlit" },
    };
    pgl.k_fill = ga10b_module_kernel(pgl.mod, "pgl_fill");
    pgl.k_setup = ga10b_module_kernel(pgl.mod, "pgl_tri_setup");
    pgl.k_sort = ga10b_module_kernel(pgl.mod, "pgl_bin_sort");
    pgl.k_raster = ga10b_module_kernel(pgl.mod, "pgl_raster_tile");
    if (pgl.k_fill < 0 || pgl.k_setup < 0 || pgl.k_sort < 0 ||
        pgl.k_raster < 0)
        return -1;
    for (uint32_t i = 0; i < sizeof(xf) / sizeof(xf[0]); i++) {
        pgl.k_xform[xf[i].id] = ga10b_module_kernel(pgl.mod, xf[i].name);
        if (pgl.k_xform[xf[i].id] < 0)
            return -1;
    }
    void *xu, *ru, *cnt, *bp;
    if (ga10b_dev_alloc(PGL_VOUT_BYTES, &pgl.vout_va, &pgl.vout_gva) != 0 ||
        ga10b_dev_alloc(PGL_UNIF_WORDS * 4u, &xu, &pgl.xu_gva) != 0 ||
        ga10b_dev_alloc(PGL_UNIF_WORDS * 4u, &ru, &pgl.ru_gva) != 0 ||
        ga10b_dev_alloc(PGL_TREC_BYTES, &pgl.trec_va, &pgl.trec_gva) != 0 ||
        ga10b_dev_alloc(PGL_MAX_TILES * 4u, &cnt, &pgl.cnt_gva) != 0 ||
        ga10b_dev_alloc(PGL_LIST_WORDS * 4u, &pgl.list_va,
                        &pgl.list_gva) != 0 ||
        ga10b_dev_alloc(4096, &bp, &pgl.bp_gva) != 0)
        return -1;
    pgl.xu_va = xu;
    pgl.ru_va = ru;
    pgl.cnt_va = cnt;
    pgl.bp_va = bp;
    pgl.state = 1;
    klog("ga10b: pgl tile rasterizer ready\n");
    return 0;
}

int ga10b_pgl_ready(void) {
    return pgl.state == 1;
}

static int pgl_launch2(int kernel, const uint32_t g[3], const uint32_t b[3],
                       void* const* args) {
    if (ga10b_dev_launch(pgl.mod, kernel, g, b, args) != 0) {
        /* a launch that did not retire leaves the engine in an unknown
         * state: stop offloading for the rest of the session */
        pgl.state = -1;
        klog("ga10b: pgl launch failed - 3D offload disabled\n");
        return GA10B_PGL_E_TIMEOUT;
    }
    return GA10B_PGL_OK;
}

/* 1-D launch covering `items` threads in blocks of `block` */
static int pgl_launch(int kernel, uint32_t items, uint32_t block,
                      void* const* args) {
    const uint32_t g[3] = { (items + block - 1u) / block, 1, 1 };
    const uint32_t b[3] = { block, 1, 1 };
    return pgl_launch2(kernel, g, b, args);
}

static int pgl_fill(uint64_t gva, uint32_t words, uint32_t value) {
    void* args[3] = { &gva, &value, &words };
    return pgl_launch(pgl.k_fill, words, 256u, args);
}

static int pgl_do_clear(uint64_t fb_phy, uint32_t w, uint32_t h, uint32_t stride,
                    uint32_t argb) {
    if (pgl.state != 1)
        return GA10B_PGL_E_UNSUPPORTED;
    if (w == 0 || h == 0 || stride != w * 4u)
        return GA10B_PGL_E_DECLINED;
    uint64_t fb = ga10b_dev_map(fb_phy, stride * h);
    if (fb == 0)
        return GA10B_PGL_E_DECLINED;
    ga10b_dev_sync_to_gpu();
    int rc = pgl_fill(fb, w * h, argb);
    ga10b_dev_sync_to_cpu();
    return rc;
}

static int pgl_do_program_load(const void* blob, uint32_t size) {
    if (pgl.state != 1)
        return GA10B_PGL_E_UNSUPPORTED;
    if (blob == NULL || size < sizeof(ga10b_prog_blob_t))
        return GA10B_PGL_E_UNSUPPORTED;
    ga10b_prog_blob_t b;
    memcpy(&b, blob, sizeof(b));
    if (b.magic != GA10B_PROG_MAGIC || b.version != GA10B_PROG_VERSION)
        return GA10B_PGL_E_UNSUPPORTED;
    if (b.geom_kind == 0) {
        /* fullscreen compute: a compiler-generated cubin */
        if (size < sizeof(ga10b_compute_blob_t))
            return GA10B_PGL_E_UNSUPPORTED;
        ga10b_compute_blob_t cb;
        memcpy(&cb, blob, sizeof(cb));
        if (cb.cubin_size == 0 || size < sizeof(cb) + cb.cubin_size)
            return GA10B_PGL_E_UNSUPPORTED;
        ga10b_module_t* mod = ga10b_dev_module((const uint8_t*)blob + sizeof(cb),
                                               cb.cubin_size);
        if (mod == NULL)
            return GA10B_PGL_E_UNSUPPORTED;
        int kernel = ga10b_module_kernel(mod, "pgl_cs");
        if (kernel < 0)
            return GA10B_PGL_E_UNSUPPORTED;
        for (uint32_t i = 0; i < PGL_MAX_PROGS; i++) {
            if (pgl.prog[i].used)
                continue;
            memset(&pgl.prog[i], 0, sizeof(pgl.prog[i]));
            pgl.prog[i].used = 1;
            pgl.prog[i].is_compute = 1;
            pgl.prog[i].mod = mod;
            pgl.prog[i].kernel = kernel;
            memcpy(pgl.prog[i].local_size, cb.local_size,
                   sizeof(cb.local_size));
            pgl.prog[i].unif_dwords = cb.unif_dwords;
            return (int)i;
        }
        return GA10B_PGL_E_DECLINED;
    }
    /* a kind fixes which kernel pair it may name and how much of the
     * uniform block each one reads */
    uint32_t xf_min = 0, rs_min = 0;
    int ok = 0;
    switch (b.geom_kind) {
    case GA10B_PGL_KIND_GOURAUD:
        ok = b.raster_kernel == GA10B_PGL_K_RASTER &&
             (b.xform_kernel == GA10B_PGL_K_XFORM_COLOR ||
              b.xform_kernel == GA10B_PGL_K_XFORM_LIT);
        xf_min = b.xform_kernel == GA10B_PGL_K_XFORM_LIT ? GA10B_PGL_XL_DWORDS
                                                         : GA10B_PGL_XF_DWORDS;
        rs_min = GA10B_PGL_RS_DWORDS;
        break;
    case GA10B_PGL_KIND_TEX:
        ok = b.raster_kernel == GA10B_PGL_K_RASTER_TEX &&
             b.xform_kernel == GA10B_PGL_K_XFORM_TEX;
        xf_min = GA10B_PGL_XF_VERT_COUNT + 1u;
        rs_min = GA10B_PGL_RT_TEX_H + 1u;
        break;
    case GA10B_PGL_KIND_INST:
        ok = b.raster_kernel == GA10B_PGL_K_RASTER &&
             (b.xform_kernel == GA10B_PGL_K_XFORM_INST ||
              b.xform_kernel == GA10B_PGL_K_XFORM_INSTLIT);
        xf_min = GA10B_PGL_XI_VERT_COUNT + 1u;
        rs_min = GA10B_PGL_RS_DWORDS;
        break;
    case GA10B_PGL_KIND_TEXCOL:
        ok = b.raster_kernel == GA10B_PGL_K_RASTER_TEXCOL &&
             b.xform_kernel == GA10B_PGL_K_XFORM_TEXCOL;
        xf_min = GA10B_PGL_XF_VERT_COUNT + 1u;
        rs_min = GA10B_PGL_RTC_DWORDS;
        break;
    default:
        break;
    }
    if (!ok || b.xform_unif_dwords < xf_min || b.raster_unif_dwords < rs_min ||
        b.xform_unif_dwords > PGL_UNIF_WORDS ||
        b.raster_unif_dwords > PGL_UNIF_WORDS)
        return GA10B_PGL_E_UNSUPPORTED;
    for (uint32_t i = 0; i < PGL_MAX_PROGS; i++) {
        if (pgl.prog[i].used)
            continue;
        memset(&pgl.prog[i], 0, sizeof(pgl.prog[i]));
        pgl.prog[i].used = 1;
        pgl.prog[i].b = b;
        return (int)i;
    }
    return GA10B_PGL_E_DECLINED;
}

static int pgl_do_program_free(int handle) {
    if (handle < 0 || (uint32_t)handle >= PGL_MAX_PROGS ||
        !pgl.prog[handle].used)
        return GA10B_PGL_E_UNSUPPORTED;
    pgl.prog[handle].used = 0;
    return GA10B_PGL_OK;
}

static int pgl_do_draw(int handle, const ga10b_pgl_draw_t* d,
                   const uint32_t* unif, uint32_t num_uniforms) {
    if (pgl.state != 1)
        return GA10B_PGL_E_UNSUPPORTED;
    if (handle < 0 || (uint32_t)handle >= PGL_MAX_PROGS ||
        !pgl.prog[handle].used)
        return GA10B_PGL_E_UNSUPPORTED;
    if (pgl.prog[handle].is_compute) {
        /* fullscreen compute: one thread per pixel, output to SSBO block 0 */
        if (d->w == 0 || d->h == 0 || d->stride < d->w * 4u ||
            (d->stride & 3u) != 0)
            return GA10B_PGL_E_DECLINED;
        uint64_t fb = ga10b_dev_map(d->fb_phy, d->stride * d->h);
        if (fb == 0)
            return GA10B_PGL_E_DECLINED;
        uint32_t lx = pgl.prog[handle].local_size[0] ?
                      pgl.prog[handle].local_size[0] : 32u;
        uint32_t ly = pgl.prog[handle].local_size[1] ?
                      pgl.prog[handle].local_size[1] : 8u;
        const uint32_t grid[3] = { (d->w + lx - 1u) / lx,
                                   (d->h + ly - 1u) / ly, 1 };
        const uint32_t block[3] = { lx, ly, 1 };
        uint32_t t0 = prof_now();
        ga10b_dev_sync_to_gpu();
        uint32_t t1 = prof_now();
        int rc = ga10b_dev_launch_compute(pgl.prog[handle].mod,
                                          pgl.prog[handle].kernel, grid, block,
                                          unif, num_uniforms, &fb, 1);
        uint32_t t2 = prof_now();
        ga10b_dev_sync_to_cpu();
        if (prof.on) {
            prof.p[handle].prep_us += t1 - t0;
            prof.p[handle].raster_us += t2 - t1;
            prof.p[handle].flush_us += prof_now() - t2;
            if (++prof.p[handle].draws == PROF_EVERY) {
                klog("ga10b: pgl prof h%d compute %ux%u local %ux%u: "
                     "sync=%u launch=%u flush=%u us\n",
                     handle, d->w, d->h, lx, ly,
                     prof.p[handle].prep_us / PROF_EVERY,
                     prof.p[handle].raster_us / PROF_EVERY,
                     prof.p[handle].flush_us / PROF_EVERY);
                memset(&prof.p[handle], 0, sizeof(prof.p[handle]));
            }
        }
        return rc == 0 ? GA10B_PGL_OK : GA10B_PGL_E_TIMEOUT;
    }
    const ga10b_prog_blob_t* p = &pgl.prog[handle].b;
    uint32_t split = p->xform_unif_dwords;
    uint32_t ntris = d->nverts / 3u;
    uint32_t kind = p->geom_kind;
    int textured = kind == GA10B_PGL_KIND_TEX || kind == GA10B_PGL_KIND_TEXCOL;
    int use_depth = kind != GA10B_PGL_KIND_TEXCOL;
    int use_attr1 = kind == GA10B_PGL_KIND_GOURAUD ||
                    kind == GA10B_PGL_KIND_INST;

    if (d->nverts == 0 || d->nverts % 3u != 0 || d->nverts > PGL_MAX_VERTS)
        return GA10B_PGL_E_DECLINED;
    if (d->w == 0 || d->h == 0 || d->stride < d->w * 4u ||
        (d->stride & 3u) != 0)
        return GA10B_PGL_E_DECLINED;
    /* the raster kernels index colour and depth with one uFbStride */
    if (use_depth && (d->z_phy == 0 || d->z_stride != d->stride))
        return GA10B_PGL_E_DECLINED;
    if (d->pos_phy == 0 || (use_attr1 && d->col_phy == 0))
        return GA10B_PGL_E_DECLINED;
    if (unif == NULL || split > num_uniforms ||
        num_uniforms - split < p->raster_unif_dwords)
        return GA10B_PGL_E_DECLINED;
    const uint32_t* ru = unif + split;
    /* sampler unit 0: a linear w*4-pitch image the kernel indexes with the
     * uTexW/uTexH the client patched, or the instance record stream */
    if (textured &&
        (d->ctex_phy == 0 || d->ctex_w == 0 || d->ctex_h == 0 ||
         d->ctex_stride != d->ctex_w * 4u ||
         ru[GA10B_PGL_RT_TEX_W] != d->ctex_w ||
         ru[GA10B_PGL_RT_TEX_H] != d->ctex_h))
        return GA10B_PGL_E_DECLINED;
    if (kind == GA10B_PGL_KIND_INST &&
        (d->ctex_phy == 0 || d->ctex_stride == 0 || d->ctex_h == 0))
        return GA10B_PGL_E_DECLINED;

    uint32_t tp = prof_now();
    uint32_t tiles_x = (d->w + GA10B_PGL_TILE - 1u) / GA10B_PGL_TILE;
    uint32_t tiles_y = (d->h + GA10B_PGL_TILE - 1u) / GA10B_PGL_TILE;
    uint32_t tiles = tiles_x * tiles_y;
    if (tiles > PGL_MAX_TILES)
        return GA10B_PGL_E_DECLINED;
    uint32_t cap = PGL_LIST_WORDS / tiles;
    if (cap > PGL_CAP_MAX)
        cap = PGL_CAP_MAX;
    if (cap < PGL_CAP_MIN)
        return GA10B_PGL_E_DECLINED;

    /* vertex stream strides (bytes): gouraud pos.xyz, textured pos+uv+pad,
     * instanced pos.xyz+index, colour-modulated pos+uv+rgba+pad */
    static const uint32_t pos_stride[5] = { 0, 12, 24, 16, 40 };
    uint64_t fb = ga10b_dev_map(d->fb_phy, d->stride * d->h);
    uint64_t z = use_depth ? ga10b_dev_map(d->z_phy, d->z_stride * d->h) : fb;
    uint64_t pos = ga10b_dev_map(d->pos_phy, d->nverts * pos_stride[kind]);
    uint64_t col = pgl.bp_gva, ctex = pgl.bp_gva;   /* unused: any valid VA */
    if (use_attr1) {
        /* attribute 1: normals / rgb (12 B/vert) or rgba (16, uColAlpha) */
        uint32_t bytes = 12u;
        if (p->xform_kernel == GA10B_PGL_K_XFORM_COLOR &&
            unif[GA10B_PGL_XF_COL_ALPHA] != 0u)
            bytes = 16u;
        col = ga10b_dev_map(d->col_phy, d->nverts * bytes);
    }
    if (textured || kind == GA10B_PGL_KIND_INST)
        ctex = ga10b_dev_map(d->ctex_phy, d->ctex_stride * d->ctex_h);
    if (fb == 0 || z == 0 || pos == 0 || col == 0 || ctex == 0)
        return GA10B_PGL_E_DECLINED;

    /* clear_rgba is portablegl's 0xAABBGGRR, the surface 0xAARRGGBB */
    uint32_t c = d->clear_rgba;
    uint32_t px = (c & 0xff000000u) | ((c & 0x000000ffu) << 16) |
                  (c & 0x0000ff00u) | ((c & 0x00ff0000u) >> 16);
    if (ga10b_pgl_fill_bp(pgl.bp_va, p->raster_kernel, ru, tiles_x, tiles_y,
                          cap, d->first_frame, px) != 0)
        return GA10B_PGL_E_DECLINED;
    memcpy(pgl.xu_va, unif, split * 4u);
    memcpy(pgl.ru_va, ru, p->raster_unif_dwords * 4u);
    memset(pgl.cnt_va, 0, tiles * 4u);
    __asm__ volatile("dsb sy" ::: "memory");
    ga10b_dev_sync_to_gpu();

    int rc;
    uint32_t t0 = prof_now();
    void* xa[5];
    uint32_t na = 0;
    xa[na++] = &pgl.vout_gva;
    xa[na++] = &pos;
    if (use_attr1)
        xa[na++] = &col;
    if (kind == GA10B_PGL_KIND_INST)
        xa[na++] = &ctex;
    xa[na++] = &pgl.xu_gva;
    if ((rc = pgl_launch(pgl.k_xform[p->xform_kernel], d->nverts, 64u,
                         xa)) != 0)
        goto out;
    uint32_t t1 = prof_now();
    void* sa[6] = { &pgl.trec_gva, &pgl.cnt_gva, &pgl.list_gva, &pgl.vout_gva,
                    &pgl.ru_gva, &pgl.bp_gva };
    if ((rc = pgl_launch(pgl.k_setup, ntris, 256u, sa)) != 0)
        goto out;
    uint32_t t2 = prof_now();
    void* oa[3] = { &pgl.cnt_gva, &pgl.list_gva, &pgl.bp_gva };
    if ((rc = pgl_launch(pgl.k_sort, tiles, 256u, oa)) != 0)
        goto out;
    uint32_t t3 = prof_now();
    void* ra[8] = { &fb, &z, &pgl.trec_gva, &pgl.cnt_gva, &pgl.list_gva,
                    &ctex, &pgl.ru_gva, &pgl.bp_gva };
    const uint32_t rg[3] = { tiles_x, tiles_y, 1 };
    const uint32_t rb[3] = { GA10B_PGL_TILE, GA10B_PGL_TILE, 1 };
    rc = pgl_launch2(pgl.k_raster, rg, rb, ra);
    if (prof.on) {
        uint32_t t4 = prof_now();
        ga10b_dev_sync_to_cpu();
        prof.p[handle].flush_us += prof_now() - t4;
        prof.p[handle].prep_us += t0 - tp;
        prof.p[handle].xform_us += t1 - t0;
        prof.p[handle].setup_us += t2 - t1;
        prof.p[handle].sort_us += t3 - t2;
        prof.p[handle].raster_us += t4 - t3;
        if (++prof.p[handle].draws == PROF_EVERY) {
            klog("ga10b: pgl prof h%d kind %u %ux%u %u tris clr %u: prep=%u "
                 "xform=%u setup=%u sort=%u raster=%u flush=%u us\n",
                 handle, kind, d->w, d->h, ntris, d->first_frame,
                 prof.p[handle].prep_us / PROF_EVERY,
                 prof.p[handle].xform_us / PROF_EVERY,
                 prof.p[handle].setup_us / PROF_EVERY,
                 prof.p[handle].sort_us / PROF_EVERY,
                 prof.p[handle].raster_us / PROF_EVERY,
                 prof.p[handle].flush_us / PROF_EVERY);
            memset(&prof.p[handle], 0, sizeof(prof.p[handle]));
        }
        return rc;
    }
out:
    /* also after a failed launch: earlier passes may have left dirty lines */
    ga10b_dev_sync_to_cpu();
    return rc;
}

/* g2dd runs its request handlers on concurrent IPC worker threads; the
 * device has one channel, one pushbuffer and one set of scratch buffers,
 * so every entry point is serialized. */
static pthread_mutex_t pgl_lock = PTHREAD_MUTEX_INITIALIZER;

int ga10b_pgl_init(void) {
    pthread_mutex_lock(&pgl_lock);
    int r = pgl_do_init();
    pthread_mutex_unlock(&pgl_lock);
    return r;
}

int ga10b_pgl_clear(uint64_t fb_phy, uint32_t w, uint32_t h, uint32_t stride,
                    uint32_t argb) {
    pthread_mutex_lock(&pgl_lock);
    int r = pgl_do_clear(fb_phy, w, h, stride, argb);
    pthread_mutex_unlock(&pgl_lock);
    return r;
}

int ga10b_pgl_program_load(const void* blob, uint32_t size) {
    pthread_mutex_lock(&pgl_lock);
    int r = pgl_do_program_load(blob, size);
    pthread_mutex_unlock(&pgl_lock);
    return r;
}

int ga10b_pgl_program_free(int handle) {
    pthread_mutex_lock(&pgl_lock);
    int r = pgl_do_program_free(handle);
    pthread_mutex_unlock(&pgl_lock);
    return r;
}

int ga10b_pgl_draw(int handle, const ga10b_pgl_draw_t* d,
                   const uint32_t* unif, uint32_t num_uniforms) {
    pthread_mutex_lock(&pgl_lock);
    int r = pgl_do_draw(handle, d, unif, num_uniforms);
    pthread_mutex_unlock(&pgl_lock);
    return r;
}

