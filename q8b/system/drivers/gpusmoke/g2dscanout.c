/* Actual displayd library -> live g2dd -> owned RAM readback. This client
 * never initializes hardware or addresses the real display framebuffer. */
#include <displayd/displayd.h>
#include <g2dclient/g2dclient.h>
#include <ewoksys/vdevice.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUARD 0x13579bdfu
#define GUARD_WORDS 4096u

static uint32_t pattern(unsigned x,unsigned y) {
    return 0xff000000u|(((3*x+7*y)&255)<<16)|(((5*x+y)&255)<<8)|((x+11*y)&255);
}

static int check(const char *name,const uint32_t *p,const uint32_t *ref,unsigned words) {
    unsigned errors=0;
    for(unsigned i=0;i<words+GUARD_WORDS;i++) {
        if(p[i]!=ref[i] && errors++==0)
            printf("%s first word %u: %08x expected %08x\n",name,i,p[i],ref[i]);
    }
    printf("%s: %s (%u pixel/padding/guard errors)\n",name,errors?"FAIL":"PASS",errors);
    return errors!=0;
}

static uint64_t count(const char *stats,const char *name) {
    const char *p=strstr(stats,name);
    return p?strtoull(p+strlen(name),NULL,10):0;
}

static int run_case(unsigned edge,unsigned pitch,unsigned rows,unsigned expected_blits) {
    int sid,did,bad=1;
    uint32_t *src=NULL,*dst=NULL,*ref=NULL;
    ewokos_addr_t sp,dp;
    graph_t gs;
    disp_info_t fb={0};
    char *before=NULL,*after=NULL;
    unsigned src_words=edge*edge,dst_words=pitch*rows,origin=pitch+16;
    printf("display case %ux%u pitch=%u origin=(16,1)\n",edge,edge,pitch*4);
    if(has_g2d() || g2d_shm_alloc_phy((src_words+GUARD_WORDS)*4,&sid,&src,&sp) ||
       g2d_shm_alloc_phy((dst_words+GUARD_WORDS)*4,&did,&dst,&dp)) goto done;
    ref=malloc((dst_words+GUARD_WORDS)*4);
    if(!ref) goto done;
    for(unsigned i=0;i<src_words+GUARD_WORDS;i++)
        src[i]=i<src_words?pattern(i%edge,i/edge):GUARD;
    graph_init(&gs,src,edge,edge); gs.shm_id=sid; gs.shm_contig=1;
    fb.width=edge; fb.height=edge; fb.depth=32; fb.pitch=pitch*4;
    fb.xoffset=16; fb.yoffset=1; fb.pointer=(ewokos_addr_t)dst;
    fb.phy_base=dp; fb.size=dst_words*4;
    before=dev_cmd("/dev/g2d","stat");
    if(!before) goto done;
    for(unsigned cpu=0;cpu<2;cpu++) {
        gs.shm_contig=cpu?0:1;
        fb.phy_base=cpu?0:dp;
        for(unsigned full=0;full<2;full++) {
            grect_t r=full?(grect_t){0,0,edge,edge}:(grect_t){3,5,29,17};
            for(unsigned i=0;i<dst_words+GUARD_WORDS;i++) dst[i]=ref[i]=GUARD;
            for(int y=0;y<r.h;y++) for(int x=0;x<r.w;x++)
                ref[origin+(r.y+y)*pitch+r.x+x]=pattern(r.x+x,r.y+y);
            uint32_t n=fbdisplayd_flush_rect_to(&fb,&gs,&r);
            if(n!=(uint32_t)(r.w*r.h*4) ||
               check(cpu?"display CPU rect/full pitch+origin":"display GPU rect/full pitch+origin",dst,ref,dst_words)) goto done;
        }
        for(int rot=G_ROTATE_90;rot<=G_ROTATE_270;rot++) {
            for(unsigned i=0;i<dst_words+GUARD_WORDS;i++) dst[i]=ref[i]=GUARD;
            for(unsigned y=0;y<edge;y++) for(unsigned x=0;x<edge;x++) {
                unsigned sx=rot==G_ROTATE_90?y:rot==G_ROTATE_180?edge-1-x:edge-1-y;
                unsigned sy=rot==G_ROTATE_90?edge-1-x:rot==G_ROTATE_180?edge-1-y:x;
                ref[origin+y*pitch+x]=pattern(sx,sy);
            }
            printf("display rotation=%d CPU-only=%u\n",rot*90,cpu);
            if(fbdisplayd_rotate_to(&fb,&gs,rot)!=src_words*4 ||
               check("display rotation pitch+origin",dst,ref,dst_words)) goto done;
        }
        grect_t invalid={-1,0,16,16};
        if(fbdisplayd_flush_rect_to(&fb,&gs,&invalid)!=0 ||
           check("display invalid rect no-write",dst,ref,dst_words)) goto done;
    }
    for(unsigned i=0;i<src_words+GUARD_WORDS;i++)
        if(src[i]!=(i<src_words?pattern(i%edge,i/edge):GUARD)) {
            puts("display source/guard changed FAIL"); goto done;
        }
    after=dev_cmd("/dev/g2d","stat");
    if(!after) goto done;
    uint64_t blits=count(after,"blit_phy ")-count(before,"blit_phy ");
    uint64_t rotations=count(after,"rotate_phy ")-count(before,"rotate_phy ");
    /* Two direct rect copies. At/above graph's contiguous allocation
       threshold, 90/270 also GPU-copy the CPU-rotated intermediate.
       Below that threshold the intermediate copy stays on the CPU.
       180 goes straight to rotate_phy, with no intermediate blit. */
    printf("display IPC delta: blit_phy=%llu (%u), rotate_phy=%llu (3)\n",
        (unsigned long long)blits,expected_blits,(unsigned long long)rotations);
    if(blits!=expected_blits || rotations!=3) goto done;
    bad=0;
done:
    free(before); free(after); free(ref);
    g2d_shm_free(src); g2d_shm_free(dst);
    return bad;
}

int main(void) {
    int bad=run_case(64,80,72,2) || run_case(128,160,136,4);
    printf("g2dscanout: %s (display library into owned RAM; no real scan-out)\n",bad?"FAIL":"PASS");
    return bad;
}
