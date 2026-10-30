/* Regression: an empty host reply after consuming bytes must time out. */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <bt/bsp_bt.h>
#include <ewoksys/vdevice.h>
static int fake_usleep(useconds_t);
#define usleep fake_usleep
#include "../src/bsp_bt.c"
#undef usleep
static uint64_t now;
static int replies,recv_calls;
static unsigned char payload[]={4,0x0e,0};
static proto_factor_t factor;
static proto_factor_t *init(proto_t *p) { memset(p,0,sizeof(*p)); return &factor; }
static proto_factor_t *clear(proto_t *p) { (void)p; return &factor; }
static proto_factor_t *addi(proto_t *p,int32_t v) { (void)p; (void)v; return &factor; }
static proto_factor_t *add(proto_t *p,const void *v,uint32_t n) {
    (void)p; (void)v; (void)n; return &factor;
}
proto_factor_t *get_proto_factor(void) { return &factor; }
int32_t proto_read_int(proto_t *p) { (void)p; return 1; }
void *proto_read(proto_t *p,int32_t *n) { *n=p->size; return p->data; }
int dev_get_pid(const char *name) { (void)name; return 53; }
int dev_cntl_by_pid(int pid,int cmd,proto_t *in,proto_t *out) {
    (void)pid; (void)in;
    if (cmd==USBHCI_CMD_RECV) {
        recv_calls++;
        if (replies>0) { replies--; out->data=payload; out->size=sizeof(payload); }
    }
    return 0;
}
uint64_t kernel_tic_ms(int zone) { (void)zone; return now; }
static int fake_usleep(useconds_t us) { now+=(us+999)/1000; return 0; }
int main(void) {
    factor.init=init; factor.clear=clear; factor.addi=addi; factor.add=add;
    replies=1;
    assert(bsp_bt_recv(0)==4);
    assert(bsp_bt_recv(0)==0x0e);
    assert(bsp_bt_recv(0)==0);
    assert(recv_calls==1);
    assert(bsp_bt_recv(0)==-1 && recv_calls==2);
    assert(bsp_bt_recv(5)==-1 && now==5 && recv_calls==8);
    replies=1;
    assert(bsp_bt_recv(0)==4);
    assert(bsp_bt_flush()==2);
    assert(bsp_bt_recv(0)==-1);
    puts("bt_client: exhausted/empty receive timeout and flush passed");
    return 0;
}
