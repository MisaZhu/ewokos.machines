/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/* WMI control protocol reference: Linux ath11k/wmi.c and hw.c.
 * Copyright (c) 2018-2019 The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. */
#include "wcn_wmi.h"
#include "wcn_wmi_proto.h"
#include "wcn_htc.h"
#include <ewoksys/klog.h>
#include <ewoksys/kernel_tic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static struct {
    unsigned service_ready, ext_ready, ext2_ready, init_sent, ready, failed;
    unsigned chains, events, bad, memory_requests;
    unsigned vdev_sent, channels_sent, scan_pending, scan_active, scan_done;
    unsigned scan_id, scan_reason, scan_events, management_rx, ap_count;
    unsigned scan_cancelling, scan_timedout;
    uint8_t mac[6];
    uint64_t deadline;
    uint64_t scan_deadline;
    struct { uint8_t bssid[6]; char ssid[33]; unsigned mhz, privacy; int rssi; } ap[64];
} f;
static const unsigned scan_freqs[] = {
    2412,2417,2422,2427,2432,2437,2442,2447,2452,2457,2462,
    5180,5200,5220,5240
};
#define SCAN_CHANNELS (sizeof(scan_freqs)/sizeof(scan_freqs[0]))
static unsigned get16(const uint8_t *p) { return p[0] | ((unsigned)p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | ((uint32_t)get16(p+2) << 16); }
static uint32_t tlv(unsigned tag, unsigned bytes) { return tag << 16 | bytes; }

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
}

void wcn_wmi_receive(const void *packet, unsigned bytes) {
    const uint8_t *p = packet;
    if (bytes < 4) { f.bad++; return; }
    unsigned event = get32(p) & 0xffffff;
    f.events++;
    klog("wcn: WMI event=%x bytes=%u\n", event, bytes);
    /* Validate the entire TLV stream before publishing any state. */
    unsigned pos = 4;
    while (pos < bytes) {
        if (bytes - pos < 4 || get16(p+pos) > bytes - pos - 4) { f.bad++; f.failed=1; return; }
        pos += 4 + get16(p+pos);
    }
    const uint8_t *mgmt = NULL, *frame = NULL;
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
        if (event == 0x4009 && tag == 428) f.ext_ready = 1;
        if (event == 0x4022 && tag == 820) f.ext2_ready = 1;
        if (event == 2 && tag == 35 && len >= 36) {
            unsigned status=get32(v+32);
            if (status || (v[24]&1)) { f.failed=1; return; }
            memcpy(f.mac,v+24,6);
            f.ready = 1;
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
        pos += len + 4;
    }
    if (mgmt && frame && mgmt_len>=68 && !get32(mgmt+20) && get32(mgmt+16)<=frame_len) {
        f.management_rx++;
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
        p.streams[i].band=i+1;
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
        if (send_init()) return;
        f.init_sent=1; f.deadline=kernel_tic_ms(0)+20000;
    }
    if (f.init_sent && !f.ready && kernel_tic_ms(0)>=f.deadline) {
        f.failed=1; klog("wcn: WMI READY timeout\n");
    }
    if (!f.ready) return;
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
void wcn_wmi_status(char *out, size_t size) {
    snprintf(out,size,"wmi_ready=%u wmi_events=%u wmi_bad=%u wmi_failed=%u scan_active=%u aps=%u",
             f.ready,f.events,f.bad,f.failed,f.scan_active,f.ap_count);
}
int wcn_wmi_ready(void) { return f.ready && !f.failed; }
int wcn_wmi_scan(void) {
    if (!wcn_wmi_ready() || f.scan_pending || f.scan_active || f.scan_cancelling) return -1;
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
        n=snprintf(out+pos,size-pos,"%02x:%02x:%02x:%02x:%02x:%02x %u MHz %d dBm %s %s\n",
                   f.ap[i].bssid[0],f.ap[i].bssid[1],f.ap[i].bssid[2],f.ap[i].bssid[3],f.ap[i].bssid[4],f.ap[i].bssid[5],
                   f.ap[i].mhz,f.ap[i].rssi,f.ap[i].privacy?"protected":"open",f.ap[i].ssid);
        if (n<0 || (size_t)n>=size-pos) return;
        pos+=n;
    }
}
