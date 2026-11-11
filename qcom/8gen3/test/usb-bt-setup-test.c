/* Exercise the real QCA setup with a USB-rootfs callback during firmware IO. */
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <bsp/bsp_usb.h>
#include <ewoksys/usbhci.h>
static int in_handler, usb_calls, firmware_opens, nested_calls, reset_pending;
static int fail_open;
static const char *fixture;
static int fw_open(const char *path, int flags) {
    assert(!in_handler); /* A USB-root read must be able to reenter the host. */
    assert(strstr(path, "rampatch_usb_00130201.bin"));
    firmware_opens++;
    proto_t out={0};
    assert(bsp_usb_bt_cntl(NULL, 1, USBHCI_CMD_INFO, NULL, &out)==0);
    assert(out.ints[0]==1 && out.ints[1]==0);
    nested_calls++;
    return fail_open ? -1 : open(fixture, flags);
}
static int no_sleep(useconds_t us) { (void)us; return 0; }
#define open fw_open
#define usleep no_sleep
#ifndef USB_BT_SOURCE
#define USB_BT_SOURCE "../system/libs/bsp/src/bsp_usbbt.c"
#endif
#include USB_BT_SOURCE
#undef open
#undef usleep

static void set16(uint8_t *p, unsigned v) { p[0]=v;p[1]=v>>8; }
static void set32(uint8_t *p, unsigned v) { set16(p,v);set16(p+2,v>>16); }
uint64_t kernel_tic_ms(int unused) { static uint64_t t; (void)unused;return ++t; }
void klog(const char *format, ...) { (void)format; }
int bsp_usb_control_xfer(bsp_usb_dev_t *d, usb_setup_pkt_t *s, void *buf, bool input) {
    (void)d; (void)input; assert(!in_handler); usb_calls++;
    uint8_t *p=buf;
    if (s->bRequest==9) {
        memset(p,0,s->wLength);set32(p,0x00130201);set32(p+4,0x38e6);
    } else if (s->bRequest==5) p[0]=0x40; /* NVM already loaded. */
    else if (s->bmRequestType==0x20) reset_pending=1;
    return s->wLength;
}
int bsp_usb_bulk_xfer(bsp_usb_dev_t *d,uint8_t e,void *p,int n,bool input) {
    (void)d;(void)e;(void)p;(void)input;assert(!in_handler);usb_calls++;return n;
}
int bsp_usb_int_in_poll(bsp_usb_dev_t *d,uint8_t e,void *p,int n) {
    (void)d;(void)e;(void)n;
    if (!reset_pending) return 0;
    const uint8_t reply[]={0x0e,4,1,3,12,0};memcpy(p,reply,sizeof(reply));
    reset_pending=0;return sizeof(reply);
}
int bsp_usb_bulk_in_poll(bsp_usb_dev_t *d,uint8_t e,void *p,int n) {
    (void)d;(void)e;(void)p;(void)n;return 0;
}
int bsp_usb_int_in_open(bsp_usb_dev_t *d,uint8_t e,uint16_t m,uint8_t i) {
    (void)d;(void)e;(void)m;(void)i;return 0;
}
int bsp_usb_bulk_open(bsp_usb_dev_t *d,uint8_t e,uint16_t m) {
    (void)d;(void)e;(void)m;return 0;
}
bool bsp_usb_xhci_owned(bsp_usb_dev_t *d) { return d!=NULL; }
static proto_factor_t *addi(proto_t *p,int n) { p->ints[p->count++]=n;return PF; }
static proto_factor_t *add(proto_t *p,const void *data,uint32_t n) {
    (void)p;(void)data;(void)n;return PF;
}
proto_factor_t *get_proto_factor(void) { static proto_factor_t f={addi,add};return &f; }
int proto_read_int(proto_t *p) { (void)p;return 0; }
void *proto_read(proto_t *p,int32_t *n) { (void)p;*n=0;return NULL; }
static void claim(void) {
    static bsp_usb_dev_t device;
    bsp_usb_bt_reset();bt.dev=&device;bt.claimed=true;bt.ep_out=2;bt.ep_evt=0x81;
}
static void request(void) {
    int calls=usb_calls;
    in_handler=1;
    assert(bsp_usb_bt_cntl(NULL,1,USBHCI_CMD_SETUP,NULL,NULL)==-1);
    in_handler=0;
    assert(usb_calls==calls);
}
int main(int argc,char **argv) {
    assert(argc==2);fixture=argv[1];
    uint8_t fw[44]={1};set32(fw,40u<<8|1);set16(fw+16,0x13);
    set16(fw+18,0x0201);set16(fw+20,0x38e7);
    int fd=open(fixture,O_CREAT|O_TRUNC|O_WRONLY,0600);assert(fd>=0);
    assert(write(fd,fw,sizeof(fw))==sizeof(fw));close(fd);
    claim();request();request(); /* Coalesce requests; no IO in either handler. */
    bsp_usb_bt_poll();assert(bt.ready && !bt.busy && firmware_opens==1 && nested_calls==1);
    int calls=usb_calls;
    assert(bsp_usb_bt_cntl(NULL,1,USBHCI_CMD_SETUP,NULL,NULL)==0);
    assert(usb_calls==calls);
    claim();fail_open=1;request();bsp_usb_bt_poll();assert(!bt.ready && !bt.busy);
    fail_open=0;request();bsp_usb_bt_poll();assert(bt.ready);
    claim();request();bsp_usb_bt_reset();calls=usb_calls;bsp_usb_bt_poll();assert(usb_calls==calls);
    puts("USB Bluetooth deferred setup/reentrant firmware IO: PASS");return 0;
}
