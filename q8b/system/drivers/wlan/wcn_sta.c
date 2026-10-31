/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/* Station WMI protocol reference: Linux ath11k/wmi.c, mac.c, hal_rx.c.
 * Copyright (c) 2018-2019 The Linux Foundation. All rights reserved. */
#include "wcn_sta.h"
#include "wcn_wmi.h"
#include "wcn_wmi_proto.h"
#include "wcn_dp.h"
#include "wcn_wpa.h"
#include "wcn_net.h"
#include "wcn_htc.h"
#include <ewoksys/dma.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/klog.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define STA_DMA_BYTES 16384u
#define FRAME_BYTES 1024u
#define FRAME_SLOTS 4u
#define QUEUE_BASE (FRAME_BYTES*FRAME_SLOTS)
enum { IDLE, CONFIG, PEER, PEER_WAIT, ROUTE, QUEUES, START, START_WAIT,
       AUTH, AUTH_WAIT, ASSOC, ASSOC_WAIT, PEER_ASSOC, UP, ASSOCIATED, FAILED };
static const char *const names[]={"idle","configure","peer-create","peer-wait","route",
    "queues","vdev-start","vdev-wait","authenticate","auth-wait","associate","assoc-wait",
    "peer-assoc","vdev-up","associated-awaiting-wpa","failed"};
static struct {
    unsigned state,config,queue,peer_mapped,peer,hw_peer,aid,seq,desc;
    unsigned tx,tx_done,tx_errors,rx,status,power,lmac,data_rx,eapol_rx,data_tx;
    unsigned pending[FRAME_SLOTS];
    uint8_t *dma;
    uint64_t pa,deadline;
    uint8_t mac[6],rsn[22],rates[12];
    unsigned rates_len;
    struct wcn_bss bss;
    const char *error;
} s;
static unsigned le16(const uint8_t *p) { return p[0]|((unsigned)p[1]<<8); }
static unsigned le32(const uint8_t *p) { return le16(p)|((uint32_t)le16(p+2)<<16); }
static void put16(uint8_t *p,unsigned v) { p[0]=v; p[1]=v>>8; }
static unsigned tlv(unsigned tag,unsigned bytes) { return tag<<16|bytes; }
static void state(unsigned value) {
    s.state=value; s.deadline=kernel_tic_ms(0)+8000;
    klog("wcn: station %s\n",names[value]);
}
static void fail(const char *why,unsigned status) {
    s.error=why; s.status=status; state(FAILED);
    klog("wcn: station failure=%s status=%u; stop/start before retry\n",why,status);
}
/* Negotiate WPA2-PSK/CCMP only. Reject mandatory PMF and unsupported group ciphers. */
static int select_rsn(const uint8_t *ie,unsigned bytes) {
    static const uint8_t ccmp[]={0,15,172,4}, psk[]={0,15,172,2};
    if (bytes<20 || ie[0]!=48 || ie[1]+2u!=bytes || le16(ie+2)!=1 || memcmp(ie+4,ccmp,4)) return -1;
    unsigned pos=8,n=le16(ie+pos),ok=0; pos+=2;
    if (!n || n>(bytes-pos)/4) return -1;
    for (unsigned i=0;i<n;i++) if (!memcmp(ie+pos+4*i,ccmp,4)) ok=1;
    if (!ok) return -1;
    pos+=n*4; if (bytes-pos<2) return -1;
    n=le16(ie+pos); pos+=2; ok=0;
    if (!n || n>(bytes-pos)/4) return -1;
    for (unsigned i=0;i<n;i++) if (!memcmp(ie+pos+4*i,psk,4)) ok=1;
    if (!ok) return -1;
    pos+=n*4;
    if (pos<bytes && (bytes-pos<2 || (le16(ie+pos)&64))) return -1;
    memset(s.rsn,0,sizeof(s.rsn)); s.rsn[0]=48; s.rsn[1]=20;
    put16(s.rsn+2,1); memcpy(s.rsn+4,ccmp,4); put16(s.rsn+8,1);
    memcpy(s.rsn+10,ccmp,4); put16(s.rsn+14,1); memcpy(s.rsn+16,psk,4);
    return 0;
}
static int select_rates(void) {
    static const uint8_t supported[]={2,4,11,22,12,18,24,36,48,72,96,108};
    for (unsigned i=0;i<s.bss.rates_len;i++) {
        unsigned r=s.bss.rates[i]&127,j;
        for (j=0;j<sizeof(supported);j++) if (r==supported[j]) break;
        if (j==sizeof(supported)) { if (s.bss.rates[i]&128) return -1; continue; }
        unsigned duplicate=0;
        for (j=0;j<s.rates_len;j++) if ((s.rates[j]&127)==r) duplicate=1;
        if (!duplicate && s.rates_len<sizeof(s.rates)) s.rates[s.rates_len++]=s.bss.rates[i];
    }
    return s.rates_len ? 0 : -1;
}
int wcn_sta_associate(const char *ssid) {
    if (!ssid || !*ssid || strlen(ssid)>32 || s.state!=IDLE ||
        !wcn_wmi_station_available() || !wcn_dp_ready() || wcn_wmi_find_bss(ssid,&s.bss)) return -1;
    s.rates_len=0;
    int power=wcn_wmi_channel_power(s.bss.mhz);
    if (power<0 || !s.bss.privacy || select_rsn(s.bss.rsn,s.bss.rsn_len) || select_rates()) return -1;
    s.power=power>10 ? 10 : power;
    s.dma=(uint8_t *)dma_alloc(0,STA_DMA_BYTES);
    if (!s.dma) return -1;
    s.pa=dma_phy_addr(0,(ewokos_addr_t)s.dma);
    if (!s.pa || s.pa+STA_DMA_BYTES>0x100000000ULL) {
        dma_free(0,(ewokos_addr_t)s.dma); s.dma=NULL; return -1;
    }
    memset(s.dma,0,STA_DMA_BYTES); wcn_wmi_mac(s.mac);
    state(CONFIG);
    klog("wcn: station BSSID=%02x:%02x:%02x:%02x:%02x:%02x MHz=%u power=%u WPA2-PSK CCMP\n",
         s.bss.bssid[0],s.bss.bssid[1],s.bss.bssid[2],s.bss.bssid[3],s.bss.bssid[4],s.bss.bssid[5],s.bss.mhz,s.power);
    return 0;
}
int wcn_sta_busy(void) { return s.state!=IDLE; }
static int configure(void) {
    if (!s.config) {
        const uint32_t p[]={0x401e,(515u<<16)|12,0,1,0}; /* DBS, same mode as INIT. */
        return wcn_htc_wmi_send(p,sizeof(p));
    }
    static const unsigned params[][2]={{48,1},{91,2},{34,1},{1,2347},{2,2346},{18,10},{19,20}};
    unsigned i=s.config-1;
    uint32_t p[]={0x5008,(95u<<16)|12,0,params[i][0],params[i][1]};
    return wcn_htc_wmi_send(p,sizeof(p));
}
static int peer_create(void) {
    struct { uint32_t command; struct wmi_peer_create_cmd cmd; } p;
    memset(&p,0,sizeof(p)); p.command=0x6001; p.cmd.tlv_header=tlv(97,sizeof(p.cmd)-4);
    memcpy(p.cmd.peer_macaddr.addr,s.bss.bssid,6);
    return wcn_htc_wmi_send(&p,sizeof(p));
}
static int route(void) {
    struct { uint32_t command; struct wmi_peer_set_param_cmd cmd; } p;
    memset(&p,0,sizeof(p)); p.command=0x6004; p.cmd.tlv_header=tlv(100,sizeof(p.cmd)-4);
    memcpy(p.cmd.peer_macaddr.addr,s.bss.bssid,6);
    p.cmd.param_id=0x13; p.cmd.param_value=3; /* RX hash enabled, REO2SW1. */
    return wcn_htc_wmi_send(&p,sizeof(p));
}
static int queue_setup(void) {
    unsigned tid=s.queue;
    uint32_t *q=(uint32_t *)(void *)(s.dma+QUEUE_BASE+tid*512);
    if (!q[0]) {
        q[0]=4|(8<<4); q[1]=tid;
        unsigned ac=(tid==1||tid==2) ? 0 : (tid==4||tid==5) ? 2 : (tid==6||tid==7) ? 3 : 1;
        q[2]=3|(ac<<5)|(1u<<25)|(tid==16 ? 1u<<8 : 1u<<11);
        if (tid!=16) for (unsigned i=0;i<3;i++) q[32+i*32]=4|(9<<4);
    }
    __asm__ volatile("dsb sy" ::: "memory");
    struct { uint32_t command; struct wmi_peer_reorder_queue_setup_cmd cmd; } p;
    memset(&p,0,sizeof(p)); p.command=0x6013; p.cmd.tlv_header=tlv(549,sizeof(p.cmd)-4);
    memcpy(p.cmd.peer_macaddr.addr,s.bss.bssid,6);
    uint64_t pa=s.pa+QUEUE_BASE+tid*512;
    p.cmd.tid=p.cmd.queue_no=tid; p.cmd.queue_ptr_lo=pa; p.cmd.queue_ptr_hi=pa>>32;
    p.cmd.ba_window_size_valid=1; p.cmd.ba_window_size=1;
    return wcn_htc_wmi_send(&p,sizeof(p));
}
static int vdev_start(void) {
    struct { uint32_t command; struct wmi_vdev_start_request_cmd cmd; struct wmi_channel ch; uint32_t noa; } p;
    memset(&p,0,sizeof(p)); p.command=0x5003; p.cmd.tlv_header=tlv(88,sizeof(p.cmd)-4);
    p.cmd.beacon_interval=s.bss.beacon_interval; p.cmd.dtim_period=1;
    p.cmd.preferred_tx_streams=p.cmd.preferred_rx_streams=1;
    p.ch.tlv_header=tlv(80,sizeof(p.ch)-4); p.ch.mhz=p.ch.band_center_freq1=s.bss.mhz;
    p.ch.info=s.bss.mhz<3000 ? 1 : 0;
    p.ch.reg_info_1=(s.power<<8)|(s.power<<16); p.ch.reg_info_2=s.power<<8;
    p.noa=tlv(18,0);
    return wcn_htc_wmi_send(&p,sizeof(p));
}
static int management(unsigned assoc) {
    unsigned slot;
    for (slot=0;slot<FRAME_SLOTS;slot++) if (!s.pending[slot]) break;
    if (slot==FRAME_SLOTS) return -1;
    uint8_t *f=s.dma+slot*FRAME_BYTES;
    memset(f,0,FRAME_BYTES); f[0]=assoc ? 0 : 0xb0;
    memcpy(f+4,s.bss.bssid,6); memcpy(f+10,s.mac,6); memcpy(f+16,s.bss.bssid,6);
    put16(f+22,(++s.seq&4095)<<4);
    unsigned len=30;
    if (!assoc) put16(f+26,1); /* Open-system auth transaction1. */
    else {
        put16(f+24,(s.bss.capability&(1|16|32|1024))|1); put16(f+26,10); len=28;
        unsigned n=strlen(s.bss.ssid); f[len++]=0; f[len++]=n; memcpy(f+len,s.bss.ssid,n); len+=n;
        n=s.rates_len>8 ? 8 : s.rates_len; f[len++]=1; f[len++]=n;
        memcpy(f+len,s.rates,n); len+=n;
        if (s.rates_len>8) { f[len++]=50; f[len++]=s.rates_len-8; memcpy(f+len,s.rates+8,s.rates_len-8); len+=s.rates_len-8; }
        memcpy(f+len,s.rsn,sizeof(s.rsn)); len+=sizeof(s.rsn);
    }
    struct { uint32_t command; struct wmi_mgmt_send_cmd cmd; uint32_t array; uint8_t frame[64]; } p;
    memset(&p,0,sizeof(p)); p.command=0x7008; p.cmd.tlv_header=tlv(422,sizeof(p.cmd)-4);
    unsigned id=++s.desc; if (!id) id=++s.desc;
    p.cmd.desc_id=id; p.cmd.chanfreq=s.bss.mhz;
    uint64_t pa=s.pa+slot*FRAME_BYTES; p.cmd.paddr_lo=pa; p.cmd.paddr_hi=pa>>32;
    p.cmd.frame_len=len; p.cmd.buf_len=len>64 ? 64 : len;
    unsigned padded=(p.cmd.buf_len+3)&~3u;
    p.array=tlv(17,padded); memcpy(p.frame,f,p.cmd.buf_len);
    __asm__ volatile("dsb sy" ::: "memory");
    if (wcn_htc_wmi_send(&p,sizeof(p)-64+padded)) return -1;
    s.pending[slot]=id; s.tx++;
    return 0;
}
static int peer_assoc(void) {
    uint8_t packet[512]; memset(packet,0,sizeof(packet));
    uint32_t command=0x6005; memcpy(packet,&command,4);
    struct wmi_peer_assoc_complete_cmd *p=(void *)(packet+4);
    p->tlv_header=tlv(101,sizeof(*p)-4); memcpy(p->peer_macaddr.addr,s.bss.bssid,6);
    p->peer_new_assoc=1; p->peer_associd=s.aid; p->peer_flags=4;
    p->peer_caps=s.bss.capability; p->peer_listen_intval=10; p->peer_nss=1;
    p->peer_phymode=s.bss.mhz<3000 ? 1 : 0; p->num_peer_legacy_rates=s.rates_len;
    unsigned off=4+sizeof(*p),n=(s.rates_len+3)&~3u,v=tlv(17,n);
    memcpy(packet+off,&v,4); off+=4;
    for (unsigned i=0;i<s.rates_len;i++) packet[off+i]=s.rates[i]&127;
    off+=n; v=tlv(17,0); memcpy(packet+off,&v,4); off+=4;
    struct wmi_vht_rate_set *vht=(void *)(packet+off); vht->tlv_header=tlv(102,sizeof(*vht)-4); off+=sizeof(*vht);
    v=tlv(18,0); memcpy(packet+off,&v,4); off+=4;
    return wcn_htc_wmi_send(packet,off);
}
static int vdev_up(void) {
    struct { uint32_t command; struct wmi_vdev_up_cmd cmd; } p;
    memset(&p,0,sizeof(p)); p.command=0x5005; p.cmd.tlv_header=tlv(92,sizeof(p.cmd)-4);
    p.cmd.vdev_assoc_id=s.aid; memcpy(p.cmd.vdev_bssid.addr,s.bss.bssid,6);
    return wcn_htc_wmi_send(&p,sizeof(p));
}
void wcn_sta_poll(void) {
    if (s.state==ASSOCIATED) {
        if (!wcn_dp_ready() || !wcn_wmi_ready()) { fail("controller-unavailable",0);return; }
        wcn_wpa_poll();
        if (wcn_wpa_failed()) { fail("WPA-failed",0); return; }
        if (!s.data_tx) {
            uint8_t f[36]={8,1};
            memcpy(f+4,s.bss.bssid,6); memcpy(f+10,s.mac,6); memcpy(f+16,s.bss.bssid,6);
            const uint8_t llc[]={0xaa,0xaa,3,0,0,0,0x88,0x8e,2,1,0,0};
            memcpy(f+24,llc,sizeof(llc));
            if (wcn_dp_transmit(f,sizeof(f),s.lmac)>=0) s.data_tx++;
        }
        return;
    }
    if (s.state==IDLE || s.state==FAILED) return;
    if (!wcn_wmi_ready() || !wcn_dp_ready()) { fail("controller-unavailable",0); return; }
    if (kernel_tic_ms(0)>=s.deadline) { fail("timeout",s.state); return; }
    switch (s.state) {
    case CONFIG: if (!configure() && ++s.config==8) state(PEER); break;
    case PEER: if (!peer_create()) state(PEER_WAIT); break;
    case PEER_WAIT: if (s.peer_mapped) state(ROUTE); break;
    case ROUTE: if (!route()) state(QUEUES); break;
    case QUEUES: if (!queue_setup() && ++s.queue==17) state(START); break;
    case START: if (!vdev_start()) state(START_WAIT); break;
    case AUTH: if (!management(0)) state(AUTH_WAIT); break;
    case ASSOC: if (!management(1)) state(ASSOC_WAIT); break;
    case PEER_ASSOC: if (!peer_assoc()) state(UP); break;
    case UP: if (!vdev_up()) state(ASSOCIATED); break;
    default: break;
    }
}
void wcn_sta_event(unsigned event,unsigned tag,const uint8_t *p,unsigned bytes) {
    if (s.state==ASSOCIATED && event==0xb001 && tag==48 && bytes>=12 && !le32(p) && le32(p+4)==2)
        fail("beacon-loss",0);
    if (event==0x5003 && tag==42 && bytes>=24 && !le32(p) && !memcmp(p+4,s.bss.bssid,6))
        wcn_wpa_key_complete(le32(p+12),le32(p+16),le32(p+20));
    if (event==0x7006 && tag==423 && bytes>=20) {
        unsigned id=le32(p);
        for (unsigned i=0;i<FRAME_SLOTS;i++) if (id && s.pending[i]==id) {
            s.pending[i]=0; s.tx_done++; if (le32(p+4)) s.tx_errors++;
            klog("wcn: management tx id=%u status=%u\n",id,le32(p+4));
        }
    }
    if (s.state==START_WAIT && event==0x5001 && tag==40 && bytes>=40 && !le32(p)) {
        /* WCN6855 exposes one host pdev; firmware routes its physical MAC.
         * The response union carries a target pdev ID, not a host LMAC index. */
        s.lmac=0;
        if (le32(p+12)) fail("vdev-start-rejected",le32(p+12));
        else state(AUTH);
    }
}
void wcn_sta_peer(unsigned vdev,const uint8_t *mac,unsigned peer,unsigned hw_peer) {
    if ((s.state==PEER || s.state==PEER_WAIT) && !vdev && !memcmp(mac,s.bss.bssid,6)) {
        s.peer_mapped=1; s.peer=peer; s.hw_peer=hw_peer;
        klog("wcn: AP peer mapped id=%u hw=%u\n",peer,hw_peer);
    }
}
void wcn_sta_management(const uint8_t *p,unsigned bytes,unsigned mhz) {
    if (s.state==IDLE || s.state==FAILED || bytes<24 || mhz!=s.bss.mhz ||
        (p[0]&15) || memcmp(p+4,s.mac,6) || memcmp(p+10,s.bss.bssid,6) || memcmp(p+16,s.bss.bssid,6)) return;
    unsigned subtype=p[0]>>4; s.rx++;
    if ((subtype==12 || subtype==10) && bytes>=26) { fail("AP-disconnected",le16(p+24)); return; }
    if (s.state==AUTH_WAIT && subtype==11 && bytes>=30 && !le16(p+24) && le16(p+26)==2) {
        if (le16(p+28)) fail("authentication-rejected",le16(p+28)); else state(ASSOC);
    }
    if (s.state==ASSOC_WAIT && subtype==1 && bytes>=30) {
        if (le16(p+26)) { fail("association-rejected",le16(p+26)); return; }
        s.aid=le16(p+28)&0x3fff;
        if (!s.aid || s.aid>2007) { fail("invalid-association-id",s.aid); return; }
        state(PEER_ASSOC);
    }
}
void wcn_sta_data(const uint8_t *frame,unsigned bytes,const struct wcn_rx_security *security) {
    if (bytes<14 || s.state<PEER_ASSOC || s.state==FAILED || !security ||
        memcmp(security->transmitter,s.bss.bssid,6)) return;
    s.data_rx++;
    if (frame[12]==0x88 && frame[13]==0x8e) {
        if (memcmp(frame+6,s.bss.bssid,6) || memcmp(frame,s.mac,6)) return;
        s.eapol_rx++;
        wcn_wpa_receive(frame+14,bytes-14);
        klog("wcn: EAPOL received bytes=%u type=%u\n",bytes,bytes>15 ? frame[15]:255);
    } else if (security->valid && security->encrypted &&
               wcn_wpa_accept_rx(security->group,security->key,security->tid,security->pn))
        wcn_net_receive(frame,bytes);
}
void wcn_sta_release(void) {
    if (s.dma) dma_free(0,(ewokos_addr_t)s.dma);
    wcn_wpa_reset();
    wcn_net_reset();
    memset(&s,0,sizeof(s));
}
void wcn_sta_status(char *out,size_t size) {
    snprintf(out,size,"station=%s error=%s status=%u mhz=%u peer=%u mapped=%u queues=%u aid=%u tx=%u done=%u tx_errors=%u rx=%u lmac=%u data_rx=%u eapol_rx=%u data_tx=%u network_ready=%u\n",
             wcn_sta_link_ready()?"connected":names[s.state],s.error?s.error:"none",s.status,s.bss.mhz,s.peer,s.peer_mapped,s.queue,s.aid,s.tx,s.tx_done,s.tx_errors,s.rx,s.lmac,s.data_rx,s.eapol_rx,s.data_tx,wcn_sta_link_ready());
    size_t n=strlen(out);
    if (n<size) wcn_wpa_status(out+n,size-n);
}

int wcn_sta_eapol_send(const uint8_t *packet,unsigned bytes) {
    if (s.state!=ASSOCIATED || bytes>1536) return -1;
    uint8_t f[1568]={8,1};
    if (wcn_wpa_established()) f[1]|=0x40;
    memcpy(f+4,s.bss.bssid,6); memcpy(f+10,s.mac,6); memcpy(f+16,s.bss.bssid,6);
    put16(f+22,(++s.seq&4095)<<4);
    const uint8_t llc[]={0xaa,0xaa,3,0,0,0,0x88,0x8e};
    memcpy(f+24,llc,8); memcpy(f+32,packet,bytes);
    int ret=wcn_dp_transmit(f,bytes+32,s.lmac);
    if (ret>=0) s.data_tx++;
    return ret;
}
int wcn_sta_install_key(unsigned index,unsigned group,const uint8_t key[16],const uint8_t *rsc) {
    if (s.state!=ASSOCIATED || index>3 || group>1) return -1;
    struct { uint32_t command; struct wmi_vdev_install_key_cmd cmd; uint32_t array; uint8_t key[16]; } p;
    memset(&p,0,sizeof(p)); p.command=0x5009; p.cmd.tlv_header=tlv(96,sizeof(p.cmd)-4);
    memcpy(p.cmd.peer_macaddr.addr,s.bss.bssid,6);p.cmd.key_idx=index;p.cmd.key_flags=group;
    p.cmd.key_cipher=4;p.cmd.key_len=16;
    if (rsc) { p.cmd.key_rsc_counter.key_seq_counter_l=le32(rsc);p.cmd.key_rsc_counter.key_seq_counter_h=le32(rsc+4); }
    p.array=tlv(17,16);memcpy(p.key,key,16);
    int ret=wcn_htc_wmi_send(&p,sizeof(p));
    volatile uint8_t *v=(void *)&p;for (unsigned i=0;i<sizeof(p);i++) v[i]=0;
    return ret;
}
int wcn_sta_authorize(void) {
    struct { uint32_t command; struct wmi_peer_set_param_cmd cmd; } p;
    memset(&p,0,sizeof(p));p.command=0x6004;p.cmd.tlv_header=tlv(100,sizeof(p.cmd)-4);
    memcpy(p.cmd.peer_macaddr.addr,s.bss.bssid,6);p.cmd.param_id=3;p.cmd.param_value=1;
    return wcn_htc_wmi_send(&p,sizeof(p));
}
static int hex_seed(const char *p,uint8_t seed[32]) {
    if (strlen(p)!=64) return -1;
    for (unsigned i=0;i<32;i++) {
        unsigned v=0;
        for (unsigned j=0;j<2;j++) {
            char c=p[i*2+j];unsigned n;
            if (c>='0' && c<='9') n=c-'0';else if (c>='a' && c<='f') n=c-'a'+10;
            else if (c>='A' && c<='F') n=c-'A'+10;else return -1;
            v=v*16+n;
        }
        seed[i]=v;
    }
    return 0;
}
int wcn_sta_connect_file(const char *path) {
    if (s.state!=IDLE || !wcn_wmi_station_available() || !wcn_dp_ready()) return -1;
    /* Private one-use config: SSID, passphrase, 32-byte random seed in hex.
     * Remove immediately after reading. Seed must come from a trusted CSPRNG. */
    FILE *fp=fopen(path,"r");if (!fp) return -1;
    char config[192];uint8_t seed[32];memset(config,0,sizeof(config));memset(seed,0,sizeof(seed));
    unsigned n=fread(config,1,sizeof(config)-1,fp);int extra=fgetc(fp);fclose(fp);
    int ret=-1;
    if (unlink(path) || extra!=EOF || !n || memchr(config,0,n)) goto done;
    char *ssid=config,*pass=strchr(ssid,'\n');if (!pass) goto done;*pass++=0;
    char *entropy=strchr(pass,'\n');if (!entropy) goto done;*entropy++=0;
    char *end=strchr(entropy,'\n');if (end) { *end++=0;if (*end) goto done; }
    if (hex_seed(entropy,seed) || strlen(pass)<8 || strlen(pass)>63 || wcn_sta_associate(ssid)) goto done;
    if (wcn_wpa_init(ssid,pass,seed,s.mac,s.bss.bssid,s.bss.rsn,s.bss.rsn_len,s.rsn)) {
        fail("WPA-initialization",0);goto done;
    }
    ret=0;
done:
    { volatile uint8_t *v=(void *)config;for(unsigned i=0;i<sizeof(config);i++) v[i]=0;
      v=seed;for(unsigned i=0;i<sizeof(seed);i++) v[i]=0; }
    return ret;
}

int wcn_sta_link_ready(void) {
    return s.state==ASSOCIATED && wcn_wpa_ready() && wcn_dp_ready() && wcn_wmi_ready();
}
int wcn_sta_network_send(const uint8_t *frame,unsigned bytes) {
    if (!wcn_sta_link_ready() || bytes<14 || bytes>1514 || memcmp(frame+6,s.mac,6)) return -1;
    /* EAPOL belongs to the supplicant, never accept a second VFS writer. */
    if (frame[12]==0x88 && frame[13]==0x8e) return -1;
    uint8_t f[1536]={8,0x41}; /* ToDS, protected; firmware adds CCMP. */
    memcpy(f+4,s.bss.bssid,6);memcpy(f+10,s.mac,6);memcpy(f+16,frame,6);
    put16(f+22,(++s.seq&4095)<<4);
    const uint8_t llc[]={0xaa,0xaa,3,0,0,0};memcpy(f+24,llc,6);
    memcpy(f+30,frame+12,bytes-12);
    return wcn_dp_transmit(f,bytes+18,s.lmac);
}
