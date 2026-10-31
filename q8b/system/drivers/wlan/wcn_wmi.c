/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/* WMI control protocol reference: Linux ath11k/wmi.c and hw.c.
 * Copyright (c) 2018-2019 The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. */
#include "wcn_wmi.h"
#include "wcn_wmi_proto.h"
#include "wcn_htc.h"
#include "wcn_dp.h"
#include "wcn_sta.h"
#include <ewoksys/klog.h>
#include <ewoksys/kernel_tic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static struct {
    unsigned service_ready, ext_ready, ext2_ready, init_sent, ready, failed, hw_mode_sent;
    unsigned chains, vht5, nss160, events, bad, memory_requests;
    struct wcn_he_caps he5;
    unsigned vdev_sent, channels_sent, scan_pending, scan_active, scan_done;
    unsigned scan_id, scan_reason, scan_events, management_rx, ap_count;
    unsigned scan_cancelling, scan_timedout,country_state,country_seen,country_mask,country_sent;
    uint64_t country_deadline;
    uint8_t mac[6];
    uint64_t deadline;
    uint64_t scan_deadline;
    struct wcn_bss ap[64];
    struct {
        unsigned count, valid, max_bw5;
        struct { unsigned start,end,power,flags,bw; } rule[32];
    } reg[2];
} f;
/* Explicit local regulatory setting survives controller recovery. Default 00
 * remains unchanged unless a country is provisioned or requested by root. */
static char country[3];
static int country_valid(const char *p) { return p && p[0]>='A' && p[0]<='Z' && p[1]>='A' && p[1]<='Z' && !p[2]; }
static int country_load(void) {
    FILE *fp=fopen("/etc/wlan.country","r");if (!fp) return 0;
    char value[5]={0};unsigned n=fread(value,1,4,fp);int extra=fgetc(fp);fclose(fp);
    if (extra!=EOF || (n!=2 && (n!=3 || value[2]!='\n'))) return -1;
    value[2]=0;if (!country_valid(value)) return -1;
    memcpy(country,value,3);return 0;
}
int wcn_wmi_country(const char *alpha2) {
    if (!country_valid(alpha2) || !f.ready || f.failed || wcn_sta_busy() || f.scan_pending || f.scan_active || f.scan_cancelling || f.country_state) return -1;
    memcpy(country,alpha2,3);f.country_state=1;return 0;
}
static const unsigned scan_freqs[] = {
    2412,2417,2422,2427,2432,2437,2442,2447,2452,2457,2462,
    5180,5200,5220,5240
};
#define SCAN_CHANNELS (sizeof(scan_freqs)/sizeof(scan_freqs[0]))
static unsigned get16(const uint8_t *p) { return p[0] | ((unsigned)p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | ((uint32_t)get16(p+2) << 16); }
static uint32_t tlv(unsigned tag, unsigned bytes) { return tag << 16 | bytes; }

/* HE extension bodies exclude the extension ID. Validate variable MCS maps
 * and PPE bits before exposing any capability to association. */
static int he_capability(struct wcn_he_caps *cap,const uint8_t *p,unsigned n) {
    memset(cap,0,sizeof(*cap));
    if (n<21) return -1;
    unsigned maps=4+((p[6]&8) ? 4:0)+((p[6]&16) ? 4:0),off=17+maps,ppe=0;
    if (n<off) return -1;
    if (p[12]&128) {
        if (n==off) return -1;
        unsigned ru=(p[off]>>3)&15;
        if (!ru) return -1;
        ppe=(7+6*((p[off]&7)+1)*__builtin_popcount(ru)+7)/8;
        if (n-off<ppe) return -1;
    }
    memcpy(cap->mac,p,6);memcpy(cap->phy,p+6,11);
    memset(cap->mcs,255,sizeof(cap->mcs));memcpy(cap->mcs,p+17,4);
    unsigned pos=21;
    if (p[6]&8) { memcpy(cap->mcs+4,p+pos,4);pos+=4; }
    if (p[6]&16) memcpy(cap->mcs+8,p+pos,4);
    if (ppe) memcpy(cap->ppe,p+off,ppe);
    cap->present=1;return 0;
}
static int he_operation(const uint8_t *p,unsigned n) {
    if (n<6) return -1;
    unsigned flags=get32(p)&0xffffff,need=6;
    if (flags&(1u<<14)) need+=3;
    if (flags&(1u<<15)) need++;
    if (flags&(1u<<17)) need+=5;
    return n>=need ? 0:-1;
}
/* Return 1 only for a complete HE negotiation; 0 means absent and -1 invalid. */
int wcn_wmi_parse_he(const uint8_t *ies,unsigned bytes,struct wcn_he_caps *cap,uint8_t operation[6]) {
    unsigned seen=0;
    memset(cap,0,sizeof(*cap));memset(operation,0,6);
    for (unsigned pos=0;pos<bytes;) {
        if (bytes-pos<2 || ies[pos+1]>bytes-pos-2) goto invalid;
        unsigned len=ies[pos+1];const uint8_t *p=ies+pos+2;
        if (ies[pos]==255 && len && p[0]==35) {
            if ((seen&1) || he_capability(cap,p+1,len-1)) goto invalid;
            seen|=1;
        }
        if (ies[pos]==255 && len && p[0]==36) {
            if ((seen&2) || he_operation(p+1,len-1)) goto invalid;
            memcpy(operation,p+1,6);seen|=2;
        }
        pos+=2+len;
    }
    return seen==3;
invalid:
    memset(cap,0,sizeof(*cap));memset(operation,0,6);return -1;
}

/* Optional persistent station address, provisioned outside the driver image.
 * Some modules report a generated firmware fallback address on every start. */
static int station_mac(uint8_t mac[6]) {
    FILE *fp=fopen("/etc/wlan.mac","r");
    if (!fp) return 0;
    char text[20]={0};unsigned n=fread(text,1,sizeof(text)-1,fp);
    int extra=fgetc(fp);fclose(fp);
    if (extra!=EOF || (n!=17 && (n!=18 || text[17]!='\n'))) return -1;
    uint8_t value[6];unsigned any=0;
    for (unsigned i=0;i<6;i++) {
        unsigned v=0;
        for (unsigned j=0;j<2;j++) {
            char c=text[i*3+j];unsigned x;
            if (c>='0' && c<='9') x=c-'0';else if (c>='a' && c<='f') x=c-'a'+10;
            else if (c>='A' && c<='F') x=c-'A'+10;else return -1;
            v=v*16+x;
        }
        if (i<5 && text[i*3+2]!=':') return -1;
        value[i]=v;any|=v;
    }
    if (!any || (value[0]&1)) return -1;
    memcpy(mac,value,6);return 0;
}

static void receive_management(const uint8_t *frame, unsigned length, unsigned mhz, int rssi) {
    if (length < 36 || (frame[0] & 0x0c) || ((frame[0] >> 4) != 8 && (frame[0] >> 4) != 5)) return;
    unsigned pos = 36, ssid_len = 0;
    const uint8_t *ssid = NULL;
    while (pos < length) {
        if (length - pos < 2 || frame[pos+1] > length - pos - 2) return;
        unsigned len = frame[pos+1];
        if (!frame[pos] && !ssid) { if (len > 32) return; ssid=frame+pos+2; ssid_len=len; }
        pos += 2+len;
    }
    if (!ssid) return;
    unsigned index;
    for (index=0; index<f.ap_count; index++) if (!memcmp(f.ap[index].bssid,frame+16,6)) break;
    if (index==f.ap_count) {
        if (index==64) return;
        f.ap_count++; memcpy(f.ap[index].bssid,frame+16,6);
    }
    f.ap[index].mhz=mhz; f.ap[index].rssi=rssi; f.ap[index].privacy=!!(frame[34]&0x10);
    if (ssid_len) {
        for (unsigned i=0;i<ssid_len;i++) f.ap[index].ssid[i]=ssid[i]>=32 && ssid[i]<127 ? ssid[i] : '.';
        f.ap[index].ssid[ssid_len]=0;
    }
    struct wcn_bss *b=&f.ap[index];
    b->capability=get16(frame+34); b->beacon_interval=get16(frame+32);
    b->rsn_len=b->rates_len=b->ht=b->vht=b->wmm=0;
    b->he_operation=wcn_wmi_parse_he(frame+36,length-36,&b->he,b->he_op)==1;
    memset(b->ht_op,0,sizeof(b->ht_op));memset(b->vht_op,0,sizeof(b->vht_op));
    for (pos=36;pos<length;pos+=2+frame[pos+1]) {
        unsigned len=frame[pos+1], id=frame[pos];
        if (id==45 && len==26) { memcpy(b->ht_cap,frame+pos+2,26);b->ht=1; }
        if (id==61 && len==22) memcpy(b->ht_op,frame+pos+2,22);
        if (id==191 && len==12) { memcpy(b->vht_cap,frame+pos+2,12);b->vht=1; }
        if (id==192 && len==5) memcpy(b->vht_op,frame+pos+2,5);
        if (id==221 && len==24 && !memcmp(frame+pos+2,"\x00\x50\xf2\x02\x01\x01",6)) {
            unsigned seen=0;
            for (unsigned ac=0;ac<4;ac++) {
                const uint8_t *a=frame+pos+10+4*ac;unsigned n=(a[0]>>5)&3;
                if ((seen&(1u<<n)) || !(a[0]&15) || (a[1]&15)>(a[1]>>4)) break;
                seen|=1u<<n;memcpy(b->wmm_ac+4*n,a,4);
            }
            b->wmm=seen==15;
        }
        if (id==48) { b->rsn_len=len+2; memcpy(b->rsn,frame+pos,len+2); }
        if ((id==1 || id==50) && len<=sizeof(b->rates)-b->rates_len) {
            memcpy(b->rates+b->rates_len,frame+pos+2,len); b->rates_len+=len;
        }
    }
}

void wcn_wmi_receive(const void *packet, unsigned bytes) {
    const uint8_t *p = packet;
    if (bytes < 4) { f.bad++; return; }
    unsigned event = get32(p) & 0xffffff;
    f.events++;
    /* Validate the entire TLV stream before publishing any state. */
    unsigned pos = 4;
    while (pos < bytes) {
        if (bytes - pos < 4 || get16(p+pos) > bytes - pos - 4) { f.bad++; f.failed=1; return; }
        if (event==0x4009 && get16(p+pos+2)==18) {
            unsigned end=pos+4+get16(p+pos);
            for (unsigned off=pos+4;off<end;) {
                if (end-off<4 || get16(p+off)>end-off-4) { f.bad++;f.failed=1;return; }
                off+=4+get16(p+off);
            }
        }
        pos += 4 + get16(p+pos);
    }
    const uint8_t *mgmt = NULL, *frame = NULL;
    const uint8_t *reg = NULL, *rules = NULL;
    unsigned rules_len=0;
    unsigned frame_len=0, mgmt_len=0;
    for (pos = 4; pos < bytes;) {
        unsigned len = get16(p+pos), tag = get16(p+pos+2);
        const uint8_t *v = p + pos + 4;
        if (event == 1 && tag == 32 && len >= 76) {
            f.chains = get32(v+36); f.memory_requests = get32(v+72);
            if (!f.chains || f.chains > 2 || f.memory_requests) { f.failed = 1; return; }
            f.service_ready = 1;
            klog("wcn: WMI service ready chains=%u memory_requests=%u build=%x\n", f.chains, f.memory_requests, get32(v));
        }
        if (event==0x4009 && tag==18) {
            for (unsigned off=0;off<len;) {
                unsigned length=get16(v+off),id=get16(v+off+2);const uint8_t *m=v+off+4;
                /* Capabilities for the selected DBS mode, 5 GHz on LMAC0. */
                if (id==528 && length>=236 && get32(m)==1 && get32(m+4)==1 &&
                    !get32(m+8) && (get32(m+16)&2)) {
                    f.vht5=get32(m+64);f.nss160=0;
                    memset(&f.he5,0,sizeof(f.he5));
                    memcpy(f.he5.mac,m+72,4);memcpy(f.he5.mac+4,m+204,2);
                    memcpy(f.he5.phy,m+100,11);
                    memset(f.he5.mcs,255,sizeof(f.he5.mcs));memcpy(f.he5.mcs,m+76,4);
                    if (f.he5.phy[0]&8) memcpy(f.he5.mcs+4,m+76,4);
                    f.he5.present=get32(m+72) && (f.he5.phy[0]&4) &&
                        (get16(m+76)&3)!=3 && (get16(m+78)&3)!=3;
                    if (f.he5.phy[6]&128) {
                        unsigned last=get32(m+152),ru=get32(m+156),bit=7;
                        if (last>7 || !ru || ru>15) f.he5.present=0;
                        else {
                            f.he5.ppe[0]=last|(ru<<3);
                            for (unsigned ss=0;ss<=last;ss++) for (unsigned r=0;r<4;r++) if (ru&(1u<<r)) {
                                unsigned v=(get32(m+160+ss*4)>>(6*r))&63;
                                v=((v>>3)&7)|((v&7)<<3);
                                for (int j=5;j>=0;j--,bit++) f.he5.ppe[bit/8]|=((v>>j)&1)<<(bit%8);
                            }
                        }
                    }
                    unsigned tx=__builtin_popcount(get32(m+80)),rx=__builtin_popcount(get32(m+84));
                    unsigned nss=tx<rx ? tx:rx,ratio=get32(m+232);
                    if (get32(m+56)==3 && nss && nss<=2 && (ratio&1)) {
                        if (((ratio>>1)&15)==2) f.nss160=nss;
                        else if (!((ratio>>1)&15)) f.nss160=nss/2;
                    }
                }
                off+=4+length;
            }
        }
        if (event == 0x4009 && tag == 428) f.ext_ready = 1;
        if (event == 0x4022 && tag == 820) f.ext2_ready = 1;
        if (event == 2 && tag == 35 && len >= 36) {
            unsigned status=get32(v+32);
            if (status || (v[24]&1)) { f.failed=1; return; }
            memcpy(f.mac,v+24,6);
            if (station_mac(f.mac)) { f.failed=1;return; }
            f.ready = 1;
            if (country_load()) { f.failed=1;return; }
            if (country[0]) f.country_state=1;
            klog("wcn: WMI READY MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
                 f.mac[0],f.mac[1],f.mac[2],f.mac[3],f.mac[4],f.mac[5]);
        }
        if (event==0x3001 && tag==36 && len>=24 &&
            get32(v+12)==0xa001 && get32(v+16)==(0xa000 | f.scan_id) &&
            !get32(v+20) && (f.scan_active || f.scan_cancelling)) {
            unsigned type=get32(v), reason=get32(v+4);
            f.scan_events++;
            klog("wcn: scan event=%x reason=%u frequency=%u\n",type,reason,get32(v+8));
            if (type & 1) f.scan_active=1;
            if (type & (2|0x40)) {
                f.scan_active=f.scan_cancelling=0; f.scan_done=1;
                f.scan_reason=f.scan_timedout ? 3 : reason;
            }
        }
        if (event==0x7001 && tag==44 && len>=68) { mgmt=v; mgmt_len=len; }
        if (event==0x7001 && tag==17) { frame=v; frame_len=len; }
        if (event==0x3a001 && tag==609 && len>=56) reg=v;
        if (event==0x3a001 && tag==18) { rules=v; rules_len=len; }
        wcn_sta_event(event,tag,v,len);
        pos += len + 4;
    }
    if (reg && rules && !get32(reg) && get32(reg+4)<2) {
        unsigned phy=get32(reg+4), count=get32(reg+48)+get32(reg+52);
        if (count<=32 && count==rules_len/16 && !(rules_len%16)) {
            unsigned valid=1;
            for (unsigned i=0;i<count;i++)
                if (get16(rules+i*16)!=12 || get16(rules+i*16+2)!=608) valid=0;
            if (valid) {
                f.reg[phy].count=count; f.reg[phy].valid=1;f.reg[phy].max_bw5=get32(reg+44);
                if (f.country_state==2 && reg[8]==(uint8_t)country[0] && reg[9]==(uint8_t)country[1]) f.country_seen|=1u<<phy;
                for (unsigned i=0;i<count;i++) {
                    const uint8_t *v=rules+i*16+4;
                    f.reg[phy].rule[i].start=get16(v);
                    f.reg[phy].rule[i].end=get16(v+2);
                    f.reg[phy].rule[i].power=v[6];
                    f.reg[phy].rule[i].bw=get16(v+4);
                    f.reg[phy].rule[i].flags=get16(v+8);
                    klog("wcn: reg phy=%u %u-%u MHz power=%u flags=%x\n",phy,
                         get16(v),get16(v+2),v[6],get16(v+8));
                }
                klog("wcn: reg country=%c%c phy=%u rules=%u\n",reg[8],reg[9],phy,count);
            }
        }
    }
    if (mgmt && frame && mgmt_len>=68 && !get32(mgmt+20) && get32(mgmt+16)<=frame_len) {
        f.management_rx++;
        wcn_sta_management(frame,get32(mgmt+16),get32(mgmt+64));
        receive_management(frame,get32(mgmt+16),get32(mgmt+64),(int32_t)get32(mgmt+44));
    }
}

static int send_channel_list(void) {
    struct {
        uint32_t command;
        struct wmi_scan_chan_list_cmd cmd;
        uint32_t array;
        struct wmi_channel ch[SCAN_CHANNELS];
    } p;
    memset(&p,0,sizeof(p)); p.command=0x3003;
    p.cmd.tlv_header=tlv(79,sizeof(p.cmd)-4); p.cmd.num_scan_chans=SCAN_CHANNELS;
    p.array=tlv(18,sizeof(p.ch));
    for (unsigned i=0;i<SCAN_CHANNELS;i++) {
        p.ch[i].tlv_header=tlv(80,sizeof(p.ch[i])-4);
        p.ch[i].mhz=p.ch[i].band_center_freq1=scan_freqs[i];
        /* Passive reception only; leave all transmit power limits at zero. */
        p.ch[i].info=0x80 | (scan_freqs[i]<3000 ? 1 : 0);
    }
    return wcn_htc_wmi_send(&p,sizeof(p));
}

static int send_vdev(void) {
    struct {
        uint32_t command;
        struct wmi_vdev_create_cmd cmd;
        uint32_t array;
        struct wmi_vdev_txrx_streams streams[2];
    } p;
    memset(&p,0,sizeof(p)); p.command=0x5001;
    p.cmd.tlv_header=tlv(86,sizeof(p.cmd)-4); p.cmd.vdev_type=2;
    memcpy(p.cmd.vdev_macaddr.addr,f.mac,6); p.cmd.num_cfg_txrx_streams=2;
    p.array=tlv(18,sizeof(p.streams));
    for (unsigned i=0;i<2;i++) {
        p.streams[i].tlv_header=tlv(411,sizeof(p.streams[i])-4);
        p.streams[i].band=i; /* WMI chainmask bands: 2 GHz = 0, 5 GHz = 1. */
        p.streams[i].supported_tx_streams=p.streams[i].supported_rx_streams=f.chains;
    }
    return wcn_htc_wmi_send(&p,sizeof(p));
}

static int send_scan(void) {
    struct {
        uint32_t command;
        struct wmi_start_scan_cmd cmd;
        uint32_t array, channels[SCAN_CHANNELS], ssids, bssids, ies;
    } p;
    memset(&p,0,sizeof(p)); p.command=0x3001;
    p.cmd.tlv_header=tlv(77,sizeof(p.cmd)-4);
    p.cmd.scan_id=0xa000|f.scan_id; p.cmd.scan_req_id=0xa001;
    p.cmd.scan_priority=2; p.cmd.notify_scan_events=0x1ff;
    p.cmd.dwell_time_active=40; p.cmd.dwell_time_passive=120;
    p.cmd.dwell_time_active_2g=40; p.cmd.dwell_time_passive_6g=120;
    p.cmd.min_rest_time=50; p.cmd.max_rest_time=500;
    p.cmd.idle_time=50; p.cmd.max_scan_time=20000; p.cmd.probe_delay=5;
    p.cmd.scan_ctrl_flags=1 | 0x100 | 0x10000;
    p.cmd.num_chan=SCAN_CHANNELS;
    p.array=tlv(16,sizeof(p.channels));
    for (unsigned i=0;i<SCAN_CHANNELS;i++) p.channels[i]=scan_freqs[i];
    p.ssids=p.bssids=tlv(19,0); p.ies=tlv(17,0);
    if (wcn_htc_wmi_send(&p,sizeof(p))) return -1;
    klog("wcn: passive scan id=%x channels=%u\n",p.cmd.scan_id,(unsigned)SCAN_CHANNELS);
    return 0;
}

static int send_scan_cancel(void) {
    struct {
        uint32_t command, header, requestor, scan_id, type, vdev, pdev;
    } p = {0x3002, 0, 0xa001, 0xa000 | f.scan_id, 0, 0, 0};
    p.header=tlv(78,sizeof(p)-8);
    return wcn_htc_wmi_send(&p,sizeof(p));
}

static int send_init(void) {
    struct {
        uint32_t command, header, abi[6], chunks;
        struct wmi_resource_config resource;
        uint32_t memory_array;
    } p;
    memset(&p, 0, sizeof(p));
    p.command = 1; p.header = tlv(74, 28); p.memory_array = tlv(18, 0);
    struct wmi_resource_config *r = &p.resource;
    r->tlv_header = tlv(75, sizeof(*r)-4);
    r->num_vdevs=3; r->num_peers=16; r->num_tids=32;
    r->num_offload_peers=3; r->num_offload_reorder_buffs=3;
    r->num_peer_keys=2; r->ast_skid_limit=16;
    r->tx_chain_mask = r->rx_chain_mask = (1u << f.chains)-1;
    r->rx_timeout_pri[0]=r->rx_timeout_pri[1]=r->rx_timeout_pri[2]=100;
    r->rx_timeout_pri[3]=40; r->rx_decap_mode=1;
    r->scan_max_pending_req=4; r->bmiss_offload_max_vdev=3;
    r->roam_offload_max_vdev=3; r->roam_offload_max_ap_profiles=8;
    r->tx_dbg_log_size=1024; r->gtk_offload_max_vdev=2;
    r->num_msdu_desc=1024; r->max_frag_entries=10;
    r->num_tdls_vdevs=1; r->num_tdls_conn_table_entries=8;
    r->beacon_tx_offload_max_vdev=2; r->num_multicast_filter_entries=32;
    r->num_wow_filters=22; r->use_pdev_id=1; r->flag1=1u << 5;
    r->flags2=1u << 9;
    if (wcn_htc_wmi_send(&p, sizeof(p))) return -1;
    klog("wcn: WMI INIT sent bytes=%u\n", (unsigned)sizeof(p));
    return 0;
}

void wcn_wmi_poll(void) {
    if (f.failed) return;
    if (f.service_ready && f.ext_ready && f.ext2_ready && !f.init_sent) {
        if (wcn_dp_init()) { f.failed=1; return; }
        if (send_init()) return;
        f.init_sent=1; f.deadline=kernel_tic_ms(0)+20000;
    }
    if (f.init_sent && !f.ready && kernel_tic_ms(0)>=f.deadline) {
        f.failed=1; klog("wcn: WMI READY timeout\n");
    }
    if (!f.ready) return;
    if (!f.hw_mode_sent) {
        /* Set DBS before HTT installs RXDMA rings. Changing mode afterwards
         * can discard the firmware's physical-MAC ring configuration. */
        const uint32_t p[]={0x401e,(515u<<16)|12,0,1,0};
        if (!wcn_htc_wmi_send(p,sizeof(p))) f.hw_mode_sent=1;
        return;
    }
    if (f.country_state==1) {
        f.country_seen=f.country_sent=0;f.country_mask=(f.reg[0].valid ? 1:0)|(f.reg[1].valid ? 2:0);
        if (!f.country_mask) f.country_mask=3;
        f.reg[0].valid=f.reg[1].valid=0;f.channels_sent=0;
        f.country_state=2;f.country_deadline=kernel_tic_ms(0)+8000;
        klog("wcn: country requested=%s\n",country);return;
    }
    if (f.country_state==2) {
        /* The firmware updates only the addressed pdev, even in DBS mode. */
        for (unsigned phy=0;phy<2;phy++) if ((f.country_mask & (1u<<phy)) && !(f.country_sent & (1u<<phy))) {
            uint32_t p[]={0x3a001,(607u<<16)|8,phy+1,(uint8_t)country[0]|((uint32_t)(uint8_t)country[1]<<8)};
            if (!wcn_htc_wmi_send(p,sizeof(p))) f.country_sent|=1u<<phy;
            break;
        }
        if ((f.country_seen&f.country_mask)==f.country_mask) { f.country_state=0;klog("wcn: country applied=%s\n",country); }
        else { if (kernel_tic_ms(0)>=f.country_deadline) { f.failed=1;klog("wcn: country update timeout\n"); } return; }
    }
    if (f.scan_pending) {
        if (kernel_tic_ms(0)>=f.scan_deadline) {
            f.scan_pending=0; f.scan_done=1; f.scan_reason=3;
            return;
        }
        if (!f.channels_sent) { if (!send_channel_list()) f.channels_sent=1; }
        else if (!f.vdev_sent) { if (!send_vdev()) f.vdev_sent=1; }
        else if (!send_scan()) {
            f.scan_pending=0; f.scan_active=1; f.scan_deadline=kernel_tic_ms(0)+25000;
        }
    }
    if (f.scan_active && !f.scan_cancelling && kernel_tic_ms(0)>=f.scan_deadline) {
        f.scan_cancelling=1; f.scan_timedout=1;
        f.scan_deadline=kernel_tic_ms(0)+3000;
        klog("wcn: passive scan timeout; requesting cancellation\n");
    }
    if (f.scan_cancelling) {
        if (f.scan_cancelling==1 && !send_scan_cancel()) f.scan_cancelling=2;
        if (kernel_tic_ms(0)>=f.scan_deadline) {
            /* Do not reuse a vdev while firmware may still be scanning. */
            f.failed=1;
            klog("wcn: scan cancellation timeout; stop/start required\n");
        }
    }
}
void wcn_wmi_reset(void) { memset(&f,0,sizeof(f)); }
int wcn_wmi_station_available(void) {
    return wcn_wmi_ready() && !f.country_state && f.vdev_sent && !f.scan_pending && !f.scan_active && !f.scan_cancelling;
}
void wcn_wmi_status(char *out, size_t size) {
    snprintf(out,size,"wmi_ready=%u wmi_events=%u wmi_bad=%u wmi_failed=%u scan_active=%u aps=%u",
             f.ready,f.events,f.bad,f.failed,f.scan_active,f.ap_count);
}
int wcn_wmi_ready(void) { return f.ready && f.hw_mode_sent && !f.failed; }
void wcn_wmi_mac(uint8_t mac[6]) { memcpy(mac,f.mac,6); }
int wcn_wmi_find_bss(const char *ssid,struct wcn_bss *bss) {
    int found=-1,best=-1000;
    for (unsigned i=0;i<f.ap_count;i++) {
        const struct wcn_bss *b=&f.ap[i];
        if (strcmp(ssid,b->ssid) || wcn_wmi_channel_power(b->mhz)<0) continue;
        /* Strong VHT has more capacity even beside a much louder 2.4 GHz
         * BSS. Reduce the bias near the 5 GHz coverage edge. */
        int bias=b->mhz>3000 && b->ht && b->vht && b->wmm && b->rssi>=-72 ?
            (b->rssi>=-65 ? 40:25):0;
        int score=b->rssi+bias;
        if (found<0 || score>best) { found=i;best=score; }
    }
    if (found<0) return -1;
    *bss=f.ap[found]; return 0;
}
unsigned wcn_wmi_chains(void) { return f.chains; }
unsigned wcn_wmi_vht_caps(void) { return f.vht5; }
unsigned wcn_wmi_nss160(void) { return f.nss160; }
const struct wcn_he_caps *wcn_wmi_he_caps(void) { return &f.he5; }
/* Standard and revised VHT Operation encodings for contiguous 160 MHz.
 * 80+80 and extended-NSS-only capability encodings remain unsupported. */
unsigned wcn_wmi_bss_160_center(const struct wcn_bss *b) {
    unsigned cap=(get32(b->vht_cap)>>2)&3,channel=0;
    if (!b->vht || b->mhz<5000 || (cap!=1 && cap!=2)) return 0;
    if (b->vht_op[0]==2 && !b->vht_op[2]) channel=b->vht_op[1];
    if (b->vht_op[0]==1 && b->vht_op[2] &&
        (b->vht_op[1]+8u==b->vht_op[2] || b->vht_op[2]+8u==b->vht_op[1])) {
        unsigned primary80=5000+5*b->vht_op[1];
        unsigned delta=b->mhz>primary80 ? b->mhz-primary80:primary80-b->mhz;
        if (delta==10 || delta==30) channel=b->vht_op[2];
    }
    if (channel!=50 && channel!=114 && channel!=163) return 0;
    unsigned center=5000+5*channel,delta=b->mhz>center ? b->mhz-center:center-b->mhz;
    return delta<=70 && delta%20==10 ? center:0;
}
int wcn_wmi_station_width_power(const struct wcn_bss *b,unsigned center,unsigned width) {
    if (width!=160) return wcn_wmi_channel_width_power(b->mhz,center,width);
    /* Only follow a discovered AP's complete 160 MHz channel. The primary
     * must permit active station traffic. DFS on secondary subchannels is
     * managed by the AP; this does not enable active scanning or AP mode. */
    if (center!=wcn_wmi_bss_160_center(b) || wcn_wmi_channel_power(b->mhz)<0) return -1;
    int power=-1;unsigned phys=0;
    for (unsigned phy=0;phy<2;phy++) {
        if (!f.reg[phy].valid) continue;
        if (f.reg[phy].max_bw5<160) return -1;
        phys++;
        /* Adjacent 5 GHz rules use AUTO_BW in Linux ath11k/reg.c. Check
         * complete coverage and the lowest power across all subchannels. */
        for (unsigned mhz=center-70;mhz<=center+70;mhz+=20) {
            unsigned covered=0;
            for (unsigned i=0;i<f.reg[phy].count;i++) {
                if (mhz-10<f.reg[phy].rule[i].start || mhz+10>f.reg[phy].rule[i].end) continue;
                unsigned flags=f.reg[phy].rule[i].flags;
                if (flags&(1|2|16|64|128|256|2048) || f.reg[phy].rule[i].bw<80) return -1;
                unsigned limit=f.reg[phy].rule[i].power;
                if (power<0 || limit<(unsigned)power) power=limit;
                covered=1;
            }
            if (!covered) return -1;
        }
    }
    return phys ? power:-1;
}
int wcn_wmi_scan_finished(void) { return f.scan_done && !f.scan_pending && !f.scan_active && !f.scan_cancelling; }
int wcn_wmi_channel_power(unsigned mhz) { return wcn_wmi_channel_width_power(mhz,mhz,20); }
int wcn_wmi_channel_width_power(unsigned mhz,unsigned center,unsigned width) {
    if ((width!=20 && width!=40 && width!=80) || center<width/2 || mhz+10>center+width/2 || mhz< center-width/2+10) return -1;
    int power=-1;
    for (unsigned phy=0;phy<2;phy++) {
        if (!f.reg[phy].valid) continue;
        for (unsigned i=0;i<f.reg[phy].count;i++) {
            if (center-width/2<f.reg[phy].rule[i].start || center+width/2>f.reg[phy].rule[i].end || width>f.reg[phy].rule[i].bw) continue;
            /* No active transmission on disabled/no-IR/radar/no-20MHz rules. */
            if (f.reg[phy].rule[i].flags & (1|2|8|64|2048|(width>=40 ? 16:0)|(width>=80 ? 128:0))) return -1;
            unsigned limit=f.reg[phy].rule[i].power;
            if (power<0 || limit<(unsigned)power) power=limit;
        }
    }
    return power>30 ? 30 : power;
}
int wcn_wmi_scan(void) {
    if (f.country_state || wcn_sta_busy() || !wcn_wmi_ready() || f.scan_pending || f.scan_active || f.scan_cancelling) return -1;
    memset(f.ap,0,sizeof(f.ap)); f.ap_count=f.scan_done=f.scan_reason=f.scan_events=0;
    f.scan_id=(f.scan_id+1)&0xfff; if (!f.scan_id) f.scan_id=1;
    f.scan_pending=1; f.scan_timedout=0;
    f.scan_deadline=kernel_tic_ms(0)+5000;
    return 0;
}
int wcn_wmi_scan_cancel(void) {
    if (!wcn_wmi_ready()) return -1;
    if (f.scan_pending) {
        f.scan_pending=0; f.scan_done=1; f.scan_reason=1;
    } else if (f.scan_active && !f.scan_cancelling) {
        f.scan_cancelling=1; f.scan_deadline=kernel_tic_ms(0)+3000;
    } else return -1;
    return 0;
}
void wcn_wmi_scan_results(char *out, size_t size) {
    int n=snprintf(out,size,"scan pending=%u active=%u cancelling=%u done=%u reason=%u events=%u management_rx=%u aps=%u\n",
                   f.scan_pending,f.scan_active,f.scan_cancelling,f.scan_done,f.scan_reason,f.scan_events,f.management_rx,f.ap_count);
    if (n<0 || (size_t)n>=size) return;
    unsigned pos=n;
    for (unsigned i=0;i<f.ap_count && pos<size;i++) {
        n=snprintf(out+pos,size-pos,"%02x:%02x:%02x:%02x:%02x:%02x %u MHz %d dBm %s %s HT=%u VHT=%u HE=%u WMM=%u cap=%08x op=%u/%u/%u\n",
                   f.ap[i].bssid[0],f.ap[i].bssid[1],f.ap[i].bssid[2],f.ap[i].bssid[3],f.ap[i].bssid[4],f.ap[i].bssid[5],
                   f.ap[i].mhz,f.ap[i].rssi,f.ap[i].privacy?"protected":"open",f.ap[i].ssid,f.ap[i].ht,f.ap[i].vht,f.ap[i].he.present && f.ap[i].he_operation,f.ap[i].wmm,get32(f.ap[i].vht_cap),f.ap[i].vht_op[0],f.ap[i].vht_op[1],f.ap[i].vht_op[2]);
        if (n<0 || (size_t)n>=size-pos) return;
        pos+=n;
    }
}
