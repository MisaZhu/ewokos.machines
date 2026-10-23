/* Bounded Q8B API conformance tests. Direct requests have independent pixel
 * oracles and no rendering fallback. Integration cases explicitly test graph
 * fallback separately. A decline is never counted as an accelerated PASS. */
static void api_reset(surface_t *s, uint32_t color) {
    for(unsigned i = 0; i < s->count; i++)
        s->p[i] = i < s->w*s->h ? color : GUARD;
}

static void api_pattern(surface_t *s, int alpha) {
    static const unsigned levels[] = {0, 1, 127, 128, 254, 255};
    for(unsigned y = 0; y < s->h; y++)
        for(unsigned x = 0; x < s->w; x++) {
            uint32_t c = pattern(x, y, NULL);
            if(alpha) c = (c & 0xffffffu) | (levels[(x+y)%6] << 24);
            s->p[y*s->w+x] = c;
        }
}

static int api_ret(const char *name, int got, int want) {
    checked(got);
    printf("API %s: ret=%d expected=%d %s\n", name, got, want,
           got == want ? "PASS" : "FAIL");
    return got != want;
}

static int api_pixels(const char *name, surface_t *s, const uint32_t *ref,
                      unsigned tolerance) {
    unsigned errors = 0, guards = 0;
    volatile uint32_t *p = s->p;
    for(unsigned i = 0; i < s->count; i++) {
        uint32_t want = i < s->w*s->h ? ref[i] : GUARD;
        uint32_t got = p[i];
        int bad = 0;
        for(unsigned shift = 0; shift < 32; shift += 8) {
            int diff = (int)((got >> shift)&255) - (int)((want >> shift)&255);
            unsigned limit = i >= s->w*s->h ? 0 : tolerance;
            if(diff < -(int)limit || diff > (int)limit) bad = 1;
        }
        if(bad) {
            if(i >= s->w*s->h) guards++;
            else if(errors++ == 0)
                printf("API %s first mismatch (%u,%u): %08x expected %08x\n",
                       name, i%s->w, i/s->w, got, want);
        }
    }
    printf("API %s readback: %s (%u pixels, %u guards)\n", name,
           errors || guards ? "FAIL" : "PASS", errors, guards);
    return errors || guards;
}

static uint32_t api_blend(uint32_t src, uint32_t dst, unsigned global) {
    unsigned sa=src>>24, da=dst>>24;
    if(global == 255 && sa == 255) return src;
    unsigned a = (sa*global)>>8;
    if(a == 0) return dst;
    if(da == 0) return (a<<24)|(src&0xffffffu);
    uint32_t out = (da+(255-da)*a/255)<<24;
    for(unsigned shift = 0; shift < 24; shift += 8)
        out |= (((((src>>shift)&255)*a + ((dst>>shift)&255)*(255-a))/255) << shift);
    return out;
}

/* Solid fill uses the color alpha directly; unlike BLIT_ALPHA it has no
   global-alpha product and therefore must not reduce alpha by one. */
static uint32_t api_solid_blend(uint32_t src, uint32_t dst) {
    unsigned a=src>>24, da=dst>>24;
    if(!a) return dst;
    if(a==255 || !da) return src;
    uint32_t out=(da+(255-da)*a/255)<<24;
    for(unsigned shift=0;shift<24;shift+=8)
        out|=(((((src>>shift)&255)*a+((dst>>shift)&255)*(255-a))/255)<<(shift));
    return out;
}

static int api_solid_alpha(surface_t *dst,uint32_t *ref) {
    static const unsigned alphas[]={0,1,127,128,254,255};
    static const uint32_t backgrounds[]={0xff204060u,0x80204060u,0x00204060u};
    g2d_fill_req_t fill;
    for(unsigned a=0;a<6;a++) for(unsigned b=0;b<3;b++) {
        api_reset(dst,backgrounds[b]);
        for(unsigned i=0;i<dst->w*dst->h;i++) ref[i]=backgrounds[b];
        uint32_t color=(alphas[a]<<24)|0xc03020u;
        for(unsigned y=5;y<28;y++) for(unsigned x=3;x<32;x++)
            ref[y*dst->w+x]=api_solid_blend(color,backgrounds[b]);
        g2d_fill_req_init(&fill,dst->canvas,g2d_rect(3,5,29,23),color);
        printf("API solid alpha=%u dst=%08x\n",alphas[a],backgrounds[b]);
        if(api_ret("solid alpha GPU",g2d_fill_rect(&fill),0) ||
           api_pixels("solid alpha exact crop",dst,ref,0)) return 1;
    }
    return 0;
}

/* The oracle retains the CPU's original 64-bit Q14 center calculation.
   The shader instead combines its shifts into bounded 32-bit arithmetic. */
static int api_rotations(void) {
    static const unsigned sizes[][2]={{64,48},{65,37},{1,1},{320,240}};
    static const int degrees[]={90,270,45,135,1,89,91,179,-45,450,180};
    unsigned cases=0;
    for(unsigned size=0;size<sizeof(sizes)/sizeof(sizes[0]);size++) {
        surface_t src={0};
        if(surface_new(&src,sizes[size][0],sizes[size][1])) return 1;
        api_pattern(&src,1);
        uint32_t *original=malloc(src.w*src.h*4);
        if(!original) { g2d_shm_free(src.p); return 1; }
        memcpy(original,src.p,src.w*src.h*4);
        for(unsigned angle=0;angle<sizeof(degrees)/sizeof(degrees[0]);angle++) {
            int degree=((degrees[angle]%360)+360)%360;
            double radians=degree*(3.14159265358979323846/180.0);
            int c=(int)round(cos(radians)*16384.0),s=(int)round(sin(radians)*16384.0);
            unsigned ac=c<0?-c:c,as=s<0?-s:s;
            unsigned w=(src.w*ac+src.h*as+16383u)>>14;
            unsigned h=(src.w*as+src.h*ac+16383u)>>14;
            surface_t dst={0};
            uint32_t *ref=NULL;
            int bad=1;
            if(surface_new(&dst,w,h)) goto rotation_done;
            ref=malloc(w*h*4);
            if(!ref) goto rotation_done;
            for(unsigned y=0;y<h;y++) for(unsigned x=0;x<w;x++) {
                int sx,sy;
                if(degree==90) { sx=y; sy=src.h-1-x; }
                else if(degree==270) { sx=src.w-1-y; sy=x; }
                else if(degree==180) { sx=src.w-1-x; sy=src.h-1-y; }
                else {
                    int64_t dx=(int64_t)x*16384-((int64_t)w-1)*8192;
                    int64_t dy=(int64_t)y*16384-((int64_t)h-1)*8192;
                    sx=((((dx*c+dy*s)>>14)+((int64_t)src.w-1)*8192+8192)>>14);
                    sy=((((dy*c-dx*s)>>14)+((int64_t)src.h-1)*8192+8192)>>14);
                }
                ref[y*w+x]=sx>=0 && sy>=0 && sx<(int)src.w && sy<(int)src.h?
                    original[sy*src.w+sx]:0;
            }
            g2d_rotate_req_t req;
            g2d_rotate_req_init(&req,src.canvas,dst.canvas,degrees[angle]);
            printf("API rotation %ux%u -> %ux%u angle=%d\n",src.w,src.h,w,h,degrees[angle]);
            if(api_ret("compute rotation",g2d_rotate(&req),0) ||
               api_pixels("rotation exact Q14",&dst,ref,0) ||
               api_pixels("rotation source unchanged",&src,original,0)) goto rotation_done;
            bad=0; cases++;
rotation_done:
            free(ref); g2d_shm_free(dst.p);
            if(bad) { free(original); g2d_shm_free(src.p); return 1; }
        }
        free(original); g2d_shm_free(src.p);
    }
    printf("API rotation matrix: PASS (%u exact cases)\n",cases);
    return 0;
}

static int api_alpha(surface_t *src, surface_t *dst, uint32_t *ref) {
    static const uint32_t backgrounds[]={0xff204060u,0x80204060u,0x00204060u};
    static const uint8_t alphas[]={1,128,254,255};
    g2d_blit_req_t req;
    api_pattern(src,1);
    g2d_blit_req_init(&req,dst->canvas,src->canvas,
        g2d_rect(0,0,64,64),g2d_rect(0,0,64,64),255);
    for(unsigned global=0;global<4;global++) for(unsigned test=0;test<3;test++) {
        req.alpha=alphas[global];
        api_reset(dst,backgrounds[test]);
        for(unsigned i=0;i<64*64;i++) ref[i]=api_blend(src->p[i],backgrounds[test],req.alpha);
        printf("API blend global=%u dst=%08x\n",req.alpha,backgrounds[test]);
        if(api_ret("source-over destination alpha",g2d_blit_alpha(&req),0) ||
           api_pixels("source-over opaque/partial/transparent dst",dst,ref,0)) return 1;
    }
    return 0;
}

/* Check the actual NEON fallback against the independent scalar oracle,
 * including mixed-alpha SIMD blocks, short tails and zero effective alpha. */
static int api_cpu_alpha(surface_t *src, surface_t *dst, uint32_t *ref) {
    static const unsigned widths[]={1,15,16,17,31,32,33,64};
    static const unsigned alphas[]={1,128,254,255};
    static const uint32_t backgrounds[]={0xff204060u,0x80204060u,0x00204060u};
    graph_t gs,gd;
    graph_init(&gs,src->p,64,64); gs.shm_id=-1; gs.shm_contig=0;
    graph_init(&gd,dst->p,64,64); gd.shm_id=-1; gd.shm_contig=0;
    api_pattern(src,1);
    unsigned cases=0;
    for(unsigned w=0;w<8;w++) for(unsigned a=0;a<4;a++) for(unsigned b=0;b<3;b++) {
        api_reset(dst,backgrounds[b]);
        for(unsigned i=0;i<64*64;i++) ref[i]=backgrounds[b];
        for(unsigned y=0;y<7;y++) for(unsigned x=0;x<widths[w];x++)
            ref[(y+5)*64+x]=api_blend(src->p[(y+2)*64+x],backgrounds[b],alphas[a]);
        graph_blt_alpha(&gs,0,2,widths[w],7,&gd,0,5,widths[w],7,alphas[a]);
        for(unsigned i=0;i<dst->count;i++) {
            uint32_t want=i<64*64?ref[i]:GUARD;
            if(dst->p[i]!=want) {
                printf("CPU alpha width=%u global=%u dst=%08x\n",widths[w],alphas[a],backgrounds[b]);
                return api_pixels("CPU alpha SIMD/tails",dst,ref,0);
            }
        }
        cases++;
    }
    printf("CPU alpha SIMD/tails: PASS (%u cases, zero pixel/guard errors)\n",cases);
    return 0;
}

static int api_basic(surface_t *src, surface_t *dst,
                     uint32_t *ref) {
    g2d_fill_req_t fill;
    g2d_blit_req_t blit;
    g2d_triangle_req_t tri;
    g2d_rotate_req_t rot;
    const unsigned n = dst->w*dst->h;
    api_reset(dst, 0xff204060u);
    memcpy(ref, dst->p, n*4);
    g2d_fill_req_init(&fill, dst->canvas, g2d_rect(-5,-7,23,29), 0xffa17c39u);
    for(unsigned y = 0; y < 22; y++)
        for(unsigned x = 0; x < 18; x++) ref[y*dst->w+x] = fill.color;
    if(api_ret("fill clipped", g2d_fill_rect(&fill), 0) ||
       api_pixels("fill clipped", dst, ref, 0)) return 1;
    fill.color = 0x00234567u;
    if(api_ret("fill transparent", g2d_fill_rect(&fill), 0) ||
       api_pixels("fill transparent no-write", dst, ref, 0)) return 1;
    fill.color = 0x80234567u;
    for(unsigned y=0;y<22;y++) for(unsigned x=0;x<18;x++)
        ref[y*dst->w+x]=api_solid_blend(fill.color,ref[y*dst->w+x]);
    if(api_ret("fill fractional GPU", g2d_fill_rect(&fill), 0) ||
       api_pixels("fill fractional exact", dst, ref, 0)) return 1;
    fill.dst.size = 4;
    if(api_ret("undersized canvas", g2d_fill_rect(&fill), -1) ||
       api_pixels("invalid canvas no-write", dst, ref, 0)) return 1;
    g2d_triangle_req_init(&tri, dst->canvas, 3,4,50,7,20,55,0xff123456u);
    if(api_ret("triangle decline", g2d_fill_triangle(&tri), -2) ||
       api_pixels("triangle no-write", dst, ref, 0)) return 1;
    tri.color = 0x00123456u;
    if(api_ret("triangle transparent", g2d_fill_triangle(&tri), 0) ||
       api_pixels("triangle transparent no-write", dst, ref, 0)) return 1;
    api_pattern(src, 1);
    g2d_blit_req_init(&blit, dst->canvas, src->canvas,
                     g2d_rect(0,0,64,64), g2d_rect(0,0,64,64), 0);
    if(api_ret("blit alpha zero", g2d_blit_alpha(&blit), 0) ||
       api_pixels("blit alpha zero no-write", dst, ref, 0)) return 1;
    blit.alpha = 128;
    for(unsigned i = 0; i < n; i++) ref[i] = api_blend(src->p[i], ref[i], 128);
    if(api_ret("blit global alpha", g2d_blit_alpha(&blit), 0) ||
       api_pixels("global alpha", dst, ref, 0)) return 1;
    blit.alpha = 255;
    for(unsigned i = 0; i < n; i++) ref[i] = api_blend(src->p[i], ref[i], 255);
    if(api_ret("blit source-over", g2d_blit_alpha(&blit), 0) ||
       api_pixels("source-over alpha endpoints", dst, ref, 0)) return 1;
    static const int rotations[]={0,360,-360,180,-180,540};
    for(unsigned test=0;test<6;test++) {
        int reverse=rotations[test]%360!=0;
        for(unsigned y=0;y<64;y++)
            for(unsigned x=0;x<64;x++)
                ref[y*64+x]=src->p[(reverse?63-y:y)*64+(reverse?63-x:x)];
        g2d_rotate_req_init(&rot,src->canvas,dst->canvas,rotations[test]);
        if(api_ret("rotate0/180 normalized",g2d_rotate(&rot),0) ||
           api_pixels("normalized rotation",dst,ref,0)) return 1;
    }
    rot.rotate=0; rot.dst.w=32;
    if(api_ret("identity rotation invalid size",g2d_rotate(&rot),-1) ||
       api_pixels("identity invalid size no-write",dst,ref,0)) return 1;
    for(int degree = 90; degree <= 270; degree += 180) {
        for(unsigned y=0;y<64;y++) for(unsigned x=0;x<64;x++)
            ref[y*64+x]=src->p[degree==90?(63-x)*64+y:x*64+63-y];
        g2d_rotate_req_init(&rot, src->canvas, dst->canvas, degree);
        if(api_ret("rotate90/270 GPU", g2d_rotate(&rot), 0) ||
           api_pixels("rotate90/270 exact", dst, ref, 0)) return 1;
    }
    /* A transient/sticky rejection of another operation must not disable fill. */
    g2d_fill_req_init(&fill, dst->canvas, g2d_rect(0,0,64,64), 0xff193b5du);
    for(unsigned i = 0; i < n; i++) ref[i] = fill.color;
    return api_ret("opaque fill after declines", g2d_fill_rect(&fill), 0) ||
           api_pixels("opaque fill after declines", dst, ref, 0);
}

static int api_physical(surface_t *src) {
    surface_t dst;
    uint32_t *ref;
    const unsigned pitch = 80, prefix = 64, dx = 11, dy = 13, sw = 29, sh = 17;
    g2d_blit_to_phy_req_t copy;
    g2d_rotate_to_phy_req_t rot;
    if(surface_new(&dst, pitch, 72)) return 1;
    ref = malloc(dst.w*dst.h*4);
    if(!ref) { g2d_shm_free(dst.p); return 1; }
    api_pattern(src, 0);
    for(unsigned pass = 0; pass < 2; pass++) {
        api_reset(&dst, GUARD);
        memcpy(ref, dst.p, dst.w*dst.h*4);
        for(unsigned y = 0; y < sh; y++)
            for(unsigned x = 0; x < sw; x++)
                ref[prefix+(dy+y)*pitch+dx+x] = pattern(
                    3+(pass ? sw-1-x : x), 5+(pass ? sh-1-y : y), NULL);
        int ret;
        if(pass == 0) {
            g2d_blit_to_phy_req_init(&copy, src->canvas, g2d_rect(3,5,sw,sh),
                dst.phy+prefix*4, pitch*64*4, 64,64,pitch*4,g2d_rect(dx,dy,sw,sh));
            ret = g2d_blit_to_phy(&copy);
        } else {
            g2d_rotate_to_phy_req_init(&rot, src->canvas, g2d_rect(3,5,sw,sh),
                dst.phy+prefix*4, pitch*64*4, 64,64,pitch*4,dx,dy,180);
            ret = g2d_rotate_to_phy(&rot);
        }
        if(api_ret(pass ? "rotate_to_phy180" : "blit_to_phy",ret,0) ||
           api_pixels("physical crop/prefix/padding/suffix", &dst,ref,0)) goto fail;
    }
    /* All invalid requests still name our allocation, never MMIO/scan-out. */
    copy.dst_size = 32;
    if(api_ret("physical short size",g2d_blit_to_phy(&copy),-1) ||
       api_pixels("physical short size no-write",&dst,ref,0)) goto fail;
    g2d_blit_to_phy_req_t bad_pitch=copy;
    bad_pitch.dst_size=pitch*64*4;
    bad_pitch.pitch=63*4;
    if(api_ret("physical invalid pitch",g2d_blit_to_phy(&bad_pitch),-1) ||
       api_pixels("invalid pitch no-write",&dst,ref,0)) goto fail;
    for(int degree = 90; degree <= 270; degree += 180) {
        rot.rotate = degree;
        if(api_ret("physical rotate90/270 decline",g2d_rotate_to_phy(&rot),-1) ||
           api_pixels("physical rotation no-write",&dst,ref,0)) goto fail;
    }
    if(surface_check("physical source unchanged",src,pattern,NULL)) goto fail;
    free(ref); g2d_shm_free(dst.p); return 0;
fail:
    free(ref); g2d_shm_free(dst.p); return 1;
}

static uint32_t api_slope(unsigned src, unsigned dst) {
    return dst <= 1 ? 0 : (((src-1)<<15)+(dst-2))/(dst-1);
}

static int api_scale(surface_t *src) {
    static const unsigned sizes[][2] = {{32,37},{96,83},{17,9},{16,1},{64,64}};
    api_pattern(src, 0);
    for(unsigned test = 0; test < sizeof(sizes)/sizeof(sizes[0]); test++) {
        surface_t dst;
        uint32_t *ref;
        g2d_scale_to_req_t req;
        if(surface_new(&dst,sizes[test][0],sizes[test][1])) return 1;
        ref = malloc(dst.w*dst.h*4);
        if(!ref) { g2d_shm_free(dst.p); return 1; }
        uint32_t xs = api_slope(src->w,dst.w), ys = api_slope(src->h,dst.h);
        for(unsigned y = 0; y < dst.h; y++)
            for(unsigned x = 0; x < dst.w; x++)
                ref[y*dst.w+x] = pattern((x*xs)>>15,(y*ys)>>15,NULL);
        g2d_scale_to_req_init(&req,src->canvas,dst.canvas);
        int bad = api_ret("scale_to nearest Q15",g2d_scale_to(&req),0) ||
                  api_pixels("scale nonuniform endpoints",&dst,ref,0);
        free(ref); g2d_shm_free(dst.p);
        if(bad) return 1;
    }
    /* Cropped BLIT uses rational, origin-based nearest mapping, distinct
       from SCALE_TO's corner-preserving Q15 mapping. */
    surface_t dst;
    g2d_blit_req_t req;
    if(surface_new(&dst,64,64)) return 1;
    uint32_t *ref=malloc(64*64*4);
    if(!ref) { g2d_shm_free(dst.p); return 1; }
    for(unsigned pass=0;pass<2;pass++) {
        unsigned sw=pass?23:51, sh=pass?17:43, dw=pass?47:29, dh=pass?39:19;
        api_reset(&dst,GUARD); memcpy(ref,dst.p,64*64*4);
        for(unsigned y=0;y<dh;y++)
            for(unsigned x=0;x<dw;x++)
                ref[(y+7)*64+x+9]=pattern(3+x*sw/dw,5+y*sh/dh,NULL);
        g2d_blit_req_init(&req,dst.canvas,src->canvas,
            g2d_rect(3,5,sw,sh),g2d_rect(9,7,dw,dh),255);
        int bad=api_ret("cropped scaled blit",g2d_blit(&req),0) ||
                api_pixels("cropped rational nearest",&dst,ref,0);
        if(bad) { free(ref); g2d_shm_free(dst.p); return 1; }
    }
    free(ref); g2d_shm_free(dst.p);
    return surface_check("scale source unchanged",src,pattern,NULL);
}

/* Generic shared canvases keep their logical width. Use 64-byte-pitch backing
 * canvases while retaining odd visible rectangles, so row padding and the
 * common allocator path are both exercised. */
static int api_blur_graph_caches(void) {
    static const int canvas_w[]={320,528,128};
    static const int visible_w[]={317,513,127};
    static const int heights[]={64,43,128};
    for(unsigned c=0;c<3;c++) {
        int cw=canvas_w[c],w=visible_w[c],h=heights[c],bad=0;
        graph_t *sharp=graph_new_shm(cw,h);
        graph_t *frost=graph_new_shm(cw,h);
        if(!sharp || !frost) {
            graph_free(sharp); graph_free(frost); return 1;
        }
        if(!frost->shm_contig || ((frost->w*4) & 63)) bad=1;
        for(int pass=0;pass<2 && !bad;pass++) {
            uint32_t color=pass?0xffabcdefu:0x80123456u;
            for(int i=0;i<cw*h;i++) sharp->buffer[i]=color;
            for(int i=0;i<frost->w*h;i++) frost->buffer[i]=GUARD;
            graph_blt(sharp,0,0,w,h,frost,0,0,w,h);
            uint32_t before=0,after=0;
            graph_g2d_blur_stats(&before,NULL,NULL,NULL,NULL,NULL);
            graph_gaussian_blur(frost,0,0,w,h,4);
            graph_g2d_blur_stats(&after,NULL,NULL,NULL,NULL,NULL);
            bad=after!=before+1;
            for(int y=0;y<h;y++) for(int x=0;x<frost->w;x++)
                bad+=frost->buffer[y*frost->w+x]!=(x<w?color:GUARD);
        }
        printf("integration frost cache %dx%d pitch=%d: %s\n",
               w,h,frost->w*4,bad?"FAIL":"PASS");
        graph_free(sharp); graph_free(frost);
        if(bad) return 1;
    }
    return 0;
}

/* Public graph entry points exercise actual client routing. CPU/arch outputs
 * are only references here; these checks are labelled integration, not GPU. */
static int api_integration(void) {
    surface_t src, dst;
    graph_t gs, gd, gr;
    uint32_t *ref;
    if(surface_new(&src,128,128)) return 1;
    if(surface_new(&dst,128,128)) { g2d_shm_free(src.p); return 1; }
    ref = malloc(128*128*4);
    if(!ref) { g2d_shm_free(src.p); g2d_shm_free(dst.p); return 1; }
    graph_init(&gs,src.p,128,128); gs.shm_id=src.canvas.shm_id; gs.shm_contig=1;
    graph_init(&gd,dst.p,128,128); gd.shm_id=dst.canvas.shm_id; gd.shm_contig=1;
    graph_init(&gr,ref,128,128);
    api_pattern(&src,0);
    api_reset(&dst,0xff204060u); memcpy(ref,dst.p,128*128*4);
    graph_fill_rect(&gd,0,0,128,128,0xff193b5du);
    for(unsigned i=0;i<128*128;i++) ref[i]=0xff193b5du;
    if(api_pixels("integration opaque graph fill",&dst,ref,0)) goto fail;
    graph_fill_rect(&gd,0,0,128,128,0x80234567u);
    graph_fill_rect(&gr,0,0,128,128,0x80234567u);
    if(api_pixels("integration fractional fill",&dst,ref,0)) goto fail;
    graph_fill_triangle(&gd,0,0,127,0,0,127,0xff345678u);
    graph_fill_triangle(&gr,0,0,127,0,0,127,0xff345678u);
    if(api_pixels("integration triangle fallback",&dst,ref,0)) goto fail;
    graph_blt_alpha(&gs,0,0,128,128,&gd,0,0,128,128,128);
    graph_t source_ref = gs; source_ref.shm_id=-1; source_ref.shm_contig=0;
    graph_blt_alpha(&source_ref,0,0,128,128,&gr,0,0,128,128,128);
    if(api_pixels("integration global alpha",&dst,ref,0)) goto fail;
    gs.shm_contig=0;
    graph_blt_alpha(&gs,0,0,128,128,&gd,0,0,128,128,128);
    graph_blt_alpha(&source_ref,0,0,128,128,&gr,0,0,128,128,128);
    gs.shm_contig=1;
    if(api_pixels("integration noncontig alpha fallback",&dst,ref,0)) goto fail;
    for(int degree=G_ROTATE_90;degree<=G_ROTATE_270;degree++) {
        graph_rotate_to(&gs,&gd,degree);
        graph_rotate_to(&source_ref,&gr,degree);
        if(api_pixels("integration graph rotation",&dst,ref,0)) goto fail;
    }
    /* GPU approximation and CPU convolution share DC/alpha preservation,
       while their non-constant pixel kernels intentionally differ. */
    api_reset(&dst,0x80123456u); memcpy(ref,dst.p,128*128*4);
    uint32_t blur_before=0,blur_after=0;
    graph_g2d_blur_stats(&blur_before,NULL,NULL,NULL,NULL,NULL);
    graph_gaussian_blur(&gd,0,0,128,128,2);
    graph_gaussian_blur(&gr,0,0,128,128,2);
    graph_g2d_blur_stats(&blur_after,NULL,NULL,NULL,NULL,NULL);
    if(blur_after!=blur_before+1 ||
       api_pixels("integration accelerated blur DC",&dst,ref,0)) goto fail;
    free(ref); g2d_shm_free(src.p); g2d_shm_free(dst.p);
    return api_blur_graph_caches();
fail:
    free(ref); g2d_shm_free(src.p); g2d_shm_free(dst.p); return 1;
}

#include "blur_tests.h"

static int api_tests(const char *mode) {
    int all = strcmp(mode,"--apis") == 0;
    int scale = all || strcmp(mode,"--api-scale") == 0;
    int physical = all || strcmp(mode,"--api-physical") == 0;
    int integration = all || strcmp(mode,"--api-integration") == 0;
    int alpha = all || strcmp(mode,"--api-alpha") == 0;
    int cpu_alpha = all || strcmp(mode,"--api-cpu-alpha") == 0;
    int raster = all || strcmp(mode,"--api-raster") == 0;
    int blur = all || strcmp(mode,"--api-blur") == 0;
    int stress = strcmp(mode,"--api-blur-stress") == 0;
    surface_t src={0},dst={0},tmp={0};
    uint32_t *ref = NULL, version=0, hz=0;
    int bad=1;
    if(!all && !scale && !physical && !integration && !alpha && !cpu_alpha && !raster && !blur && !stress) return 1;
    if(has_g2d() || api_ret("get_version",g2d_get_version(&version),0) ||
       version != 690 || api_ret("get_clock unknown",g2d_get_clock(&hz),-2) || hz != 0) return 1;
    printf("API device: version=%u clock=%u (0 means unknown)\n",version,hz);
    if(stress) return api_blur_stress();
    if(surface_new(&src,64,64) || surface_new(&dst,64,64) ||
       surface_new(&tmp,64,64)) goto done;
    ref = malloc(64*64*4);
    if(!ref) goto done;
    if(cpu_alpha && api_cpu_alpha(&src,&dst,ref)) goto done;
    if(raster && (api_solid_alpha(&dst,ref) || api_rotations())) goto done;
    if(all && api_basic(&src,&dst,ref)) goto done;
    if(blur && api_blurs()) goto done;
    if(alpha && api_alpha(&src,&dst,ref)) goto done;
    if(physical && api_physical(&src)) goto done;
    if(integration && api_integration()) goto done;
    if(scale && api_scale(&src)) goto done;
    bad=0;
done:
    free(ref);
    g2d_shm_free(src.p); g2d_shm_free(dst.p); g2d_shm_free(tmp.p);
    printf("gpusmoke %s: %s; accelerated / declined / fallback checks are separate\n",
           mode,bad?"FAIL":"PASS");
    return bad;
}
