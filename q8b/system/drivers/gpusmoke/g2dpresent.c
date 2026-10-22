/* End-to-end display client -> Q8B fbdisplayd -> g2dd -> real GOP.
 * Run only against a separate test display node while the desktop is idle.
 * The original desktop canvas is saved and restored before returning. */
#include <display/display.h>
#include <bsp/bsp_fb.h>
#include <g2dclient/g2dclient.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/sys.h>
#include <sysinfo.h>
#include <q8b_platform.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t count(const char *stats,const char *name) {
    const char *p=strstr(stats,name);
    return p?strtoull(p+strlen(name),NULL,10):0;
}

static int check(const disp_info_t *fb,const uint32_t *ref,const char *name) {
    unsigned errors=0;
    for(unsigned y=0;y<fb->height;y++) {
        const volatile uint32_t *row=(const volatile uint32_t *)(fb->pointer+y*fb->pitch);
        for(unsigned x=0;x<fb->width;x++)
            if(row[x]!=ref[y*fb->width+x] && errors++==0)
                printf("%s first (%u,%u): %08x expected %08x\n",name,x,y,row[x],ref[y*fb->width+x]);
    }
    printf("%s: %s (%u real GOP pixel errors)\n",name,errors?"FAIL":"PASS",errors);
    return errors!=0;
}

int main(int argc,char **argv) {
    display_t original={.fd=-1},test={.fd=-1};
    uint32_t *saved=NULL,*ref=NULL;
    char *before=NULL,*after=NULL;
    graph_t *g=NULL;
    int bad=1,restore=0;
    sys_info_t info;
    const disp_info_t *fb=NULL;
    if(argc!=2 || strcmp(argv[1],"/dev/disp0")==0) {
        puts("Usage: g2dpresent <separate-test-display-node>");
        return 1;
    }
    if(has_g2d() || sys_get_sys_info(&info)) goto done;
    const q8b_platform_data_t *pd=(const q8b_platform_data_t *)info.platform_data;
    /* The existing ARGB8888 physical-copy protocol matches BGR GOP only. */
    if(pd->fb.format!=1 || pd->fb.bpp!=32 || bsp_fb_init(0,0,0)) {
        puts("g2dpresent requires the board's native 32-bit BGR GOP"); goto done;
    }
    fb=bsp_get_fbinfo();
    if(display_open("/dev/disp0",0,&original) || !display_fetch_graph(&original) ||
       display_open(argv[1],1,&test) || !(g=display_fetch_graph(&test))) goto done;
    if(g->w<270 || g->h<280 || g->w!=(int)fb->width || g->h!=(int)fb->height ||
       original.g->w!=g->w || original.g->h!=g->h ||
       !g->shm_contig || !test.g->shm_contig) {
        puts("g2dpresent requires matching native geometry and contiguous test canvases"); goto done;
    }
    unsigned bytes=fb->width*fb->height*4;
    saved=malloc(bytes); ref=malloc(bytes);
    if(!saved || !ref) goto done;
    memcpy(saved,original.g->buffer,bytes);
    restore=1;
    before=dev_cmd("/dev/g2d","stat");
    if(!before) goto done;
    printf("real GOP %ux%u pitch=%u, client/test display canvases contiguous\n",
        fb->width,fb->height,fb->pitch);
    for(unsigned frame=0;frame<3;frame++) {
        grect_t r={13,17,257,263};
        uint32_t bg=0xff203040u+frame*0x00171819u;
        uint32_t fg=0xffc06020u-frame*0x00110903u;
        if(frame!=1) {
            graph_fill_rect(g,0,0,g->w,g->h,bg);
            for(unsigned i=0;i<bytes/4;i++) ref[i]=bg;
        }
        graph_fill_rect(g,r.x,r.y,r.w,r.h,fg);
        for(int y=r.y;y<r.y+r.h;y++) for(int x=r.x;x<r.x+r.w;x++)
            ref[y*g->w+x]=fg;
        for(unsigned i=0;i<bytes/4;i++)
            if(g->buffer[i]!=ref[i]) {
                puts("g2dpresent Graph draw differs from CPU reference"); goto done;
            }
        if(display_set_dirty(&test,frame==1?&r:NULL,frame==1?1:0)) goto done;
        if(display_flush(&test,true)<0 ||
           check(fb,ref,frame==1?"real dirty presentation":"real full presentation")) goto done;
    }
    bad=0;
done:
    if(restore) {
        memcpy(g->buffer,saved,fb->width*fb->height*4);
        display_set_dirty(&test,NULL,0);
        if(display_flush(&test,true)<0 || check(fb,saved,"original desktop restore")) bad=1;
        else restore=0;
    }
    if(!bad) {
        after=dev_cmd("/dev/g2d","stat");
        if(!after) bad=1;
        else {
            uint64_t fills=count(after,"fill ")-count(before,"fill ");
            uint64_t blits=count(after,"blit ")-count(before,"blit ");
            uint64_t physical=count(after,"blit_phy ")-count(before,"blit_phy ");
            printf("real presentation IPC delta fill=%llu (>=5), blit=%llu (>=4), blit_phy=%llu (4)\n",
                (unsigned long long)fills,(unsigned long long)blits,(unsigned long long)physical);
            if(fills<5 || blits<4 || physical!=4) bad=1;
        }
    }
    if(restore) puts("WARNING: desktop restore did not pass; retain test display for diagnosis");
    display_close(&test); display_close(&original);
    free(before); free(after); free(saved); free(ref);
    printf("g2dpresent: %s (real GOP memory readback; panel appearance needs visual confirmation)\n",bad?"FAIL":"PASS");
    return bad;
}
