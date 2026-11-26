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
#include "wcn_util.h"
#include <ewoksys/dma.h>
#include <ewoksys/kernel_tic.h>
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
    unsigned tx,tx_done,tx_errors,rx,status,power,lmac,data_rx,eapol_rx,data_tx,rx_base;
    unsigned pending[FRAME_SLOTS];
    uint8_t *dma;
    uint64_t pa,deadline,rx_grace;
    uint8_t mac[6],rsn[22],rates[12];
    unsigned ba_delete,ba_state,ba_tid,ba_token,ba_window,ba_ssn,ba_timeout,ba_cookie,ba_status,ba_requests,ba_accepted;
    uint64_t ba_deadline;
    unsigned rates_len,ht,vht,he,nss,width,center,phymode,ht_rates_len,max_width,he_len,he_allowed;
    unsigned tx_done_base;
    uint64_t probe_ms;
    /* Wake-policy apply mask (bit0 RX_WAKE, bit1 TX_WAKE, bit2 PSPOLL,
     * bit3 PS mode=0) and retry clock: the CONFIG burst can run the WMI
     * endpoint out of credits, so a failed send is retried after WPA. */
    unsigned ps_done;
    uint64_t ps_retry;
    uint8_t ht_rates[16], ht_ie[26], vht_ie[12];
    uint8_t he_ie[64];
    struct wcn_bss bss;
    const char *error;
} s;
static unsigned le16(const uint8_t *p) { return p[0]|((unsigned)p[1]<<8); }
static unsigned le32(const uint8_t *p) { return le16(p)|((uint32_t)le16(p+2)<<16); }
static void put16(uint8_t *p,unsigned v) { p[0]=v; p[1]=v>>8; }
static unsigned tlv(unsigned tag,unsigned bytes) { return tag<<16|bytes; }
static int sta_ps_mode(int mode);
static int sta_ps_param(unsigned param,unsigned value);
static void sta_ps_configure(void);
enum { WMI_VDEV_PARAM_RX_DECAP_TYPE=0x57, RX_DECAP_ETHERNET2=2 };
static void state(unsigned value) {
    s.state=value; s.deadline=kernel_tic_ms(0)+8000;
}
static void fail(const char *why,unsigned status) {
    /* Recovery frees the station and DP state; keep the reason and counters
     * across reboots in the persistent diag - a link drop takes the ssh
     * session with it, so the ring log alone could never be read afterwards. */
    wcn_diag("station-fail why=%s status=%u phase=%s mhz=%u aid=%u mgmt=%u/%u err=%u data=%u/%u eapol=%u\n",
             why,status,names[s.state],s.bss.mhz,s.aid,s.tx,s.tx_done,s.tx_errors,
             s.data_tx,s.data_rx,s.eapol_rx);
    wcn_log("wcn: station failure=%s status=%u phase=%s; recovery required\n",why,status,names[s.state]);
    s.error=why; s.status=status; state(FAILED);
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

/* Optional local bandwidth cap; 160 MHz is selected only with matching AP,
 * firmware and regulatory capabilities. Reload on each association/recovery. */
static int width_limit(void) {
    FILE *fp=fopen("/etc/wlan.width","r");if (!fp) return 160;
    char value[6]={0};unsigned n=fread(value,1,5,fp);int extra=fgetc(fp);fclose(fp);
    if (extra!=EOF || !n) return -1;
    if (value[n-1]=='\n') value[--n]=0;
    if (n==2 && !memcmp(value,"20",2)) return 20;
    if (n==2 && !memcmp(value,"40",2)) return 40;
    if (n==2 && !memcmp(value,"80",2)) return 80;
    if (n==3 && !memcmp(value,"160",3)) return 160;
    return -1;
}
/* Prefer HE when supported; an explicit VHT preference remains available. */
static int he_enabled(void) {
    FILE *fp=fopen("/etc/wlan.phy","r");if (!fp) return 1;
    char value[5]={0};unsigned n=fread(value,1,4,fp);int extra=fgetc(fp);fclose(fp);
    if (extra!=EOF || !n) return -1;
    if (value[n-1]=='\n') value[--n]=0;
    if (n==2 && !memcmp(value,"he",2)) return 1;
    if (n==3 && !memcmp(value,"vht",3)) return 0;
    return -1;
}

static unsigned he_map(unsigned local,unsigned peer,unsigned nss,unsigned ldpc) {
    unsigned out=0xffff;
    for (unsigned i=0;i<nss;i++) {
        unsigned a=(local>>(2*i))&3,b=(peer>>(2*i))&3;
        if (a==3 || b==3) continue;
        unsigned m=a<b ? a:b;
        if (!ldpc && m>1) m=1; /* HE MCS 10/11 requires LDPC. */
        out=(out&~(3u<<(2*i)))|(m<<(2*i));
    }
    return out;
}
static void select_he(void) {
    const struct wcn_he_caps *hw=wcn_wmi_he_caps(),*ap=&s.bss.he;
    s.he=s.he_len=0;memset(s.he_ie,0,sizeof(s.he_ie));
    if (!s.he_allowed || !s.vht || !hw->present || !ap->present || !s.bss.he_operation) return;
    if (s.bss.he_op[0]&8) return; /* TWT-required HE BSS is unsupported. */
    if (s.width==160 && (!(hw->phy[0]&ap->phy[0]&8) || wcn_wmi_nss160()<s.nss)) return;
    /* LDPC is mandatory at widths above 20 MHz. Optional features are
     * advertised only when their receive path is supported locally. */
    if (s.width>20 && !(hw->phy[1]&ap->phy[1]&32)) return;
    if (s.width>=40 && !(hw->phy[0]&ap->phy[0]&4)) return;
    unsigned count=s.width==160 ? 2:1,rx[2],tx[2];
    for (unsigned i=0;i<count;i++) {
        rx[i]=he_map(le16(hw->mcs+4*i),le16(ap->mcs+4*i+2),s.nss,hw->phy[1]&32);
        tx[i]=he_map(le16(hw->mcs+4*i+2),le16(ap->mcs+4*i),s.nss,ap->phy[1]&32);
        for (unsigned ss=0;ss<s.nss;ss++)
            if (((rx[i]>>(2*ss))&3)==3 || ((tx[i]>>(2*ss))&3)==3) return;
    }
    unsigned basic=le16(s.bss.he_op+4);
    for (unsigned i=0;i<count;i++) for (unsigned ss=0;ss<8;ss++) {
        unsigned required=(basic>>(2*ss))&3,r=(rx[i]>>(2*ss))&3,t=(tx[i]>>(2*ss))&3;
        if (required!=3 && (r==3 || t==3 || r<required || t<required)) return;
    }
    s.he_ie[0]=hw->mac[0]&1; /* Firmware handles HE control fields. */
    s.he_ie[1]=hw->mac[1]&12; /* Trigger padding, no TWT/fragmentation offload. */
    s.he_ie[3]=hw->mac[3]&2; /* Firmware operating-mode control. */
    uint8_t *phy=s.he_ie+6;
    phy[0]=s.width>=40 ? 4:0;if (s.width==160) phy[0]|=8;
    phy[1]=hw->phy[1]&(16|32|64);phy[7]=hw->phy[7]&4;
    phy[9]=hw->phy[9]&192;
    unsigned off=17;
    for (unsigned i=0;i<count;i++) { put16(s.he_ie+off,rx[i]);put16(s.he_ie+off+2,tx[i]);off+=4; }
    if (hw->phy[6]&128) {
        unsigned nss=(hw->ppe[0]&7)+1,ru=(hw->ppe[0]>>3)&15;
        if (nss<s.nss || !ru) return;
        unsigned bytes=(7+6*s.nss*__builtin_popcount(ru)+7)/8;
        phy[6]|=128;memcpy(s.he_ie+off,hw->ppe,bytes);
        s.he_ie[off]=(s.he_ie[off]&~7u)|(s.nss-1);
        unsigned tail=(7+6*s.nss*__builtin_popcount(ru))%8;
        if (tail) s.he_ie[off+bytes-1]&=(1u<<tail)-1;
        off+=bytes;
    }
    s.he_len=off;s.he=1;
    s.phymode=s.width==160 ? 20:s.width==80 ? 18:s.width==40 ? 17:16;
}

static void select_phy(void) {
    s.he=s.he_len=0;
    unsigned limit=s.max_width ? s.max_width:160;
    s.nss=1;s.width=20;s.center=s.bss.mhz;s.phymode=s.bss.mhz<3000 ? 1:0;
    s.ht=s.bss.ht && s.bss.wmm && s.bss.ht_cap[3];
    if (!s.ht) return;
    s.nss=wcn_wmi_chains()>1 && s.bss.ht_cap[4] ? 2:1;
    unsigned cap=le16(s.bss.ht_cap);
    if (limit>=40 && (cap&2) && (s.bss.ht_op[1]&4)) {
        unsigned secondary=s.bss.ht_op[1]&3;
        unsigned center=secondary==1 ? s.bss.mhz+10:secondary==3 ? s.bss.mhz-10:0;
        if (center && wcn_wmi_channel_width_power(s.bss.mhz,center,40)>=0) { s.width=40;s.center=center; }
    }
    s.vht=s.bss.vht && s.bss.mhz>3000 && (le16(s.bss.vht_cap+4)&3)!=3 && (le16(s.bss.vht_cap+8)&3)!=3;
    if (s.vht) {
        if (((le16(s.bss.vht_cap+4)>>2)&3)==3 || ((le16(s.bss.vht_cap+8)>>2)&3)==3) s.nss=1;
        unsigned wide=wcn_wmi_bss_160_center(&s.bss);
        unsigned center=5000+5*s.bss.vht_op[1];
        if (s.bss.vht_op[0]==2 && wide) center=s.bss.mhz>wide ? wide+40:wide-40;
        unsigned delta=s.bss.mhz>center ? s.bss.mhz-center:center-s.bss.mhz;
        if (limit>=80 && (s.bss.vht_op[0]==1 || (s.bss.vht_op[0]==2 && wide)) && (delta==10 || delta==30) &&
            wcn_wmi_channel_width_power(s.bss.mhz,center,80)>=0) { s.width=80;s.center=center; }
        unsigned hw=(wcn_wmi_vht_caps()>>2)&3;
        if (limit>=160 && wide && (hw==1 || hw==2) && wcn_wmi_nss160()>=s.nss &&
            wcn_wmi_station_width_power(&s.bss,wide,160)>=0) { s.width=160;s.center=wide; }
    }
    s.phymode=s.vht ? (s.width==160 ? 15:s.width==80 ? 10:s.width==40 ? 9:8):
        s.bss.mhz<3000 ? (s.width==40 ? 7:5):(s.width==40 ? 6:4);
    unsigned local=12|(cap&32); /* SMPS disabled, optional SGI20. */
    if (s.width>=40) local|=2|(cap&64);
    put16(s.ht_ie,local);s.ht_ie[2]=3; /* 64 KiB A-MPDU, no density constraint. */
    for (unsigned i=0;i<s.nss*8;i++) if (s.bss.ht_cap[3+i/8]&(1u<<(i%8))) {
        s.ht_rates[s.ht_rates_len++]=i;s.ht_ie[3+i/8]|=1u<<(i%8);
    }
    s.ht_ie[15]=1; /* TX MCS defined, equal TX/RX sets. */
    unsigned vcap=(3u<<23)|(s.width>=80 ? le32(s.bss.vht_cap)&32:0);
    if (s.width==160) vcap|=4|(wcn_wmi_vht_caps()&le32(s.bss.vht_cap)&64);
    memcpy(s.vht_ie,&vcap,4);
    unsigned rx=0xffff,tx=0xffff;
    for (unsigned i=0;i<s.nss;i++) {
        unsigned a=(le16(s.bss.vht_cap+4)>>(2*i))&3,b=(le16(s.bss.vht_cap+8)>>(2*i))&3;
        rx=(rx&~(3u<<(2*i)))|(b<<(2*i));tx=(tx&~(3u<<(2*i)))|(a<<(2*i));
    }
    put16(s.vht_ie+4,rx);put16(s.vht_ie+8,tx);
    select_he();
    /* REO queues reserve all three extensions for the HE 256-frame window. */
    if (s.he) {
        vcap=(vcap&~(7u<<23))|(wcn_wmi_vht_caps()&((7u<<23)|16));
        memcpy(s.vht_ie,&vcap,4);
    }
}
static unsigned phy_ies(uint8_t *out) {
    if (!s.ht) return 0;
    const uint8_t wmm[]={221,7,0,80,242,2,0,1,0};
    memcpy(out,wmm,sizeof(wmm));unsigned n=sizeof(wmm);
    out[n++]=45;out[n++]=26;memcpy(out+n,s.ht_ie,26);n+=26;
    if (s.vht) { out[n++]=191;out[n++]=12;memcpy(out+n,s.vht_ie,12);n+=12; }
    if (s.he) { out[n++]=255;out[n++]=s.he_len+1;out[n++]=35;memcpy(out+n,s.he_ie,s.he_len);n+=s.he_len; }
    return n;
}

int wcn_sta_associate(const char *ssid) {
    if (!ssid || !*ssid || strlen(ssid)>32 || s.state!=IDLE ||
        !wcn_wmi_station_available() || !wcn_dp_ready() || wcn_wmi_find_bss(ssid,&s.bss)) return -1;
    int limit=width_limit(),he=he_enabled();if (limit<0 || he<0) return -1;
    s.max_width=limit;s.he_allowed=he;
    s.rates_len=s.ht_rates_len=0;
    s.ht=s.vht=s.he=s.he_len=0;memset(s.ht_ie,0,sizeof(s.ht_ie));memset(s.vht_ie,0,sizeof(s.vht_ie));
    int power=wcn_wmi_channel_power(s.bss.mhz);
    if (power<0 || !s.bss.privacy || select_rsn(s.bss.rsn,s.bss.rsn_len) || select_rates()) return -1;
    select_phy();
    power=wcn_wmi_station_width_power(&s.bss,s.center,s.width);
    if (power<0) return -1;
    /* Use the validated regulatory limit for the entire channel width.
     * Firmware retains the calibrated board/hardware transmit limits. */
    s.power=power;
    s.dma=(uint8_t *)dma_alloc(0,STA_DMA_BYTES);
    if (!s.dma) return -1;
    s.pa=dma_phy_addr(0,(ewokos_addr_t)s.dma);
    if (!s.pa || s.pa+STA_DMA_BYTES>0x100000000ULL) {
        dma_free(0,(ewokos_addr_t)s.dma); s.dma=NULL; return -1;
    }
    memset(s.dma,0,STA_DMA_BYTES); wcn_wmi_mac(s.mac);
    wcn_log("wcn: join bssid=%02x:%02x:%02x:%02x:%02x:%02x mhz=%u rssi=%d phy=%s width=%u nss=%u\n",
            s.bss.bssid[0],s.bss.bssid[1],s.bss.bssid[2],s.bss.bssid[3],
            s.bss.bssid[4],s.bss.bssid[5],s.bss.mhz,s.bss.rssi,
            s.he?"HE":s.vht?"VHT":s.ht?"HT":"legacy",s.width,s.nss);
    state(CONFIG);
    return 0;
}
int wcn_sta_busy(void) { return s.state!=IDLE; }
int wcn_sta_failed(void) { return s.state==FAILED; }
int wcn_sta_resume(void) {
    if (wcn_sta_associate(wcn_wpa_profile_ssid())) return -1;
    if (wcn_wpa_restart(s.mac,s.bss.bssid,s.bss.rsn,s.bss.rsn_len,s.rsn)) {
        fail("WPA-restart",0);return -1;
    }
    return 0;
}
static int configure(void) {
    if (s.config==10) {
        uint32_t p[]={0x5008,(95u<<16)|12,0,0x8003,le32(s.bss.he_op)};
        return wcn_htc_wmi_send(p,sizeof(p));
    }
    if (s.config==9) {
        uint32_t p[32]={0x500d,(199u<<16)|120,0};
        for (unsigned ac=0;ac<4;ac++) {
            const uint8_t *v=s.bss.wmm_ac+4*ac;uint32_t *q=p+3+7*ac;
            q[0]=(199u<<16)|24;
            q[1]=s.ht ? (1u<<(v[1]&15))-1:15;
            q[2]=s.ht ? (1u<<(v[1]>>4))-1:1023;
            q[3]=s.ht ? v[0]&15:3;q[4]=s.ht ? le16(v+2):0;q[5]=s.ht ? !!(v[0]&16):0;
        }
        return wcn_htc_wmi_send(p,sizeof(p));
    }
    const unsigned params[][2]={{48,1},{91,2},{34,s.nss},{1,2347},{2,2346},{18,10},{19,20},{20,s.ht},{28,!!(le16(s.ht_ie)&(32|64))}};
    unsigned i=s.config;
    uint32_t p[]={0x5008,(95u<<16)|12,0,params[i][0],params[i][1]};
    return wcn_htc_wmi_send(p,sizeof(p));
}
static int configure_rx_decap(void) {
    /* WCN6855 keeps the resource-wide default at Native Wi-Fi.  Select the
     * Ethernet-II format only for this station vdev, matching the RX parser. */
    uint32_t p[]={0x5008,(95u<<16)|12,0,WMI_VDEV_PARAM_RX_DECAP_TYPE,RX_DECAP_ETHERNET2};
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
/* The firmware keeps its own copy of the BA window per peer/TID; it has to be
 * told again whenever the REO queue's window changes (ath11k
 * ath11k_peer_rx_tid_setup / ath11k_dp_rx_ampdu_stop). */
static int reorder_queue(unsigned tid,unsigned window) {
    struct { uint32_t command; struct wmi_peer_reorder_queue_setup_cmd cmd; } p;
    memset(&p,0,sizeof(p)); p.command=0x6013; p.cmd.tlv_header=tlv(549,sizeof(p.cmd)-4);
    memcpy(p.cmd.peer_macaddr.addr,s.bss.bssid,6);
    uint64_t pa=s.pa+QUEUE_BASE+tid*512;
    p.cmd.tid=p.cmd.queue_no=tid; p.cmd.queue_ptr_lo=pa; p.cmd.queue_ptr_hi=pa>>32;
    p.cmd.ba_window_size_valid=1; p.cmd.ba_window_size=window;
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
    return reorder_queue(tid,1);
}
static int vdev_start(void) {
    struct { uint32_t command; struct wmi_vdev_start_request_cmd cmd; struct wmi_channel ch; uint32_t noa; } p;
    memset(&p,0,sizeof(p)); p.command=0x5003; p.cmd.tlv_header=tlv(88,sizeof(p.cmd)-4);
    p.cmd.beacon_interval=s.bss.beacon_interval; p.cmd.dtim_period=1;
    p.cmd.preferred_tx_streams=p.cmd.preferred_rx_streams=s.nss;
    if (s.he || (s.vht && (le32(s.vht_ie)&16))) p.cmd.flags|=8;
    if (s.he) p.cmd.he_ops=le32(s.bss.he_op);
    p.ch.tlv_header=tlv(80,sizeof(p.ch)-4); p.ch.mhz=s.bss.mhz;p.ch.band_center_freq1=s.center;
    p.ch.info=s.phymode|(s.ht ? 1u<<11:0)|(s.vht ? 1u<<12:0)|(s.he ? 1u<<17:0);
    if (s.width==40 && s.center>s.bss.mhz) p.ch.info|=1u<<6;
    p.ch.reg_info_1=(s.power<<8)|(s.power<<16); p.ch.reg_info_2=s.power<<8;
    p.noa=tlv(18,0);
    return wcn_htc_wmi_send(&p,sizeof(p));
}
static int management_send(unsigned slot,unsigned len) {
    uint8_t *f=s.dma+slot*FRAME_BYTES;
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
        len+=phy_ies(f+len);
    }
    return management_send(slot,len);
}

static int peer_assoc(void) {
    uint8_t packet[512]; memset(packet,0,sizeof(packet));
    uint32_t command=0x6005; memcpy(packet,&command,4);
    struct wmi_peer_assoc_complete_cmd *p=(void *)(packet+4);
    p->tlv_header=tlv(101,sizeof(*p)-4); memcpy(p->peer_macaddr.addr,s.bss.bssid,6);
    p->peer_new_assoc=1; p->peer_associd=s.aid; p->peer_flags=4;
    p->peer_caps=s.bss.capability; p->peer_listen_intval=10; p->peer_nss=1;
    p->peer_phymode=s.phymode;p->num_peer_legacy_rates=s.rates_len;
    p->peer_nss=s.nss;
    if (s.ht) {
        p->peer_flags|=2|0x1000|(s.width>=40 ? 0x2000:0)|(s.vht ? 0x02000000:0)|(s.width==80 ? 0x04000000:0)|(s.width==160 ? 0x40000000:0);
        if (s.width==160) p->peer_bw_rxnss_override=0x80000000u|(s.nss-1);
        p->peer_ht_caps=le16(s.bss.ht_cap);
        if (s.width==20) p->peer_ht_caps&=~(2|64);
        p->peer_max_mpdu=(1u<<(13+(s.bss.ht_cap[2]&3)))-1;
        static const unsigned density[]={0,1,1,1,2,4,8,16};
        p->peer_mpdu_density=density[(s.bss.ht_cap[2]>>2)&7];
        p->peer_rate_caps=8|(s.nss>1 ? 1:0)|(s.width>=40 ? 2:0)|(le16(s.ht_ie)&(32|64) ? 4:0);
        p->num_peer_ht_rates=s.ht_rates_len;
        if (s.vht) {
            p->peer_vht_caps=le32(s.bss.vht_cap);
            unsigned max=(1u<<(13+((p->peer_vht_caps>>23)&7)))-1;
            if (max>p->peer_max_mpdu) p->peer_max_mpdu=max;
        }
    }
    if (s.he) {
        p->peer_flags|=0x400|(s.bss.he.phy[1]&32 ? 0x10000:0);
        p->peer_he_cap_info=le32(s.bss.he.mac);
        p->peer_he_cap_info_ext=le16(s.bss.he.mac+4);
        p->peer_he_ops=le32(s.bss.he_op)&0xffffff;
        memcpy(p->peer_he_cap_phy,s.bss.he.phy,11);
        p->peer_he_mcs=s.width==160 ? 2:1;
        unsigned exp=(s.bss.he.mac[3]>>3)&3;
        if (exp) p->peer_max_mpdu=(1u<<(20+exp))-1;
        if (s.bss.he.phy[6]&128) {
            const uint8_t *ppe=s.bss.he.ppe;unsigned bit=7,ru=(ppe[0]>>3)&15;
            p->peer_ppet.numss_m1=ppe[0]&7;p->peer_ppet.ru_mask=ru;
            for (unsigned ss=0;ss<=p->peer_ppet.numss_m1;ss++) for (unsigned r=0;r<4;r++) if (ru&(1u<<r)) {
                unsigned v=0;
                for (unsigned j=0;j<6;j++,bit++) v=(v>>1)|(((ppe[bit/8]>>(bit%8))&1)<<5);
                p->peer_ppet.ppet16_ppet8_ru3_ru0[ss]|=v<<(6*r);
            }
        }
    }
    unsigned off=4+sizeof(*p),n=(s.rates_len+3)&~3u,v=tlv(17,n);
    memcpy(packet+off,&v,4); off+=4;
    for (unsigned i=0;i<s.rates_len;i++) packet[off+i]=s.rates[i]&127;
    off+=n;n=(s.ht_rates_len+3)&~3u;v=tlv(17,n);memcpy(packet+off,&v,4);off+=4;
    memcpy(packet+off,s.ht_rates,s.ht_rates_len);off+=n;
    struct wmi_vht_rate_set *vht=(void *)(packet+off); vht->tlv_header=tlv(102,sizeof(*vht)-4);
    if (s.vht) { vht->rx_mcs_set=le16(s.vht_ie+8);vht->tx_mcs_set=le16(s.vht_ie+4); }
    off+=sizeof(*vht);
    unsigned count=s.he ? (s.width==160 ? 2:1):0;
    v=tlv(18,count*12); memcpy(packet+off,&v,4); off+=4;
    for (unsigned i=0;i<count;i++) {
        /* HE rate-set fields describe the AP's RX/TX capabilities. */
        uint32_t rate[]={tlv(645,8),le16(s.he_ie+19+4*i),le16(s.he_ie+17+4*i)};
        memcpy(packet+off,rate,sizeof(rate));off+=sizeof(rate);
    }
    return wcn_htc_wmi_send(packet,off);
}
static void ba_poll(void) {
    if (!s.ba_state) return;
    if (kernel_tic_ms(0)>=s.ba_deadline) { fail("reorder-timeout",0);return; }
    if (s.ba_state==1) {
        int id=wcn_dp_reorder(s.pa+QUEUE_BASE+s.ba_tid*512,s.ba_window,s.ba_ssn);
        if (id>=0) { s.ba_cookie=id;s.ba_state=2; }return;
    }
    if (s.ba_state==3) {
        if (!s.ba_status && reorder_queue(s.ba_tid,s.ba_window)) return;
        if (s.ba_delete) { s.ba_delete=0;s.ba_state=0;return; }
        s.ba_state=4;
    }
    if (s.ba_state!=4) return;
    unsigned slot;for (slot=0;slot<FRAME_SLOTS;slot++) if (!s.pending[slot]) break;
    if (slot==FRAME_SLOTS) return;
    uint8_t *f=s.dma+slot*FRAME_BYTES;memset(f,0,FRAME_BYTES);f[0]=0xd0;
    memcpy(f+4,s.bss.bssid,6);memcpy(f+10,s.mac,6);memcpy(f+16,s.bss.bssid,6);
    put16(f+22,(++s.seq&4095)<<4);f[24]=3;f[25]=1;f[26]=s.ba_token;
    put16(f+27,s.ba_status);put16(f+29,2|(s.ba_tid<<2)|(s.ba_window<<6));put16(f+31,s.ba_timeout);
    /* No A-MSDU in A-MPDU until the complete aggregate path is validated. */
    if (!management_send(slot,33)) { if (!s.ba_status) s.ba_accepted++;s.ba_state=0; }
}
void wcn_sta_reorder_complete(unsigned cookie,unsigned status) {
    if (s.ba_state!=2 || cookie!=s.ba_cookie) return;
    s.ba_status=status ? 37:0;s.ba_state=3;
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
        /* RX liveness. The short first-frame grace judges the bring-up (the
         * reference AP lands its first frame at +50 ms). The old check stopped
         * monitoring once any frame had arrived, so a link that died silently
         * later stayed "connected" forever with no recovery. Now the window
         * re-arms on every RX completion or ACKed uplink frame, and on a
         * silent link a QoS-null probe keeps the AP's station entry warm (its
         * ACK also proves the radio path), so a quiet network is never torn
         * down while a truly dead one is recovered within the window. */
        uint64_t now=kernel_tic_ms(0);
        unsigned frames=wcn_dp_rx_frames();
        if (frames!=s.rx_base) {
            s.rx_base=frames;
            s.rx_grace=now+(frames ? 60000:1200);
        }
        /* An ACKed uplink frame proves the radio path just as well as RX. */
        unsigned done=wcn_dp_tx_done();
        if (done!=s.tx_done_base) {
            s.tx_done_base=done;
            if (frames) s.rx_grace=now+60000;
        }
        if (now>=s.rx_grace) { fail("rx-dead",0); return; }
        /* A wake-policy command lost to credit exhaustion must still land:
         * retry every 5 s until all four are applied. */
        if (wcn_wpa_established() && s.ps_done!=15u && now>=s.ps_retry) {
            s.ps_retry=now+5000;
            sta_ps_configure();
        }
        if (frames && wcn_wpa_established() && now>=s.rx_grace-30000 && now>=s.probe_ms) {
            s.probe_ms=now+10000;
            uint8_t f[26]={0xc8,0x41}; /* QoS-null (subtype 12), ToDS, protected */
            memcpy(f+4,s.bss.bssid,6);memcpy(f+10,s.mac,6);memcpy(f+16,s.bss.bssid,6);
            put16(f+22,(++s.seq&4095)<<4);
            wcn_dp_transmit(f,sizeof(f),s.lmac);
        }
        ba_poll();
        if (s.state==FAILED) return;
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
    case CONFIG: {
        unsigned want=s.he ? 11u:10u;
        if (s.config<want && !configure()) s.config++;
        /* A -1 here is almost always WMI credit exhaustion: the CONFIG
         * burst plus the idle-ps config drains the endpoint faster than
         * the firmware's trailers refill it. Retry in place like
         * configure() does - failing outright restarts the controller,
         * which replays the same burst and loops forever. The state
         * deadline still condemns a truly stuck send. */
        if (s.config==want && !configure_rx_decap()) { sta_ps_configure(); state(PEER); }
        break;
    }
    case PEER: if (!peer_create()) state(PEER_WAIT); break;
    case PEER_WAIT: if (s.peer_mapped) state(ROUTE); break;
    case ROUTE: if (!route()) state(QUEUES); break;
    case QUEUES: if (!queue_setup() && ++s.queue==17) state(START); break;
    case START: if (!vdev_start()) state(START_WAIT); break;
    case AUTH: if (!management(0)) state(AUTH_WAIT); break;
    case ASSOC: if (!management(1)) state(ASSOC_WAIT); break;
    case PEER_ASSOC: if (!peer_assoc()) state(UP); break;
    case UP:
        if (!vdev_up()) {
            /* Give the AP a real chance to send M1: the working reference AP
             * delivers its first frame ~50 ms after vdev-start, but an AP that
             * waits for its own retry timer can take several hundred ms. Judging
             * the RX path at +20 ms failed connections that were about to
             * succeed, so an attempt is only declared dead after a grace
             * period with no new completion at all. */
            s.rx_base = wcn_dp_rx_frames();
            s.rx_grace = kernel_tic_ms(0) + 1200;
            state(ASSOCIATED);
        }
        break;
    default: break;
    }
}
void wcn_sta_event(unsigned event,unsigned tag,const uint8_t *p,unsigned bytes) {
    if (s.state==ASSOCIATED && event==0x6001 && tag==43 && bytes>=8 && !memcmp(p,s.bss.bssid,6))
        fail("peer-kickout",0);
    if (s.state==ASSOCIATED && event==0xb001 && tag==48 && bytes>=12 && !le32(p) && le32(p+4)==2)
        wcn_log("wcn: firmware roam BEACON_MISS report; keeping the link\n");
    if (event==0x5003 && tag==42 && bytes>=24 && !le32(p) && !memcmp(p+4,s.bss.bssid,6))
        wcn_wpa_key_complete(le32(p+12),le32(p+16),le32(p+20));
    if (event==0x7006 && tag==423 && bytes>=20) {
        unsigned id=le32(p);
        for (unsigned i=0;i<FRAME_SLOTS;i++) if (id && s.pending[i]==id) {
            if (le32(p+4))
                wcn_log("wcn: mgmt TX failed subtype=%u desc=%u status=%u\n",
                        s.dma[i*FRAME_BYTES]>>4,id,le32(p+4));
            s.pending[i]=0; s.tx_done++; if (le32(p+4)) s.tx_errors++;
        }
    }
    if (s.state==START_WAIT && event==0x5001 && tag==40 && bytes>=40 && !le32(p)) {
        /* WCN6855 is single_pdev_only in ath11k: both bands share host
         * pdev index 0, which maps to TCL LMAC 0. The descriptor's LMAC
         * selects the AST lookup namespace, not the physical channel/radio.
         * The VDEV response union is a target pdev ID with use_pdev_id=1;
         * neither that value nor a band-to-radio mapping is a TCL selector. */
        s.lmac=0;
        wcn_diag("vdev-start status=%u target_mac_pdev=%u tx_streams=%u rx_streams=%u\n",
                le32(p+12),le32(p+24),le32(p+28),le32(p+32));
        if (le32(p+12)) fail("vdev-start-rejected",le32(p+12));
        else state(AUTH);
    }
}
void wcn_sta_peer(unsigned vdev,const uint8_t *mac,unsigned peer,unsigned hw_peer) {
    if ((s.state==PEER || s.state==PEER_WAIT) && !vdev && !memcmp(mac,s.bss.bssid,6)) {
        s.peer_mapped=1; s.peer=peer; s.hw_peer=hw_peer;
    }
}
void wcn_sta_management(const uint8_t *p,unsigned bytes,unsigned mhz) {
    if (s.state==IDLE || s.state==FAILED || bytes<24 ||
        (p[0]&15) || memcmp(p+4,s.mac,6) || memcmp(p+10,s.bss.bssid,6) || memcmp(p+16,s.bss.bssid,6)) return;
    /* Record relevant responses even if the firmware reports an unexpected
     * receive frequency, then retain the normal channel validation below. */
    unsigned subtype=p[0]>>4;
    if (mhz!=s.bss.mhz) return;
    s.rx++;
    if ((subtype==12 || subtype==10) && bytes>=26) { fail("AP-disconnected",le16(p+24)); return; }
    if (s.state==ASSOCIATED && s.ht && subtype==13 && bytes>=33 && p[24]==3 && p[25]==0 && !s.ba_state) {
        unsigned param=le16(p+27),window=param>>6;
        unsigned maximum=s.he ? 256:64;
        s.ba_delete=0;s.ba_tid=(param>>2)&15;s.ba_token=p[26];
        s.ba_window=window && window<maximum ? window:maximum;
        s.ba_ssn=le16(p+31)>>4;s.ba_timeout=le16(p+29);s.ba_status=0;s.ba_requests++;
        s.ba_state=(param&2) ? 1:4;if (!(param&2)) s.ba_status=37;
        s.ba_deadline=kernel_tic_ms(0)+2000;
    }
    if (s.state==ASSOCIATED && s.ht && subtype==13 && bytes>=30 && p[24]==3 && p[25]==2 && !s.ba_state) {
        unsigned param=le16(p+26);
        if (param&2048) { /* AP is the originator of this RX BA session. */
            s.ba_delete=1;s.ba_tid=param>>12;s.ba_window=1;s.ba_ssn=4096;s.ba_state=1;
            s.ba_deadline=kernel_tic_ms(0)+2000;
        }
    }
    if (s.state==AUTH_WAIT && subtype==11 && bytes>=30 && !le16(p+24) && le16(p+26)==2) {
        if (le16(p+28)) fail("authentication-rejected",le16(p+28)); else state(ASSOC);
    }
    if (s.state==ASSOC_WAIT && subtype==1 && bytes>=30) {
        if (le16(p+26)) { fail("association-rejected",le16(p+26)); return; }
        s.aid=le16(p+28)&0x3fff;
        if (!s.aid || s.aid>2007) { fail("invalid-association-id",s.aid); return; }
        if (s.he) {
            int he=wcn_wmi_parse_he(p+30,bytes-30,&s.bss.he,s.bss.he_op);
            if (he<0) { fail("invalid-HE-response",0);return; }
            s.bss.he_operation=he;
            s.phymode=s.width==160 ? 15:s.width==80 ? 10:s.width==40 ? 9:8;
            select_he(); /* The AP may decline HE and accept the VHT fallback. */
        }
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
    } else if (security->valid && security->encrypted &&
               wcn_wpa_accept_msdu(security->group,security->key,security->tid,security->pn,security->amsdu,security->first,security->last,security->sequence,security->number))
        wcn_net_receive(frame,bytes);
}
void wcn_sta_release(void) {
    if (s.dma) dma_free(0,(ewokos_addr_t)s.dma);
    wcn_wpa_reset();
    wcn_net_reset();
    memset(&s,0,sizeof(s));
}
void wcn_sta_status(char *out,size_t size) {
    snprintf(out,size,"station=%s error=%s status=%u mhz=%u peer=%u mapped=%u queues=%u aid=%u tx=%u done=%u tx_errors=%u rx=%u lmac=%u data_rx=%u eapol_rx=%u data_tx=%u network_ready=%u phy=%s width=%u nss=%u center=%u ba=%u/%u ba_window=%u power=%u max_width=%u\n",
             wcn_sta_link_ready()?"connected":names[s.state],s.error?s.error:"none",s.status,s.bss.mhz,s.peer,s.peer_mapped,s.queue,s.aid,s.tx,s.tx_done,s.tx_errors,s.rx,s.lmac,s.data_rx,s.eapol_rx,s.data_tx,wcn_sta_link_ready(),s.he?"HE":s.vht?"VHT":s.ht?"HT":"legacy",s.width,s.nss,s.center,s.ba_accepted,s.ba_requests,s.ba_window,s.power,s.max_width);
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
/* Station power save. The WCN firmware parks itself after a few idle seconds
 * even with PS disabled, and the AP then holds downlink frames for a station
 * it thinks is asleep, so isolated traffic (ssh, ping) goes missing while a
 * busy stream keeps the radio awake. Explicit WMI PS mode=1 makes the protocol
 * proper (station wakes at DTIM, AP buffers + station polls), which is what
 * powerd's sleep path uses to stay connected while saving power; wake sends
 * mode=0 back to the always-awake config. Tags/command ids follow ath11k:
 * WMI_GRP_STA_PS commands 0x9001/0x9002, TLV tags 108/109 (matching the
 * current Linux wmi_tlv_tag enum). */
static int sta_ps_mode(int mode) {
    struct { uint32_t command; struct wmi_sta_powersave_mode_cmd cmd; } p;
    memset(&p,0,sizeof(p)); p.command=0x9001; p.cmd.tlv_header=tlv(108,sizeof(p.cmd)-4);
    p.cmd.vdev_id=0; p.cmd.sta_ps_mode=mode; /* 0=DISABLED 1=ENABLED */
    int ret=wcn_htc_wmi_send(&p,sizeof(p));
    if (ret) wcn_diag("sta-ps mode=%d failed ret=%d\n",mode,ret);
    return ret;
}
static int sta_ps_param(unsigned param,unsigned value) {
    struct { uint32_t command; struct wmi_sta_powersave_param_cmd cmd; } p;
    memset(&p,0,sizeof(p)); p.command=0x9002; p.cmd.tlv_header=tlv(109,sizeof(p.cmd)-4);
    p.cmd.vdev_id=0; p.cmd.param=param; p.cmd.value=value;
    return wcn_htc_wmi_send(&p,sizeof(p));
}
/* ath11k configures the station wake policy at vdev-create time, before any
 * association (mac.c, WMI_VDEV_TYPE_STA case): RX_WAKE_POLICY=WAKE, TX_WAKE_
 * THRESHOLD=ALWAYS, PSPOLL_COUNT=NO_MAX, then PS mode disabled. Sending only
 * mode=0 after WPA left the firmware wake policy at its defaults: downlink
 * unicast died a few seconds after the last traffic (AP buffering for an
 * apparently sleeping station) while DTIM multicast kept flowing. Best-effort:
 * a rejected param must not block association. Each command is independent
 * and remembered in s.ps_done: the CONFIG burst sends ~12 WMI commands with
 * no RX in between, which exhausts the endpoint credits, so the first param
 * send failed with -1 on every boot while the later authorize-time mode=0
 * (credits replenished by EAPOL/events) succeeded. Whatever failed here is
 * retried by wcn_sta_authorize() and the ASSOCIATED poll until applied. */
static void sta_ps_configure(void) {
    int r0=(s.ps_done&1)?0:sta_ps_param(0,0); /* RX_WAKE_POLICY = WAKE */
    if (!r0) s.ps_done|=1;
    int r1=(s.ps_done&2)?0:sta_ps_param(1,1); /* TX_WAKE_THRESHOLD = ALWAYS */
    if (!r1) s.ps_done|=2;
    int r2=(s.ps_done&4)?0:sta_ps_param(2,0); /* PSPOLL_COUNT = NO_MAX */
    if (!r2) s.ps_done|=4;
    int r3=(s.ps_done&8)?0:sta_ps_mode(0); /* PS_MODE_DISABLED */
    if (!r3) s.ps_done|=8;
    wcn_diag("sta-ps configure done=%x ret=%d/%d/%d/%d credits=%u\n",
             s.ps_done,r0,r1,r2,r3,wcn_htc_wmi_credits());
}
/* powerd sleep/wake hook: enable PS on sleep, disable on wake. A no-op success
 * when not associated (nothing to save); the link-watch ladder re-authorizes
 * (mode=0) on any reconnect, so a dropped link recovers awake. */
int wcn_sta_ps_set(int enable) {
    if (s.state!=ASSOCIATED) return 0;
    int ret=sta_ps_mode(enable?1:0);
    wcn_diag("sta-ps mode=%s ret=%d\n",enable?"enabled":"disabled",ret);
    return ret;
}
int wcn_sta_authorize(void) {
    struct { uint32_t command; struct wmi_peer_set_param_cmd cmd; } p;
    memset(&p,0,sizeof(p));p.command=0x6004;p.cmd.tlv_header=tlv(100,sizeof(p.cmd)-4);
    memcpy(p.cmd.peer_macaddr.addr,s.bss.bssid,6);p.cmd.param_id=3;p.cmd.param_value=1;
    int ret=wcn_htc_wmi_send(&p,sizeof(p));
    if (!ret) {
        /* WPA/EAPOL traffic replenished the WMI credits: apply whatever the
         * CONFIG-phase burst could not send, and re-assert the awake mode. */
        s.ps_done&=~8u;
        sta_ps_configure();
        sta_ps_param(4,0); /* WMI_STA_PS_PARAM_UAPSD: no wakeup scheduling */
    }
    return ret;
}
/* Join a network with either a passphrase (PBKDF2) or a precomputed PMK
 * ("pmk" in network.json / a 64-hex credential). The caller must have a
 * fresh scan cache so wcn_sta_associate() can resolve the BSS. */
int wcn_sta_join(const char *ssid,const char *credential) {
    if (ssid == NULL || ssid[0] == 0 || strlen(ssid) > 32 || credential == NULL) return -1;
    if (s.state!=IDLE || !wcn_wmi_station_available() || !wcn_dp_ready()) return -1;
    unsigned n=strlen(credential);
    int pmk=(n==64 && wcn_hex_is(credential,64));
    if (!pmk && (n<8 || n>63)) return -1;
    if (wcn_sta_associate(ssid)) return -1;
    uint8_t seed[32],key[32];
    wcn_random_bytes(seed,sizeof(seed));
    int ret;
    if (pmk) {
        if (wcn_hex_decode(credential,key,32)) { fail("WPA-initialization",0);return -1; }
        ret=wcn_wpa_init_pmk(ssid,key,seed,s.mac,s.bss.bssid,s.bss.rsn,s.bss.rsn_len,s.rsn);
        memset(key,0,sizeof(key));
    } else {
        ret=wcn_wpa_init(ssid,credential,seed,s.mac,s.bss.bssid,s.bss.rsn,s.bss.rsn_len,s.rsn);
    }
    memset(seed,0,sizeof(seed));
    if (ret) { fail("WPA-initialization",0);return -1; }
    return 0;
}

/* True when this BSS can actually be joined: protected and carrying the
 * WPA2-PSK/CCMP RSN this supplicant implements. Used to skip configured
 * networks that would only fail after a full scan/associate cycle. */
int wcn_sta_bss_supported(const struct wcn_bss *bss) {
    return bss != NULL && bss->privacy &&
        !select_rsn(bss->rsn,bss->rsn_len) &&
        wcn_wmi_channel_power(bss->mhz) >= 0;
}

/* Associated BSS, or NULL when the station is not associated (the struct is
 * cleared by wcn_sta_release()). */
const struct wcn_bss *wcn_sta_bss(void) {
    return s.bss.ssid[0] ? &s.bss : NULL;
}
const char *wcn_sta_ssid(void) {
    return s.bss.ssid;
}
const char *wcn_sta_error(void) {
    return s.error ? s.error : "none";
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
