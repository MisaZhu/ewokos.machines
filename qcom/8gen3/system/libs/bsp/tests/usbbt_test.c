/* Run the real transport with deterministic USB, file, and clock peers. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <bsp/bsp_usb.h>
#include <ewoksys/kernel_tic.h>

static int fake_open(const char *,int,...);
static off_t fake_lseek(int,off_t,int);
static ssize_t fake_read(int,void *,size_t);
static int fake_close(int);
static int fake_usleep(useconds_t);
#define open fake_open
#define lseek fake_lseek
#define read fake_read
#define close fake_close
#define usleep fake_usleep
#include "../src/bsp_usbbt.c"
#undef open
#undef lseek
#undef read
#undef close
#undef usleep

static uint8_t version[20], fw[8400], events[600];
static size_t file_size,file_pos,event_size,event_pos,event_chunk;
static int controls,bulks,fail_control,fail_bulk,fw_mode,reads;
static uint8_t status;
static uint64_t now;
static char paths[2][160];
static unsigned path_count;
static uint16_t header_sizes[2];
static int hindex;
static int ipc_type,ipc_size;
static uint8_t *ipc_data;
static void put16(uint8_t *p,uint16_t x) { p[0]=x; p[1]=x>>8; }
static void put32(uint8_t *p,uint32_t x) { put16(p,x); put16(p+2,x>>16); }

static int fake_open(const char *name,int flags,...) {
    (void)flags;
    assert(path_count<2);
    snprintf(paths[path_count++],160,"%s",name);
    memset(fw,0,sizeof(fw)); file_pos=0;
    int patch=strstr(name,"rampatch")!=NULL;
    file_size=patch ? 8400 : 48;
    fw[0]=patch ? 1 : 2;
    uint32_t n=(uint32_t)file_size-4;
    fw[1]=n; fw[2]=n>>8; fw[3]=n>>16;
    if (patch) { put16(fw+16,le32(version)>>16); put16(fw+18,le32(version)); put16(fw+20,8); }
    if (fw_mode==1) fw[0]=0;
    if (fw_mode==2) fw[1]^=1;
    if (fw_mode==3 && patch) put16(fw+18,0xffff);
    if (fw_mode==4 && patch) put16(fw+20,le32(version+4));
    if (fw_mode==5) file_size=3;
    return fw_mode==6 ? -1 : 7;
}
static off_t fake_lseek(int fd,off_t off,int whence) {
    assert(fd==7);
    if (whence==SEEK_END) return file_size;
    assert(whence==SEEK_SET && off==0); file_pos=0; return 0;
}
static ssize_t fake_read(int fd,void *p,size_t n) {
    assert(fd==7); reads++;
    if (n>113) n=113; /* exercise short filesystem reads */
    if (n>file_size-file_pos) n=file_size-file_pos;
    memcpy(p,fw+file_pos,n); file_pos+=n; return n;
}
static int fake_close(int fd) { assert(fd==7); return 0; }
static int fake_usleep(useconds_t us) { now+=(us+999)/1000; return 0; }
uint64_t kernel_tic_ms(int zone) { (void)zone; return now; }
void klog(const char *fmt,...) { (void)fmt; }
bool bsp_usb_xhci_owned(bsp_usb_dev_t *dev) { return dev!=NULL; }
int bsp_usb_int_in_open(bsp_usb_dev_t *d,uint8_t ep,uint16_t mps,uint8_t iv) {
    (void)d; (void)ep; (void)mps; (void)iv; return 0;
}
int bsp_usb_bulk_open(bsp_usb_dev_t *d,uint8_t ep,uint16_t mps) {
    (void)d; (void)ep; (void)mps; return 0;
}
int bsp_usb_control_xfer(bsp_usb_dev_t *d,const usb_setup_pkt_t *s,void *p,bool in) {
    (void)d; controls++;
    if (fail_control) return s->wLength-1;
    if (in) {
        if (s->bRequest==9) memcpy(p,version,20);
        else { assert(s->bRequest==5); *(uint8_t *)p=status; }
    } else if (s->bmRequestType==0x40) {
        assert(s->bRequest==1 && hindex<2); header_sizes[hindex++]=s->wLength;
    } else {
        assert(s->bmRequestType==0x20 && s->wLength==3);
        assert(memcmp(p,"\x03\x0c\x00",3)==0);
    }
    return s->wLength;
}
int bsp_usb_bulk_xfer(bsp_usb_dev_t *d,uint8_t ep,void *p,int n,bool in) {
    (void)d; (void)p; assert(ep==2 && !in && n<=(ipc_data ? 4100 : 4096)); bulks++;
    return fail_bulk ? n-1 : n;
}
int bsp_usb_int_in_poll(bsp_usb_dev_t *d,uint8_t ep,void *p,int n) {
    (void)d; (void)ep;
    size_t count=event_size-event_pos;
    if (count>event_chunk) count=event_chunk;
    if (count>(size_t)n) count=n;
    memcpy(p,events+event_pos,count); event_pos+=count; return count;
}
int bsp_usb_bulk_in_poll(bsp_usb_dev_t *d,uint8_t ep,void *p,int n) {
    (void)d; (void)ep; (void)p; (void)n; return 0;
}
/* SEND needs only these two reads; other IPC paths remain unmocked. */
proto_factor_t *get_proto_factor(void) { abort(); }
int32_t proto_read_int(proto_t *p) { (void)p; return ipc_type; }
void *proto_read(proto_t *p,int32_t *n) { (void)p; *n=ipc_size; return ipc_data; }

static void reset(void) {
    memset(&bt,0,sizeof(bt)); rx_reset();
    memset(version,0,sizeof(version)); put32(version,0x00130201); put32(version+4,7);
    now=0; controls=bulks=fail_control=fail_bulk=fw_mode=reads=0;
    status=0; path_count=0; hindex=0; memset(paths,0,sizeof(paths));
    memcpy(events,"\x0e\x04\x01\x03\x0c\x00",6);
    event_pos=0; event_size=6; event_chunk=1;
    bt.ep_out=2; bt.ep_evt=0x81;
    ipc_data=NULL; ipc_size=ipc_type=0;
}
static void framing(void) {
    reset();
    uint8_t evt[]={0x0e,4,1,3,12,0,0xff,0};
    for (unsigned i=0;i<sizeof(evt);i++) frame_feed(&bt.evt,evt+i,1);
    const uint8_t want[]={4,0x0e,4,1,3,12,0,4,0xff,0};
    assert(bt.wp==sizeof(want) && memcmp(bt.rx,want,sizeof(want))==0);
    /* Reject 65535-byte ACL, then recover exactly at next frame. */
    uint8_t hdr[]={1,0,255,255},valid[]={2,0,1,0,0xab};
    frame_feed(&bt.acl,hdr,4);
    uint8_t *body=calloc(65535,1); assert(body);
    frame_feed(&bt.acl,body,65535); free(body);
    frame_feed(&bt.acl,valid,sizeof(valid));
    assert(bt.dropped==1 && bt.acl.have==0 && bt.acl.skip==0);
    assert(bt.wp==sizeof(want)+6 && bt.rx[sizeof(want)]==2);
    /* Atomic drop on a full ring, including unsigned counter wrap. */
    rx_reset(); bt.rp=UINT32_MAX-100; bt.wp=bt.rp+RX_SIZE-2;
    uint32_t before=bt.wp;
    frame_feed(&bt.evt,evt,6); assert(bt.wp==before && bt.dropped==2);
    bt.rp=bt.wp; frame_feed(&bt.evt,evt,6);
    assert(bt.wp-bt.rp==7 && bt.rx[bt.rp%RX_SIZE]==4);
    /* Largest accepted ACL payload. */
    rx_reset(); uint8_t *acl=calloc(4100,1); assert(acl);
    put16(acl+2,4096); frame_feed(&bt.acl,acl,4100); free(acl);
    assert(bt.wp==4101);
}
static void setup(void) {
    reset(); assert(qca_setup()==0);
    assert(path_count==2 && hindex==2 && header_sizes[0]==40 && header_sizes[1]==4);
    assert(bulks==4 && reads>2);
    assert(strstr(paths[0],"/qca/rampatch_usb_00130201.bin"));
    assert(strstr(paths[1],"/qca/nvm_usb_00130201.bin"));
    reset(); status=0xc0; assert(qca_setup()==0 && path_count==0 && bulks==0);
    reset(); status=0x80; put32(version+8,0x400c1211);
    version[12]=3; version[13]=10; put16(version+14,0x8000);
    assert(qca_setup()==0 && strstr(paths[0],"/QCA2066/nvm_usb_00130201_gf_030a.bin"));
    reset(); status=0x80; version[12]=version[13]=255; put16(version+14,0x8000);
    assert(qca_setup()==0 && strstr(paths[0],"nvm_usb_00130201.bin"));
    reset(); put32(version,0xdeadbeef); assert(qca_setup()<0 && path_count==0);
    for (int mode=1;mode<=6;mode++) {
        reset(); fw_mode=mode; assert(qca_setup()<0 && bulks==0);
    }
    reset(); status=0x80; fw_mode=2; assert(qca_setup()<0 && bulks==0);
    reset(); fail_control=1; assert(qca_setup()<0 && bulks==0);
    reset(); fail_bulk=1; assert(qca_setup()<0 && bulks==1 && path_count==1);
    reset(); fail_control=1; assert(download(fw,48,40)<0 && bulks==0);
    reset(); status=0xc0; events[5]=1; assert(qca_setup()<0);
    reset(); status=0xc0; events[3]=4; assert(qca_setup()<0 && now>=2000);
    reset(); status=0xc0; event_size=0; assert(qca_setup()<0 && now>=2000);
    /* An unrelated event before a fragmented reset completion is allowed. */
    reset(); memmove(events+3,events,6); memcpy(events,"\xff\x01\xaa",3);
    event_size=9; event_chunk=4; assert(hci_reset()==0);
}
static void send_boundaries(void) {
    reset();
    uint8_t *packet=calloc(4101,1); assert(packet);
    ipc_data=packet; ipc_type=H4_ACL; ipc_size=4100; put16(packet+2,4096);
    /* An unready adapter must never submit a transfer. */
    assert(bsp_usb_bt_cntl(NULL,0,USBHCI_CMD_SEND,NULL,NULL)<0 && bulks==0);
    bt.ready=true;
    assert(bsp_usb_bt_cntl(NULL,0,USBHCI_CMD_SEND,NULL,NULL)==0 && bulks==1);
    ipc_size=4101;
    assert(bsp_usb_bt_cntl(NULL,0,USBHCI_CMD_SEND,NULL,NULL)<0 && bulks==1);
    ipc_size=4100; put16(packet+2,4095);
    assert(bsp_usb_bt_cntl(NULL,0,USBHCI_CMD_SEND,NULL,NULL)<0 && bulks==1);
    put16(packet+2,4096); fail_bulk=1;
    assert(bsp_usb_bt_cntl(NULL,0,USBHCI_CMD_SEND,NULL,NULL)<0 && bulks==2);
    free(packet); ipc_data=NULL;
}
int main(void) {
    framing(); setup(); send_boundaries();
    puts("usbbt: framing, DFU validation, firmware selection, HCI reset, ACL send bounds passed");
    return 0;
}
