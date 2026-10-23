/* Throughput-first blur acceptance: effect, DC/alpha, crop and DMA bounds.
 * No comparison to a different CPU convolution kernel is made. */
static int api_blur_bounds(surface_t *dst, surface_t *tmp, const uint32_t *before,
                           unsigned x, unsigned y, unsigned w, unsigned h) {
    unsigned bad=0;
    for(unsigned i=0;i<dst->count;i++) {
        if(i>=dst->w*dst->h) bad+=dst->p[i]!=GUARD;
        else if(i%dst->w<x || i%dst->w>=x+w || i/dst->w<y || i/dst->w>=y+h)
            bad+=dst->p[i]!=before[i];
    }
    unsigned limit=(h-1)*dst->w+w;
    for(unsigned i=limit;i<tmp->count;i++) bad+=tmp->p[i]!=GUARD;
    printf("API blur crop/scratch/16KiB guards: %s (%u errors)\n",bad?"FAIL":"PASS",bad);
    return bad!=0;
}

static int api_blurs(void) {
    static const unsigned sizes[][2]={{64,48},{80,37},{320,200},{720,720}};
    static const int radii[]={1,2,3,4,6,16,64};
    static const uint32_t colors[]={0x00000000,0x01234567,0x80123456,0xffabcdef};
    unsigned cases=0;
    for(unsigned size=0;size<4;size++) {
        surface_t dst={0},tmp={0};
        uint32_t *before=NULL;
        int bad=1;
        if(surface_new(&dst,sizes[size][0],sizes[size][1]) ||
           surface_new(&tmp,dst.w,dst.h)) goto blur_done;
        before=malloc(dst.w*dst.h*4);
        if(!before) goto blur_done;
        unsigned previous_spread=0;
        for(unsigned r=0;r<7;r++) {
            /* A broad edge checks locality, strength and alpha: a global
               average would pass the checkerboard but fails here. */
            api_reset(&dst,GUARD); api_reset(&tmp,GUARD);
            for(unsigned yy=0;yy<dst.h;yy++) for(unsigned xx=0;xx<dst.w;xx++)
                dst.p[yy*dst.w+xx]=xx<dst.w/2?0x80000000u:0x80ffffffu;
            memcpy(before,dst.p,dst.w*dst.h*4);
            g2d_gaussian_blur_req_t edge;
            g2d_gaussian_blur_req_init(&edge,dst.canvas,tmp.canvas,
                g2d_rect(0,0,dst.w,dst.h),radii[r]);
            if(api_ret("edge blur GPU",g2d_gaussian_blur(&edge),0) ||
               api_blur_bounds(&dst,&tmp,before,0,0,dst.w,dst.h)) goto blur_done;
            unsigned spread=0,previous=0;
            for(unsigned xx=0;xx<dst.w;xx++) {
                uint32_t p=dst.p[(dst.h/2)*dst.w+xx];
                unsigned v=p&255;
                if((p>>24)!=128 || ((p>>8)&255)!=v || ((p>>16)&255)!=v ||
                   (xx && v+1<previous)) goto blur_done;
                previous=v; spread+=v>8 && v<247;
            }
            if(!spread || spread<previous_spread ||
               (dst.p[(dst.h/2)*dst.w]&255)>8 ||
               (dst.p[(dst.h/2)*dst.w+dst.w-1]&255)<247) {
                printf("API blur locality/radius: FAIL (spread=%u previous=%u)\n",spread,previous_spread);
                goto blur_done;
            }
            previous_spread=spread;
            printf("API blur locality/radius: PASS (r=%d transition=%u pixels)\n",radii[r],spread);
            cases++;
        }
        for(unsigned r=0;r<7;r++) for(unsigned crop=0;crop<3;crop++) {
            unsigned x=crop==1?3:0, y=crop==1?5:0;
            unsigned w=crop==0?dst.w:dst.w-7, h=crop==0?dst.h:dst.h-9;
            /* Negative origins must clip without shifting the output. */
            g2d_rect_t rect=crop==2?g2d_rect(-3,-5,w+3,h+5):g2d_rect(x,y,w,h);
            g2d_gaussian_blur_req_t req;
            /* Constants check every channel, including non-opaque alpha. */
            for(unsigned c=0;c<4;c++) {
                api_reset(&dst,GUARD); api_reset(&tmp,GUARD);
                for(unsigned yy=y;yy<y+h;yy++) for(unsigned xx=x;xx<x+w;xx++)
                    dst.p[yy*dst.w+xx]=colors[c];
                memcpy(before,dst.p,dst.w*dst.h*4);
                g2d_canvas_t scratch=tmp.canvas;
                scratch.w=w; scratch.h=h; scratch.size=((h-1)*dst.w+w)*4;
                g2d_gaussian_blur_req_init(&req,dst.canvas,scratch,rect,radii[r]);
                printf("API fast blur %ux%u crop=%u r=%d DC=%08x\n",dst.w,dst.h,crop,radii[r],colors[c]);
                if(api_ret("fast blur GPU",g2d_gaussian_blur(&req),0) ||
                   api_pixels("blur DC/alpha",&dst,before,0) ||
                   api_blur_bounds(&dst,&tmp,before,x,y,w,h)) goto blur_done;
                cases++;
            }
            /* A one-pixel checkerboard must lose high-frequency contrast.
               Mean/alpha bounds reject black output or a channel swap. */
            api_reset(&dst,GUARD); api_reset(&tmp,GUARD);
            for(unsigned yy=y;yy<y+h;yy++) for(unsigned xx=x;xx<x+w;xx++)
                dst.p[yy*dst.w+xx]=((xx+yy)&1)?0xffe0a060u:0xff204080u;
            memcpy(before,dst.p,dst.w*dst.h*4);
            if(api_ret("checkerboard blur GPU",g2d_gaussian_blur(&req),0) ||
               api_blur_bounds(&dst,&tmp,before,x,y,w,h)) goto blur_done;
            uint64_t contrast=0, red=0, green=0, blue=0;
            unsigned n=0,alpha_bad=0;
            for(unsigned yy=y+2;yy<y+h-2;yy++) for(unsigned xx=x+2;xx<x+w-2;xx++) {
                uint32_t p=dst.p[yy*dst.w+xx];
                int v=(p>>16)&255;
                contrast+=v>128?v-128:128-v;
                red+=v; green+=(p>>8)&255; blue+=p&255;
                alpha_bad+=(p>>24)!=255; n++;
            }
            int effect_bad=!n || alpha_bad || contrast>n*72u ||
                red<n*96u || red>n*160u || green<n*80u || green>n*144u ||
                blue<n*80u || blue>n*144u;
            printf("API blur checker attenuation: %s (contrast=%u mean RGB=%u,%u,%u)\n",
                effect_bad?"FAIL":"PASS",n?(unsigned)(contrast/n):0,
                n?(unsigned)(red/n):0,n?(unsigned)(green/n):0,n?(unsigned)(blue/n):0);
            if(effect_bad) goto blur_done;
            cases++;
        }
        /* Invalid requests must leave both surfaces intact, and a later
           valid request must still accelerate (FAILED is not sticky). */
        api_pattern(&dst,1); api_reset(&tmp,GUARD);
        memcpy(before,dst.p,dst.w*dst.h*4);
        for(unsigned kind=0;kind<9;kind++) {
            g2d_gaussian_blur_req_t req;
            g2d_gaussian_blur_req_init(&req,dst.canvas,tmp.canvas,
                g2d_rect(0,0,dst.w,dst.h),2);
            if(kind==0) req.radius=0;
            if(kind==1) req.radius=65;
            if(kind==2) req.tmp=dst.canvas;
            if(kind==3) req.tmp.size=4;
            if(kind==4) req.tmp.shm_id=0;
            if(kind==5) req.rect=g2d_rect(0,0,1,1);
            if(kind==6) req.rect=g2d_rect(0x7fffffff,0,2,2);
            if(kind==7) req.rect=g2d_rect(-2147483647-1,0,1,2);
            if(kind==8) { req.dst.w=dst.w-1; req.rect.w=dst.w-1; }
            if(api_ret("invalid fast blur",g2d_gaussian_blur(&req),-1) ||
               api_pixels("invalid blur no-write",&dst,before,0)) goto blur_done;
            uint32_t guard=GUARD;
            if(surface_check("invalid scratch unchanged",&tmp,solid,&guard)) goto blur_done;
        }
        g2d_gaussian_blur_req_t req;
        api_reset(&dst,0x80123456); memcpy(before,dst.p,dst.w*dst.h*4);
        g2d_gaussian_blur_req_init(&req,dst.canvas,tmp.canvas,g2d_rect(0,0,dst.w,dst.h),2);
        if(api_ret("blur after declines",g2d_gaussian_blur(&req),0) ||
           api_pixels("blur after declines DC",&dst,before,0)) goto blur_done;
        static const int perf_radii[]={2,4,16};
        for(unsigned r=0;r<3;r++) {
            uint32_t start=0,end=0;
            req.radius=perf_radii[r];
            kernel_tic32(NULL,NULL,&start);
            for(unsigned frame=0;frame<16;frame++) if(checked(g2d_gaussian_blur(&req))) goto blur_done;
            kernel_tic32(NULL,NULL,&end);
            printf("PERF fast blur %ux%u r%d: %u us/frame (16 IPC requests)\n",
                dst.w,dst.h,req.radius,(end-start)/16);
        }
        bad=0;
blur_done:
        free(before); g2d_shm_free(dst.p); g2d_shm_free(tmp.p);
        if(bad) return 1;
    }
    printf("API fast Gaussian-style blur: PASS (%u effect/DC cases plus invalid requests)\n",cases);
    return 0;
}

/* Desktop-style repeated cache refreshes. Reuse three sizes so scratch grows
 * and then stays resident, varying radius and color to catch stale stages.
 * Progress is flushed BEFORE the request, so a reset leaves a useful frame ID. */
static int api_blur_stress(void) {
    static const int widths[]={320,528,720}, heights[]={64,43,720};
    static const int radii[]={2,4,16,64};
    graph_t *cache[3]={NULL,NULL,NULL};
    uint64_t total=0;
    uint32_t worst=0,slow=0;
    int bad=1;
    for(unsigned i=0;i<3;i++) {
        cache[i]=graph_new_shm(widths[i],heights[i]);
        if(!cache[i] || !cache[i]->shm_contig) goto stress_done;
    }
    for(unsigned frame=0;frame<256;frame++) {
        unsigned slot=frame%3;
        graph_t *g=cache[slot];
        int w=widths[slot],h=heights[slot],r=radii[(frame/3)%4];
        uint32_t color=0x80000000u|((frame*0x234567u)&0x00ffffffu);
        for(int y=0;y<h;y++) for(int x=0;x<g->w;x++)
            g->buffer[y*g->w+x]=x<w?color:GUARD;
        if((frame%16)==0) {
            printf("BLUR STRESS frame=%u slot=%u rect=%dx%d r=%d\n",frame,slot,w,h,r);
            fflush(stdout);
        }
        uint32_t before=0,after=0,start=0,end=0;
        graph_g2d_blur_stats(&before,NULL,NULL,NULL,NULL,NULL);
        kernel_tic32(NULL,NULL,&start);
        graph_gaussian_blur(g,0,0,w,h,r);
        kernel_tic32(NULL,NULL,&end);
        graph_g2d_blur_stats(&after,NULL,NULL,NULL,NULL,NULL);
        if(after!=before+1) goto stress_done;
        uint32_t elapsed=end-start;
        total+=elapsed; if(elapsed>worst) worst=elapsed; slow+=elapsed>16000;
        for(int y=0;y<h;y++) for(int x=0;x<g->w;x++)
            if(g->buffer[y*g->w+x]!=(x<w?color:GUARD)) {
                printf("BLUR STRESS corrupted frame=%u pixel=%d,%d\n",frame,x,y);
                goto stress_done;
            }
    }
    printf("BLUR STRESS PASS 256 frames: avg=%u max=%u us, >16ms=%u\n",
           (unsigned)(total/256),worst,slow);
    bad=0;
stress_done:
    for(unsigned i=0;i<3;i++) graph_free(cache[i]);
    return bad;
}
