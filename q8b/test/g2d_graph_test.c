/* Host routing regression using the real graph_g2d.c and blt.c entry points.
 * No GPU, IPC or board access. Compile with the installed Q8B headers. */
#include <graph/graph.h>
#include <graph/graph_g2d.h>
#include <g2dclient/g2dclient.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static int calls, answer;
void __assert_func(const char *f,int l,const char *fn,const char *e) {
    printf("%s:%d %s: %s\n",f,l,fn,e); abort();
}
int has_g2d(void) { return 0; }
void g2d_quiesce_on_fault(int r) { assert(r == 0 || r == -1 || r == -2); }
int g2d_fill_rect(const g2d_fill_req_t *r) { (void)r; calls++; return answer; }
int g2d_fill_triangle(const g2d_triangle_req_t *r) { (void)r; calls++; return answer; }
int g2d_blit(const g2d_blit_req_t *r) { (void)r; calls++; return answer; }
int g2d_blit_alpha(const g2d_blit_req_t *r) { (void)r; calls++; return answer; }
int g2d_rotate(const g2d_rotate_req_t *r) { (void)r; calls++; return answer; }
int g2d_scale_to(const g2d_scale_to_req_t *r) { (void)r; calls++; return answer; }
int g2d_gaussian_blur(const g2d_gaussian_blur_req_t *r) { (void)r; calls++; return answer; }
int main(void) {
    uint32_t *p=calloc(128*128,4), *q=calloc(128*128,4);
    assert(p && q);
    graph_t a={.buffer=p,.w=128,.h=128,.shm_id=1,.shm_contig=1};
    graph_t b={.buffer=q,.w=128,.h=128,.shm_id=2,.shm_contig=1};
    graph_fill_rect(&a,0,0,128,128,0xff123456);
    assert(calls == 1 && p[0] == 0); /* opaque fill actually offloaded */
    answer=-1;
    graph_fill_rect(&a,0,0,128,128,0xff123456);
    assert(calls == 2 && p[0] == 0xff123456); /* replayable decline */
    answer=0;
    graph_fill_rect(&a,0,0,128,128,0xff345678);
    assert(calls == 3 && p[0] == 0xff123456); /* transient not latched */
    b.shm_contig=0;
    assert(graph_scale_tof_g2d(&a,&b,1) == -1 && calls == 3);
    b.shm_contig=1; a.shm_contig=0;
    assert(graph_scale_tof_g2d(&a,&b,1) == -1 && calls == 3);
    a.shm_contig=1;
    assert(graph_scale_tof_g2d(&a,&b,2) == -1 && calls == 3);
    assert(graph_scale_tof_g2d(&a,&b,1) == 0 && calls == 4);
    answer=-2;
    assert(graph_fill_triangle_g2d(&a,0,0,127,0,0,127,0xff123456) == -2);
    assert(calls == 5);
    answer=0;
    assert(graph_fill_triangle_g2d(&a,0,0,127,0,0,127,0xff123456) == -2);
    assert(calls == 5); /* only missing triangle capability is latched */
    assert(graph_fill_g2d(&a,0,0,128,128,0xff123456) == 0 && calls == 6);
    free(p); free(q);
    puts("graph routing: opaque fill, replay, independent caps, both scale buffers PASS");
    return 0;
}
