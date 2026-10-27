/* QCNFA765 USB Bluetooth transport. Only usbhostd owns the xHCI rings.
 * Protocol reference: Linux drivers/bluetooth/btusb.c QCA USB DFU path.
 * Runtime H4 framing follows EwokOS x86 bsp_usbbt.c's USBHCI contract. */
#include <bsp/bsp_usb.h>
#include <ewoksys/usbhci.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/klog.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#define RX_SIZE 16384u
#define FW_LIMIT (2u * 1024u * 1024u)
#define H4_CMD 1
#define H4_ACL 2
#define H4_EVT 4

typedef struct {
    uint8_t data[4100];
    uint32_t have, want, skip;
    uint8_t type;
} hci_frame_t;

static struct {
    bsp_usb_dev_t *dev;
    bool claimed, ready, busy;
    uint16_t vid, pid;
    uint8_t ep_evt, ep_in, ep_out;
    uint32_t rom, patch, ram, dropped;
    uint8_t rx[RX_SIZE];
    uint32_t rp, wp;
    hci_frame_t evt, acl;
    char firmware[96];
} bt;

static uint16_t le16(const uint8_t *p) { return p[0] | ((uint16_t)p[1]<<8); }
static uint32_t le32(const uint8_t *p) { return le16(p) | ((uint32_t)le16(p+2)<<16); }

static void rx_reset(void) {
    bt.rp=bt.wp=0;
    memset(&bt.evt,0,sizeof(bt.evt)); bt.evt.type=H4_EVT;
    memset(&bt.acl,0,sizeof(bt.acl)); bt.acl.type=H4_ACL;
}

static void frame_emit(hci_frame_t *f) {
    uint32_t n=f->want+1;
    if (n > RX_SIZE-(bt.wp-bt.rp)) { bt.dropped++; return; }
    bt.rx[bt.wp++ % RX_SIZE]=f->type;
    for (uint32_t i=0;i<f->want;i++) bt.rx[bt.wp++ % RX_SIZE]=f->data[i];
}

/* Transfer boundaries can split HCI headers or contain several packets.
 * Discard the whole oversized frame before accepting the next header. */
static void frame_feed(hci_frame_t *f,const uint8_t *p,uint32_t n) {
    for (uint32_t i=0;i<n;i++) {
        if (f->skip) { f->skip--; continue; }
        f->data[f->have++]=p[i];
        if (!f->want && f->have==(f->type==H4_EVT ? 2u : 4u)) {
            f->want=f->have+(f->type==H4_EVT ? f->data[1] : le16(f->data+2));
            if (f->want>sizeof(f->data)) {
                f->skip=f->want-f->have;
                f->have=f->want=0; bt.dropped++;
                continue;
            }
        }
        if (f->want && f->have==f->want) {
            frame_emit(f); f->have=f->want=0;
        }
    }
}

void bsp_usb_bt_poll(void) {
    uint8_t p[1024];
    if (!bt.claimed || bt.busy || !bt.ready) return;
    for (int i=0;i<4;i++) {
        int n=bsp_usb_int_in_poll(bt.dev,bt.ep_evt,p,sizeof(p));
        if (n<=0) break;
        frame_feed(&bt.evt,p,(uint32_t)n);
    }
    for (int i=0;i<4;i++) {
        int n=bsp_usb_bulk_in_poll(bt.dev,bt.ep_in,p,sizeof(p));
        if (n<=0) break;
        frame_feed(&bt.acl,p,(uint32_t)n);
    }
}

static int vendor_in(uint8_t req,void *p,uint16_t n) {
    usb_setup_pkt_t s={0};
    s.bmRequestType=0xc0; s.bRequest=req; s.wLength=n;
    return bsp_usb_control_xfer(bt.dev,&s,p,true)==n ? 0 : -1;
}

static uint8_t *load_fw(const char *name,uint32_t *len) {
    char path[160];
    snprintf(path,sizeof(path),"/etc/firmware/qca/%s",name);
    int fd=open(path,O_RDONLY);
    if (fd<0) { klog("q8b bt: missing %s\n",path); return NULL; }
    off_t size=lseek(fd,0,SEEK_END);
    if (size<=0 || size>FW_LIMIT || lseek(fd,0,SEEK_SET)<0) { close(fd); return NULL; }
    uint8_t *data=malloc((size_t)size);
    if (!data) { close(fd); return NULL; }
    uint32_t pos=0;
    while (pos<(uint32_t)size) {
        int n=read(fd,data+pos,(uint32_t)size-pos);
        if (n<=0) { free(data); close(fd); return NULL; }
        pos+=(uint32_t)n;
    }
    close(fd); *len=pos;
    return data;
}

static int download(const uint8_t *data,uint32_t len,uint16_t hdr) {
    if (len<hdr || bt.ep_out!=2) return -1;
    usb_setup_pkt_t s={0};
    s.bmRequestType=0x40; s.bRequest=1; s.wLength=hdr;
    if (bsp_usb_control_xfer(bt.dev,&s,(void *)data,false)!=hdr) return -1;
    usleep(20000);
    for (uint32_t pos=hdr;pos<len;) {
        uint32_t n=len-pos;
        if (n>4096) n=4096;
        if (bsp_usb_bulk_xfer(bt.dev,bt.ep_out,(void *)(data+pos),(int)n,false)!=(int)n) {
            klog("q8b bt: DFU short/failed at %u/%u\n",pos,len); return -1;
        }
        pos+=n;
    }
    return 0;
}

/* An HCI reset confirms the downloaded image is operational before exposing
 * the stream to btd. Firmware download itself uses vendor USB, not HCI. */
static int hci_reset(void) {
    uint8_t cmd[]={3,12,0},data[257];
    uint32_t have=0,want=0;
    usb_setup_pkt_t s={0}; s.bmRequestType=0x20; s.wLength=3;
    if (bsp_usb_control_xfer(bt.dev,&s,cmd,false)!=3) return -1;
    uint64_t until=kernel_tic_ms(0)+2000;
    while (kernel_tic_ms(0)<until) {
        uint8_t p[257];
        int n=bsp_usb_int_in_poll(bt.dev,bt.ep_evt,p,sizeof(p));
        if (n<0) return -1;
        for (int i=0;i<n;i++) {
            data[have++]=p[i];
            if (have==2) want=2u+data[1];
            if (want && have==want) {
                if (data[0]==0x0e && want>=6 && le16(data+3)==0x0c03)
                    return data[5] ? -1 : 0;
                have=want=0;
            }
        }
        usleep(1000);
    }
    return -1;
}

static int qca_setup(void) {
    uint8_t ver[20],status;
    bt.ready=false; rx_reset();
    if (vendor_in(9,ver,sizeof(ver))) return -1;
    bt.rom=le32(ver); bt.patch=le32(ver+4); bt.ram=le32(ver+8);
    klog("q8b bt: %04x:%04x ROM=%08x patch=%08x RAM=%08x flags=%04x\n",
         bt.vid,bt.pid,bt.rom,bt.patch,bt.ram,le16(ver+14));
    if (bt.rom!=0x00130100 && bt.rom!=0x00130200 && bt.rom!=0x00130201) {
        klog("q8b bt: unsupported ROM; firmware not sent\n"); return -1;
    }
    if (vendor_in(5,&status,1)) return -1;
    uint16_t board=(le16(ver+14)>>8)==0x80 ? ((uint16_t)ver[12]<<8)|ver[13] : 0;
    if (board==0xffff) board=0; /* OTP marks an unavailable board ID this way. */
    const char *dir=(bt.rom==0x00130201 && (board==0x030a || board==0x030b)) ? "QCA2066/" : "";
    if (!(status&0x80)) {
        snprintf(bt.firmware,sizeof(bt.firmware),"%srampatch_usb_%08x.bin",dir,bt.rom);
        uint32_t size=0;
        uint8_t *fw=load_fw(bt.firmware,&size);
        if (!fw) return -1;
        uint32_t rom=size>=22 ? ((uint32_t)le16(fw+16)<<16)|le16(fw+18) : 0;
        uint32_t tlv=size>=4 ? le32(fw)>>8 : 0;
        if (size<40 || fw[0]!=1 || tlv!=size-4 || rom!=bt.rom || le16(fw+20)<=bt.patch) {
            klog("q8b bt: invalid rampatch ROM/version/size\n"); free(fw); return -1;
        }
        int rc=download(fw,size,40); free(fw);
        if (rc) return -1;
    }
    if (vendor_in(9,ver,sizeof(ver))) return -1;
    if (le32(ver)!=bt.rom) return -1;
    bt.patch=le32(ver+4); bt.ram=le32(ver+8);
    board=(le16(ver+14)>>8)==0x80 ? ((uint16_t)ver[12]<<8)|ver[13] : 0;
    if (board==0xffff) board=0;
    dir=(bt.rom==0x00130201 && (board==0x030a || board==0x030b)) ? "QCA2066/" : "";
    if (!(status&0x40)) {
        /* QCA2066 needs its own directory, never silently use a WCN6855 NVM. */
        const char *gf=(bt.ram==0x400c1200 || bt.ram==0x400c1211) ? "_gf" : "";
        if (board)
            snprintf(bt.firmware,sizeof(bt.firmware),"%snvm_usb_%08x%s_%04x.bin",dir,bt.rom,gf,board);
        else
            snprintf(bt.firmware,sizeof(bt.firmware),"%snvm_usb_%08x%s.bin",dir,bt.rom,gf);
        uint32_t size=0;
        uint8_t *fw=load_fw(bt.firmware,&size);
        if (!fw) return -1;
        if (size<4 || fw[0]!=2 || (le32(fw)>>8)!=size-4) {
            klog("q8b bt: invalid NVM TLV size/type\n"); free(fw); return -1;
        }
        int rc=download(fw,size,4); free(fw);
        if (rc) return -1;
        if (bt.rom>=0x00130201) usleep(100000);
    }
    if (hci_reset()) { klog("q8b bt: HCI reset failed\n"); return -1; }
    rx_reset();
    klog("q8b bt: firmware operational, USB HCI ready\n");
    return 0;
}

int bsp_usb_bt_probe(bsp_usb_dev_t *dev,uint16_t vid,uint16_t pid,
                     const uint8_t *cfg,int len) {
    klog("q8b usb: enumerated %04x:%04x config_len=%d\n",vid,pid,len);
    if (bt.claimed || !dev || !cfg || !bsp_usb_xhci_owned(dev)) return -1;
    /* Foxconn/Lite-On/Qualcomm. Verify ROM over vendor USB before DFU. */
    if (vid!=0x0489 && vid!=0x0cf3 && vid!=0x04ca) return -1;
    bool iface=false;
    uint8_t evt=0,in=0,out=0,interval=1;
    uint16_t emps=0,imps=0,omps=0;
    for (int off=0;off+2<=len;) {
        unsigned n=cfg[off],type=cfg[off+1];
        if (n<2 || n>(unsigned)(len-off)) return -1;
        if (type==USB_DESC_INTERFACE && n>=9) {
            if (iface) break;
            iface=cfg[off+3]==0 && cfg[off+5]==0xe0 && cfg[off+6]==1 && cfg[off+7]==1;
        } else if (iface && type==USB_DESC_ENDPOINT && n>=7) {
            uint8_t ep=cfg[off+2],xtype=cfg[off+3]&3;
            uint16_t mps=le16(cfg+off+4)&0x7ff;
            if (xtype==3 && (ep&0x80)) { evt=ep; emps=mps; interval=cfg[off+6]; }
            if (xtype==2 && (ep&0x80)) { in=ep; imps=mps; }
            if (xtype==2 && !(ep&0x80)) { out=ep; omps=mps; }
        }
        off+=n;
    }
    if (!evt || !in || out!=2 || !emps || !imps || !omps) return -1;
    memset(&bt,0,sizeof(bt));
    bt.dev=dev; bt.vid=vid; bt.pid=pid; bt.ep_evt=evt; bt.ep_in=in; bt.ep_out=out;
    if (bsp_usb_int_in_open(dev,evt,emps,interval) ||
        bsp_usb_bulk_open(dev,in,imps) || bsp_usb_bulk_open(dev,out,omps)) {
        memset(&bt,0,sizeof(bt)); return -1;
    }
    bt.claimed=true; bt.busy=true;
    bt.ready=qca_setup()==0;
    bt.busy=false;
    klog("q8b bt: claimed %04x:%04x ready=%d\n",vid,pid,bt.ready);
    return 0;
}

void bsp_usb_bt_detach(bsp_usb_dev_t *dev) {
    if (bt.claimed && bt.dev==dev) memset(&bt,0,sizeof(bt));
}
bool bsp_usb_bt_attached(bsp_usb_dev_t *dev) { return bt.claimed && bt.dev==dev; }
void bsp_usb_bt_reset(void) { memset(&bt,0,sizeof(bt)); }

int bsp_usb_bt_cntl(vdevice_t *dev,int from_pid,int cmd,proto_t *in,proto_t *out) {
    (void)dev; (void)from_pid;
    switch (cmd) {
    case USBHCI_CMD_INFO:
        PF->addi(out,bt.claimed); PF->addi(out,bt.ready);
        PF->addi(out,bt.vid); PF->addi(out,bt.pid); return 0;
    case USBHCI_CMD_SETUP:
        if (!bt.claimed || bt.busy) return -1;
        if (bt.ready) return 0;
        bt.busy=true; bt.ready=qca_setup()==0; bt.busy=false;
        return bt.ready ? 0 : -1;
    case USBHCI_CMD_SEND: {
        if (!bt.ready) return -1;
        int type=proto_read_int(in); int32_t n=0;
        const uint8_t *p=proto_read(in,&n);
        if (!p || n<=0 || n>(int)sizeof(bt.acl.data)) return -1;
        if (type==H4_CMD) {
            if (n<3 || n!=3+p[2]) return -1;
            usb_setup_pkt_t s={0}; s.bmRequestType=0x20; s.wLength=(uint16_t)n;
            return bsp_usb_control_xfer(bt.dev,&s,(void *)p,false)==n ? 0 : -1;
        }
        if (type==H4_ACL && n>=4 && n==4+le16(p+2))
            return bsp_usb_bulk_xfer(bt.dev,bt.ep_out,(void *)p,n,false)==n ? 0 : -1;
        return -1;
    }
    case USBHCI_CMD_RECV: {
        if (!bt.ready) return -1;
        int max=proto_read_int(in);
        if (max<=0) return -1;
        uint32_t n=bt.wp-bt.rp;
        if (n>(uint32_t)max) n=(uint32_t)max;
        uint8_t p[512];
        while (n) {
            uint32_t step=n>sizeof(p) ? sizeof(p) : n;
            for (uint32_t i=0;i<step;i++) p[i]=bt.rx[bt.rp++ % RX_SIZE];
            PF->add(out,p,step); n-=step;
        }
        return 0;
    }
    case USBHCI_CMD_FLUSH: rx_reset(); return 0;
    case USBHCI_CMD_DIAG: {
        char text[240];
        snprintf(text,sizeof(text),"qca claimed=%u ready=%u USB=%04x:%04x ROM=%08x patch=%08x RAM=%08x rx=%u drop=%u fw=%s",
                 bt.claimed,bt.ready,bt.vid,bt.pid,bt.rom,bt.patch,bt.ram,bt.wp-bt.rp,bt.dropped,bt.firmware);
        PF->add(out,text,strlen(text)+1); return 0;
    }
    default: return -1;
    }
}
