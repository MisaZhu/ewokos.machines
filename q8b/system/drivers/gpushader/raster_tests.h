/* Runs the production raster instructions through the existing fullscreen
 * interface. Both descriptors name one owned allocation; source and output
 * rows are disjoint. This verifies arithmetic/bounds, not new BSP routing. */
static uint32_t raster_solid_ref(uint32_t src, uint32_t dst) {
    unsigned a=src>>24, da=dst>>24;
    if(!a) return dst;
    if(a==255 || !da) return src;
    uint32_t out=(da+(255-da)*a/255)<<24;
    for(unsigned shift=0;shift<24;shift+=8)
        out|=(((((src>>shift)&255)*a+((dst>>shift)&255)*(255-a))/255)<<shift);
    return out;
}

static int raster_case(int handle, uint32_t *unif, int unif_id, int pos_id,
                       unsigned sw, unsigned sh, int degree, uint32_t color,
                       uint32_t background) {
    int rotation=degree>=0, ret=1, id;
    double radians=degree*(3.14159265358979323846/180.0);
    int c=(int)round(cos(radians)*16384), sn=(int)round(sin(radians)*16384);
    unsigned ac=c<0?-c:c, as=sn<0?-sn:sn;
    unsigned dw=rotation?(sw*ac+sh*as+16383u)>>14:sw;
    unsigned dh=rotation?(sw*as+sh*ac+16383u)>>14:sh;
    unsigned pitch=((sw>dw?sw:dw)+15u)&~15u;
    unsigned origin=sh+1, rows=origin+dh+1, count=pitch*rows+GUARD_BYTES/4;
    uint32_t *p=NULL, *ref=NULL;
    ewokos_addr_t pa;
    g2d_pgl_req_t draw;
    g2d_canvas_t empty={0};
    if(g2d_shm_alloc_phy(count*4,&id,&p,&pa)) goto done;
    ref=malloc(count*4);
    if(!ref) goto done;
    for(unsigned i=0;i<count;i++) p[i]=ref[i]=GUARD;
    for(unsigned y=0;y<sh;y++) for(unsigned x=0;x<sw;x++)
        p[y*pitch+x]=ref[y*pitch+x]=pattern(x,y,17);
    for(unsigned y=0;y<dh;y++) for(unsigned x=0;x<dw;x++)
        p[(origin+y)*pitch+x]=ref[(origin+y)*pitch+x]=background;
    unsigned ox=rotation?0:3, oy=rotation?0:5;
    unsigned width=rotation?dw:29, height=rotation?dh:23;
    for(unsigned y=0;y<height;y++) for(unsigned x=0;x<width;x++) {
        uint32_t want;
        if(!rotation) want=raster_solid_ref(color,background);
        else {
            int sx,sy;
            if(degree==0) { sx=x; sy=y; }
            else if(degree==90) { sx=y; sy=sh-1-x; }
            else if(degree==180) { sx=sw-1-x; sy=sh-1-y; }
            else if(degree==270) { sx=sw-1-y; sy=x; }
            else {
                /* Independent original 64-bit CPU reference. */
                int64_t dx=(int64_t)x*16384-((int64_t)dw-1)*8192;
                int64_t dy=(int64_t)y*16384-((int64_t)dh-1)*8192;
                sx=((((dx*c+dy*sn)>>14)+(int64_t)sw*8192)>>14);
                sy=((((dy*c-dx*sn)>>14)+(int64_t)sh*8192)>>14);
            }
            want=sx>=0 && sy>=0 && sx<(int)sw && sy<(int)sh?
                 pattern(sx,sy,17):0;
        }
        ref[(origin+oy+y)*pitch+ox+x]=want;
    }
    unif[0]=pitch; unif[1]=pitch; unif[2]=sw; unif[3]=sh;
    unif[4]=ox; unif[5]=origin+oy; unif[6]=width; unif[7]=height;
    unif[8]=rotation; unif[9]=rotation?(uint32_t)degree:color;
    unif[10]=(uint32_t)c; unif[11]=(uint32_t)sn;
    g2d_pgl_draw_req_init(&draw,g2d_canvas(id,count*4,pitch,rows,1),
        g2d_canvas(pos_id,36,3,3,1),empty,G2D_PGL_MODE_TRIANGLES,3,
        G2D_PGL_FMT_RGBA8,pitch*4,0,0,0,0,0,0,empty,0,0,0,1.0f);
    g2d_pgl_draw_req_set_shader(&draw,handle,unif_id,12,1);
    int rc=g2d_pgl_draw(&draw);
    g2d_quiesce_on_fault(rc);
    if(rc) { printf("raster draw ret=%d\n",rc); goto done; }
    unsigned errors=0, guards=0;
    volatile uint32_t *readback=p;
    for(unsigned i=0;i<count;i++) if(readback[i]!=ref[i]) {
        if(i>=pitch*rows) guards++;
        else if(errors++==0)
            printf("raster mismatch (%u,%u): %08x expected %08x\n",
                   i%pitch,i/pitch,readback[i],ref[i]);
    }
    printf("raster %ux%u -> %ux%u degree=%d color=%08x bg=%08x: %s (%u pixels/padding, %u guards)\n",
        sw,sh,dw,dh,degree,color,background,errors||guards?"FAIL":"PASS",errors,guards);
    ret=errors||guards;
done:
    free(ref); g2d_shm_free(p);
    return ret;
}

static int raster_tests(int handle) {
    static const unsigned alphas[]={0,1,127,128,254,255};
    static const uint32_t backgrounds[]={0xff204060u,0x80204060u,0x00204060u};
    static const unsigned sizes[][2]={{64,48},{65,37},{1,1},{320,240}};
    static const int degrees[]={0,90,180,270,45,135,1,89,91,179,315};
    uint32_t *unif=NULL, *pos=NULL;
    int unif_id, pos_id, ret=1;
    ewokos_addr_t pa;
    if(g2d_shm_alloc_phy(48,&unif_id,&unif,&pa) ||
       g2d_shm_alloc_phy(36,&pos_id,&pos,&pa)) goto done;
    memset(pos,0,36);
    for(unsigned a=0;a<6;a++) for(unsigned b=0;b<3;b++)
        if(raster_case(handle,unif,unif_id,pos_id,64,48,-1,
            (alphas[a]<<24)|0xc03020u,backgrounds[b])) goto done;
    for(unsigned size=0;size<4;size++) for(unsigned a=0;a<11;a++)
        if(raster_case(handle,unif,unif_id,pos_id,sizes[size][0],sizes[size][1],
            degrees[a],0,0xff204060u)) goto done;
    printf("raster matrix: PASS (18 solid alpha, 44 rotations, full source/padding/guards)\n");
    ret=0;
done:
    g2d_shm_free(pos); g2d_shm_free(unif);
    return ret;
}
