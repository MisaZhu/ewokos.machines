/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/* WCN6855 SRNG/HTT reference: Linux ath11k/hal.c, hw.c, dp.c, dp_tx.c.
 * Copyright (c) 2018-2019 The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. */
#include "wcn_dp.h"
#include "wcn_mhi.h"
#include "wcn_htc.h"
#include "wcn_htt.h"
#include "wcn_sta.h"
#include "wcn_wmi.h"
#include "wcn_net.h"
#include "wcn_wpa.h"
#include <ewoksys/dma.h>
#include "wcn_util.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdio.h>
#include <string.h>

#define DP_BYTES (16u * 1024 * 1024)
#define WBM 0xa34000u
#define REO 0xa38000u
#define TCL 0xa44000u
#define LINK_COUNT 4096u
/* ath11k sizing (DP_RXDMA_BUF_RING_SIZE): the firmware drains the shared refill
 * ring into one 1024-entry RX ring per MAC, so up to 2*1023 buffers sit in the
 * MAC rings on top of whatever is still queued here. Every one of them has to
 * be a distinct buffer. */
#define RX_RING 4096u
#define RX_COUNT (RX_RING - 1u)
#define RX_BYTES 2048u
#define MAX_RINGS 24u

/* Why an RX buffer was not delivered. The first group is decided by the ring
 * entry (the hardware already rejected the frame), the second by the host
 * while reading the RX descriptor. */
enum {
    DROP_NONE,DROP_STALE,DROP_REO_ERROR,DROP_REO_PARTIAL,DROP_RXDMA_ERROR,DROP_WBM,
    DROP_NOT_DONE,DROP_ATTENTION,DROP_LENGTH,DROP_DECAP,DROP_KINDS
};
/* rx_attention info1 error bits, per Linux rx_desc.h. */
static const uint8_t attn_bits[]={12,13,16,17,26,27,28,29,30,31};
#define ATTN_ERRS (sizeof(attn_bits))
struct dp_ring {
    unsigned id, reg, ptr, entries, words, dest, offset, hw_offset, sw_offset;
    unsigned index, loop, consumed, htt_type, htt_id, pdev;
};
static struct {
    uint8_t *dma;
    uint64_t pa;
    unsigned used, active, failed, count, configured, bad, completions;
    unsigned mac2_filter, rx_filter_sent;
    unsigned links, rx_buffers, tx_buffers;
    unsigned rx_packets,rx_delivered,rx_dropped,rx_stale,refilled,tx_packets,tx_done,tx_errors,tx_generation;
    unsigned tx_batch,tx_pending, tx_block_space,tx_block_slot;
    unsigned tx_last_source,tx_last_reason,tx_last_info,tx_last_rate,tx_last_count;
    unsigned tx_cookie[256];
    unsigned rx_rate,tx_rate,rx_ht,rx_vht,rx_he,tx_ht,tx_vht,tx_he;
    uint8_t rx_owner[RX_COUNT+1u];
    uint16_t rx_free[RX_COUNT];
    unsigned rx_free_count;
    unsigned drop_why[DROP_KINDS],reo_err[32],rxdma_err[32],attn_err[ATTN_ERRS];
    /* What the frames released through WBM are. seq[now][before] pairs a frame
     * with the previous frame of the same TID and sequence number (0 delivered,
     * 1 WBM); seq_once counts WBM frames whose sequence number was not seen. */
    struct {
        unsigned push[4],done,fc_valid,type[4],subtype[4][16],tid[17],retry,ampdu,encrypted,
                 from_peer,queue_null,ast_miss,tcp_ack,small,decap[4],seq[2][2],seq_once,seq_unknown,samples;
    } wbm;
    uint8_t rx_peer[6];
    uint32_t rx_seen[17][4096];
    struct dp_ring rings[MAX_RINGS];
} d;
/* Join-band request; survives wcn_dp_release. Default on: the MAC2 RXDMA
 * filter is required for 2.4G receive; the devcmd mac2filter can clear it
 * (5G-only debugging) until the next controller restart. */
static unsigned mac2_filter_req = 1;
/* Receive buffer manager id written into every posted RX buffer. 4 is
 * HAL_RX_BUF_RBM_SW1_BM, the ath11k hw_params value for WCN6855.
 * /etc/wlan.rbm overrides it for A/B tests without a rebuild. */
static unsigned dp_rbm(void) {
    static int cached=-1;
    if (cached<0) {
        unsigned v=0; cached=4;
        FILE *fp=fopen("/etc/wlan.rbm","r");
        if (fp) { if (fscanf(fp,"%u",&v)==1 && v>=1 && v<=7) cached=(int)v; fclose(fp); }
    }
    return (unsigned)cached;
}
static void dp_barrier(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static unsigned dp_reserve(unsigned bytes) {
    /* Link descriptors require 128-byte physical alignment. */
    unsigned off=(d.used+127)&~127u;
    if (!bytes || off>DP_BYTES || bytes>DP_BYTES-off) return 0;
    d.used=off+bytes; return off;
}
static uint32_t *dp_words(unsigned off) { return (uint32_t *)(void *)(d.dma+off); }
static unsigned dp_hw(const struct dp_ring *r) {
    /* SRNG setup programs HP/TP_ADDR for hardware writeback into our
     * uncached DMA arena. Reading it avoids a PCIe MMIO round trip per
     * packet; the WBM idle ring alone has no writeback address. */
    unsigned value=r->id==104 ? wcn_soc_read(r->ptr+4) :
        *(volatile uint32_t *)(void *)(d.dma+r->hw_offset);
    dp_barrier();
    if (value>=r->entries*r->words || value%r->words) {
        d.bad++; d.failed=1; return 0;
    }
    return value;
}
static void dp_publish(struct dp_ring *r) {
    dp_barrier();
    if (r->ptr) wcn_soc_write(r->ptr+(r->dest ? 4 : 0),r->index*r->words);
    else {
        *(volatile uint32_t *)(void *)(d.dma+r->sw_offset)=r->index*r->words;
        dp_barrier();
    }
}
static struct dp_ring *dp_add(unsigned id, unsigned reg, unsigned ptr,
                              unsigned entries, unsigned words, unsigned dest) {
    if (d.count==MAX_RINGS) return NULL;
    struct dp_ring *r=&d.rings[d.count];
    r->id=id; r->reg=reg; r->ptr=ptr; r->entries=entries; r->words=words; r->dest=dest;
    r->offset=dp_reserve(entries*words*4);
    r->hw_offset=dp_reserve(4); r->sw_offset=dp_reserve(4); r->loop=1;
    if (!r->offset || !r->hw_offset || !r->sw_offset) return NULL;
    d.count++;
    if (!reg) return r; /* LMAC registers are owned by firmware via HTT. */
    uint64_t pa=d.pa+r->offset, hw=d.pa+r->hw_offset;
    wcn_soc_write(reg+0x10,0);
    wcn_soc_write(reg,(uint32_t)pa);
    wcn_soc_write(reg+4,(pa>>32) | (entries*words<<8));
    wcn_soc_write(reg+8,words | (dest ? id<<8 : 0));
    unsigned off=dest ? 0x14 : 0x1c;
    /* WBM idle has an internal tail; its TP_ADDR registers are reserved. */
    if (id!=104) {
        wcn_soc_write(reg+off,(uint32_t)hw); wcn_soc_write(reg+off+4,hw>>32);
    }
    wcn_soc_write(reg+(dest ? 0x24 : 0x30),0);
    if (!dest) wcn_soc_write(reg+0x34,0);
    wcn_soc_write(reg+0x48,0); wcn_soc_write(reg+0x4c,0); /* Poll; no MSI. */
    wcn_soc_write(ptr,0); wcn_soc_write(ptr+4,0);
    dp_barrier(); wcn_soc_write(reg+0x10,dest ? 0x40 : 0x42);
    return r;
}
static int dp_local(unsigned id,unsigned base,unsigned reg,unsigned ptr,
                    unsigned entries,unsigned words,unsigned dest) {
    return dp_add(id,base+reg,base+ptr,entries,words,dest) ? 0 : -1;
}
static int dp_lmac(unsigned id,unsigned entries,unsigned words,unsigned dest,
                   unsigned type,unsigned htt_id,unsigned pdev) {
    struct dp_ring *r=dp_add(id,0,0,entries,words,dest);
    if (!r) return -1;
    r->htt_type=type; r->htt_id=htt_id; r->pdev=pdev; return 0;
}

/* REO/RXDMA arming. The HTT ring configuration only describes where the rings
 * live; the hardware still has to be told to consume them. The firmware
 * re-initialises this block whenever it reworks the MAC (DBS switch, vdev
 * restart, roam recovery), and a wiped arming shows up as "RXDMA never fetched
 * a single RX descriptor" - every RX buffer stays unconsumed, so no frame can
 * reach the host and the 4-way handshake times out. */
static void dp_arm_hw(void) {
    wcn_soc_write(REO,wcn_soc_read(REO)|12);
    wcn_soc_write(REO+0x630,wcn_soc_read(REO+0x630)&~(15u<<17));
    for (unsigned i=0;i<4;i++) wcn_soc_write(REO+0x5bc+i*4,100000);
    /* Every hash maps to REO2SW1. The remaining rings are still valid. */
    wcn_soc_write(REO+0xc,0x11111111); wcn_soc_write(REO+0x10,0x11111111);
    dp_barrier();
}

int wcn_dp_init(void) {
    if (d.active) return d.failed ? -1 : 0;
    if (d.dma) return -1;
    d.dma=(uint8_t *)dma_alloc(0,DP_BYTES);
    if (!d.dma) return -1;
    d.pa=dma_phy_addr(0,(ewokos_addr_t)d.dma);
    if (!d.pa || d.pa+DP_BYTES>0x100000000ULL) return -1;
    memset(d.dma,0,DP_BYTES); d.used=64;
    if (dp_local(104,WBM,0x870,0x30b0,LINK_COUNT,2,0) ||
        dp_local(105,WBM,0x1e8,0x3018,128,8,0) ||
        dp_local(24,TCL,0x798,0x2018,32,8,0) ||
        dp_local(25,TCL,0x8a0,0x2030,32,9,1) ||
        dp_local(16,TCL,0x690,0x2000,256,8,0) ||
        dp_local(106,WBM,0x920,0x30c0,512,8,1) ||
        dp_local(109,WBM,0xa28,0x30d8,512,8,1) ||
        dp_local(5,REO,0x1ec,0x3028,32,8,0) ||
        dp_local(4,REO,0x454,0x3060,256,16,1) ||
        dp_local(8,REO,0x194,0x3020,128,10,0) ||
        dp_local(9,REO,0x55c,0x3078,128,26,1)) return -1;
    for (unsigned i=0;i<4;i++)
        if (dp_local(i,REO,0x244+i*0x58,0x3030+i*8,512,16,1)) return -1;
    if (dp_lmac(128,RX_RING,2,0,2,5,0)) return -1;
    for (unsigned i=0;i<2;i++) {
        /* WCN6855 has 15 IDs per LMAC. FW fills each MAC's RX buffer ring. */
        if (dp_lmac(129+i*15,1024,2,0,1,0,i+1) ||
            dp_lmac(133+i*15,512,8,1,0,7,i+1) ||
            dp_lmac(132+i*15,128,2,0,1,1,i+1)) return -1;
    }
    d.links=dp_reserve(LINK_COUNT*128);
    d.rx_buffers=dp_reserve(RX_COUNT*RX_BYTES);
    d.tx_buffers=dp_reserve(256*RX_BYTES);
    if (!d.links || !d.rx_buffers || !d.tx_buffers) return -1;
    for (unsigned i=0;i<RX_COUNT;i++) { d.rx_free[i]=RX_COUNT-i; d.rx_owner[i+1]=2; }
    d.rx_free_count=RX_COUNT;
    for (unsigned j=0;j<d.count;j++) {
        struct dp_ring *r=&d.rings[j];
        if (r->id==104) {
            unsigned buffers=r->entries-1;
            for (unsigned i=0;i<buffers;i++) {
                unsigned off=r->id==104 ? d.links+i*128 : d.rx_buffers+i*RX_BYTES;
                uint64_t pa=d.pa+off;
                uint32_t *p=dp_words(r->offset+i*8);
                p[0]=pa; p[1]=(pa>>32) | (r->id==104 ? 1u<<8 : (4u<<8)|((i+1)<<11));
            }
            r->index=buffers; dp_publish(r);
        }
        if (r->id==16) {
            for (unsigned i=0;i<r->entries;i++)
                dp_words(r->offset+i*32)[0]=(324<<1)|(28<<10);
        }
        if (r->id==8) {
            for (unsigned i=0;i<r->entries;i++) dp_words(r->offset+i*40)[1]=i+1;
        }
    }
    dp_arm_hw();
    d.active=1;
    return 0;
}

static int dp_configure(const struct dp_ring *r) {
    uint64_t base=d.pa+r->offset, hw=d.pa+r->hw_offset, sw=d.pa+r->sw_offset;
    uint64_t hp=r->dest ? hw : sw, tp=r->dest ? sw : hw;
    uint32_t p[13]={0};
    p[0]=0xb | (r->pdev<<8) | (r->htt_id<<16) | (r->htt_type<<24);
    p[1]=base; p[2]=base>>32;
    p[3]=r->entries*r->words | (r->words<<16) | (r->htt_type==1 ? 1u<<25 : 0);
    p[4]=hp; p[5]=hp>>32; p[6]=tp; p[7]=tp>>32;
    p[11]=r->dest ? r->words | (1u<<16) : 0; /* Keep remote HP updates running while polling. */
    if (wcn_htc_htt_send(p,sizeof(p))) return -1;
    return 0;
}

static struct dp_ring *dp_find(unsigned id) {
    for (unsigned i=0;i<d.count;i++) if (d.rings[i].id==id) return &d.rings[i];
    return NULL;
}
static int dp_fault(const char *why) {
    d.bad++; d.failed=1; wcn_log("wcn: DP invalid %s; stop/start required\n",why);
    /* Persist the reason: the restart ladder the fault triggers takes the ssh
     * session down before the ring log can be read. */
    wcn_diag("dp-fault why=%s stale=%u rx=%u/%u\n",why,d.rx_stale,d.rx_delivered,d.rx_packets);
    return -1;
}
static unsigned dp_space(struct dp_ring *r) {
    unsigned tail=dp_hw(r)/r->words;
    return d.failed ? 0 : (tail+r->entries-r->index-1)%r->entries;
}
static void dp_next(struct dp_ring *r) { r->index=(r->index+1)%r->entries; }
static uint64_t dp_address(const uint32_t *p) { return p[0]|((uint64_t)(p[1]&255)<<32); }
/* Returns the buffer id, 0 for a completion of a buffer the host already holds
 * (counted in rx_stale, skipped), and -1 for a descriptor that cannot belong to
 * this pool at all. Every buffer is posted exactly once per ownership cycle, so
 * rx_stale staying at 0 is the proof that the pool accounting matches the
 * firmware's. */
static int dp_rx_id(const uint32_t *p) {
    unsigned id=p[1]>>11;
    if (!id || id>RX_COUNT || dp_address(p)!=d.pa+d.rx_buffers+(id-1)*RX_BYTES)
        return dp_fault("RX address/cookie");
    if (d.rx_owner[id]!=1) {
        d.rx_stale++; d.rx_dropped++; d.drop_why[DROP_STALE]++;
        if (d.rx_stale<=4)
            wcn_log("wcn: stale RX completion cookie=%u (buffer not posted); skipped\n",id);
        return 0;
    }
    return (int)id;
}
/* One shared refill ring feeds both MACs (as in ath11k wcn6855). Only buffers
 * the host got back are posted again: a buffer the firmware still holds in a
 * MAC ring must never be offered a second time. */
static void dp_refill(void) {
    struct dp_ring *r=dp_find(128);
    unsigned space=dp_space(r),posted=0;
    while (space-- && d.rx_free_count) {
        unsigned i=d.rx_free[--d.rx_free_count];
        uint64_t pa=d.pa+d.rx_buffers+(i-1)*RX_BYTES;
        uint32_t *p=dp_words(r->offset+r->index*8);
        p[0]=(uint32_t)pa; p[1]=((uint32_t)(pa>>32)&255u)|(dp_rbm()<<8)|(i<<11);
        d.rx_owner[i]=1; dp_next(r); posted++;
    }
    if (posted) { dp_publish(r); d.refilled+=posted; }
}
/* Diagnostic only: describes WBM-released frames and matches their sequence
 * numbers against the frames delivered through REO. Field positions per Linux
 * rx_desc.h (rx_mpdu_start_wcn6855 starts at word 36). */
static void dp_rx_trace(const uint32_t *p,const uint8_t *b,unsigned wbm,unsigned len) {
    unsigned info=p[47],fc=b[200]|((unsigned)b[201]<<8);
    unsigned tid=(info&(1u<<7)) ? (p[43]>>15)&15 : 16,seq=info>>20;
    unsigned peer=!memcmp(b+210,d.rx_peer,6);
    if (!wbm) memcpy(d.rx_peer,b+210,6);
    else {
        d.wbm.done+=p[23]>>31; d.wbm.fc_valid+=info&1;
        if (info&1) { d.wbm.type[(fc>>2)&3]++; d.wbm.subtype[(fc>>2)&3][(fc>>4)&15]++; }
        d.wbm.tid[tid]++; d.wbm.retry+=(info>>19)&1; d.wbm.ampdu+=(p[48]>>28)&1;
        d.wbm.encrypted+=(info>>18)&1; d.wbm.from_peer+=peer;
        d.wbm.queue_null+=!p[37] && !(p[38]&255);
        d.wbm.ast_miss+=!!(p[49]&(3u<<16)); d.wbm.tcp_ack+=(p[27]>>15)&1;
        d.wbm.small+=len<128; d.wbm.decap[(p[27]>>8)&3]++;
        if (d.wbm.samples<8) {
            d.wbm.samples++;
            wcn_diag("rx-wbm n=%u fc=%04x info1=%08x info2=%08x info3=%08x info5=%08x info6=%08x queue=%02x%08x ast=%08x attn=%08x/%08x len=%u msdu=%08x end=%08x tcp=%08x/%08x/%04x a1=%02x%02x%02x%02x%02x%02x a2=%02x%02x%02x%02x%02x%02x\n",
                     d.wbm.samples,fc,info,p[43],p[36],p[48],p[49],p[38]&255,p[37],p[46],p[22],p[23],len,p[27],p[11],p[8],p[9],p[10]&0xffff,
                     b[204],b[205],b[206],b[207],b[208],b[209],b[210],b[211],b[212],b[213],b[214],b[215]);
        }
    }
    /* Frame control, transmitter and sequence control all valid. */
    if ((info&0x49)!=0x49 || (wbm && !peer)) { d.wbm.seq_unknown+=wbm; return; }
    uint32_t before=d.rx_seen[tid][seq];
    if (before && d.rx_packets-(before&0x7fffffffu)<2048) d.wbm.seq[wbm][before>>31]++;
    else d.wbm.seq_once+=wbm;
    d.rx_seen[tid][seq]=(d.rx_packets&0x7fffffffu)|(wbm<<31);
}
static void dp_rx_buffer(unsigned id,unsigned why) {
    const uint32_t *p=dp_words(d.rx_buffers+(id-1)*RX_BYTES);
    const uint8_t *b=(const void *)p;
    d.rx_packets++;
    /* WCN6855 RX descriptor is 392 bytes, per Linux rx_desc.h. */
    unsigned len=p[26]&0x3fff,pad=(p[11]>>26)&3,decap=(p[27]>>8)&3;
    unsigned err=0;
    for (unsigned i=0;i<ATTN_ERRS;i++) err|=p[22]&(1u<<attn_bits[i]);
    if (!why) {
        if (!(p[23]>>31)) why=DROP_NOT_DONE;
        else if (err) why=DROP_ATTENTION;
        else if (len<14 || len>RX_BYTES-392-pad) why=DROP_LENGTH;
        else if (decap!=2) why=DROP_DECAP;
    }
    if (!why || why==DROP_WBM) dp_rx_trace(p,b,why==DROP_WBM,len);
    /* Frame-level trace of the first completions of a session: the differ-
     * ence between an AP that completes a handshake and one that never sends
     * M1 is visible here (a working AP writes descriptors, a silent one does
     * not reach this function at all). */
    if (d.rx_packets<=12)
        wcn_diag("rx-frame n=%u len=%u pad=%u decap=%u drop=%u err=%08x d23=%08x d11=%08x fc=%04x tid=%u rate=%08x\n",
                 d.rx_packets,len,pad,decap,why,err,p[23],p[11],
                 (unsigned)(b[200]|((unsigned)b[201]<<8)),(p[47]&(1u<<7))?(p[43]>>15)&15:16,p[30]);
    if (!why) {
        struct wcn_rx_security sec;
        memset(&sec,0,sizeof(sec));
        sec.pn=p[39]|((uint64_t)p[40]<<32);
        sec.encrypted=!!(p[47]&(1u<<18));sec.group=b[204]&1;sec.key=p[48]&3;
        sec.tid=(p[47]&(1u<<7)) ? (p[43]>>15)&15:16;
        sec.amsdu=!!((p[47]&(1u<<7)) && (b[230]&128));sec.first=!!(p[11]&(1u<<28));sec.last=!!(p[11]&(1u<<29));
        sec.sequence=p[47]>>20;sec.number=p[27]&255;
        memcpy(sec.transmitter,b+210,6);
        /* Require a validated CCMP PN and a successful hardware MIC/decrypt.
         * EAPOL without encryption is handled separately by the supplicant. */
        unsigned fc=b[200]|((unsigned)b[201]<<8);
        /* Frame control, both addresses, sequence control and crypto metadata
         * must all be present before accepting decrypted station traffic. */
        sec.valid=(fc&0x470f)==0x4208 && (p[47]&0x24d)==0x24d && !(p[47]&(31u<<10)) &&
            ((p[43]>>2)&15)==6 && (p[61]&(1u<<18)) &&
            !(p[23]&((7u<<10)|(1u<<3))) && !(p[40]&0xffff0000) && !p[41] && !p[42];
        d.rx_rate=p[30];unsigned phy=(d.rx_rate>>8)&15;
        if (phy==2) d.rx_ht++;
        if (phy==3) d.rx_vht++;
        if (phy==4) d.rx_he++;
        d.rx_delivered++; wcn_sta_data(b+392+pad,len,&sec);
    } else {
        d.rx_dropped++; d.drop_why[why]++;
        if (why==DROP_ATTENTION)
            for (unsigned i=0;i<ATTN_ERRS;i++) if (err&(1u<<attn_bits[i])) d.attn_err[i]++;
    }
    /* Cleared before reuse so a stale msdu_done can never pass for a new frame. */
    memset(d.dma+d.rx_buffers+(id-1)*RX_BYTES,0,RX_BYTES);
    d.rx_owner[id]=2; d.rx_free[d.rx_free_count++]=id;
}
/* Validate a complete bounded link chain before returning any child buffers. */
static int dp_link_drop(const uint32_t *entry,unsigned why) {
    uint32_t links[8][2],ids[48]; unsigned count=0,nids=0;
    uint32_t address[2]={entry[0],entry[1]};
    while (dp_address(address)) {
        if (count==8) return dp_fault("RX link chain length");
        uint64_t pa=dp_address(address),base=d.pa+d.links;
        if (pa<base || pa>=base+LINK_COUNT*128 || (pa-base)%128) return dp_fault("RX link address");
        for (unsigned j=0;j<count;j++) if (dp_address(links[j])==pa) return dp_fault("RX link cycle");
        memcpy(links[count++],address,8);
        const uint32_t *q=dp_words((unsigned)(pa-d.pa));
        for (unsigned j=0;j<6;j++) {
            const uint32_t *buf=q+8+j*4;
            if (!dp_address(buf)) continue;
            int id=dp_rx_id(buf); if (id<0) return -1;
            if (!id) continue;
            for (unsigned k=0;k<nids;k++) if (ids[k]==(unsigned)id) return dp_fault("duplicate RX link buffer");
            ids[nids++]=id;
        }
        address[0]=q[1]; address[1]=q[2];
    }
    struct dp_ring *r=dp_find(105);
    if (dp_space(r)<count) return 1; /* Retain destination until release space exists. */
    for (unsigned i=0;i<nids;i++) dp_rx_buffer(ids[i],why);
    for (unsigned i=0;i<count;i++) {
        uint32_t *p=dp_words(r->offset+r->index*32); memset(p,0,32);
        p[0]=links[i][0]; p[1]=(links[i][1]&255)|(1u<<8); p[2]=4|(1u<<6);
        dp_next(r);
    }
    if (count) dp_publish(r);
    return 0;
}
static int dp_complete(struct dp_ring *r,const uint32_t *p) {
    if (r->id<=4) {
        unsigned error=((p[7]>>9)&3)!=1;
        if (p[7]&(1u<<8)) {
            int rc=dp_link_drop(p,DROP_REO_ERROR);
            if (!rc) d.reo_err[(p[7]>>11)&31]++;
            return rc;
        }
        int id=dp_rx_id(p); if (id<0) return -1;
        if (!id) return 0;
        if (error) d.reo_err[(p[7]>>11)&31]++;
        /* MSDU continuation or MSDU drop: not a complete frame in this buffer. */
        dp_rx_buffer(id,error ? DROP_REO_ERROR : (p[4]&((1u<<2)|(1u<<22))) ? DROP_REO_PARTIAL : DROP_NONE);
    } else if (r->id==133 || r->id==148) {
        int rc=dp_link_drop(p,DROP_RXDMA_ERROR);
        if (!rc) d.rxdma_err[(p[6]>>2)&31]++;
        return rc;
    } else if (r->id==109) {
        unsigned type=(p[2]>>6)&7,source=p[2]&7;
        if (type>1) return dp_fault("WBM RX descriptor type");
        if (type==1) {
            int rc=dp_link_drop(p,DROP_WBM); if (rc) return rc;
        } else {
            int id=dp_rx_id(p); if (id<0) return -1;
            if (!id) return 0;
            dp_rx_buffer(id,DROP_WBM);
        }
        if (source==1) d.rxdma_err[(p[2]>>19)&31]++;
        if (source==2) { d.reo_err[(p[2]>>26)&31]++; d.wbm.push[(p[2]>>24)&3]++; }
    } else if (r->id==106) {
        unsigned cookie=p[1]>>11,slot=cookie&255;
        if (!cookie || d.tx_cookie[slot]!=cookie || dp_address(p)!=d.pa+d.tx_buffers+slot*RX_BYTES)
            return dp_fault("TX completion address/cookie");
        d.tx_cookie[slot]=0; d.tx_done++;
        unsigned src=p[2]&7;
        unsigned success=(src==0 && !((p[2]>>13)&15)) || (src==3 && !((p[2]>>9)&15));
        d.tx_last_source=src; d.tx_last_info=p[2]; d.tx_last_rate=p[5];
        d.tx_last_reason=src==0 ? (p[2]>>13)&15 : src==3 ? (p[2]>>9)&15 : 0;
        d.tx_last_count=(p[3]>>24)&127;
        if (!success) d.tx_errors++;
        /* Keep a bounded sample of failures: a recycled descriptor alone is
         * not proof of an ACK, and tx_count/success say whether the AP
         * actually answered the frame. */
        if (!success && d.tx_errors<=4)
            wcn_log("wcn: data TX failed cookie=%u source=%u reason=%u tx_count=%u info=%08x rate=%08x\n",
                    cookie,src,d.tx_last_reason,d.tx_last_count,p[2],p[5]);
        if (src==0 && (p[5]&1)) {
            d.tx_rate=p[5];unsigned phy=(d.tx_rate>>3)&15;
            if (phy==2) d.tx_ht++;
            if (phy==3) d.tx_vht++;
            if (phy==4) d.tx_he++;
        }
        wcn_wpa_tx_complete(cookie,success);
    } else if (r->id==9) {
        if (((p[0]>>1)&511)==153) wcn_sta_reorder_complete(p[1]&65535,(p[1]>>26)&3);
    } else if (r->id!=25) return dp_fault("unhandled destination");
    return 0;
}
int wcn_dp_reorder(uint64_t queue,unsigned window,unsigned ssn) {
    if (!wcn_dp_ready() || !queue || queue>>40 || !window || window>256 || ssn>4096) return -1;
    struct dp_ring *r=dp_find(8);if (!dp_space(r)) return -1;
    uint32_t *p=dp_words(r->offset+r->index*40);unsigned id=r->index+1;
    memset(p,0,40);p[0]=(419u<<1)|(36u<<10);p[1]=id|(1u<<16);
    p[2]=queue;p[3]=(queue>>32)|(1u<<18)|(ssn<4096 ? 1u<<26:0); /* BA window + optional SSN, no PN changes. */
    p[5]=(window==1 ? 1:window-1)|(ssn<4096 ? ssn<<11:0);
    dp_next(r);dp_publish(r);return id;
}
int wcn_dp_can_transmit(void) {
    if (!wcn_dp_ready() || !dp_space(dp_find(16))) return 0;
    for (unsigned i=0;i<256;i++) if (!d.tx_cookie[i]) return 1;
    return 0;
}
/* A VFS batch is serialized by the driver owner. Publish its accepted
 * prefix once, including partial writes, before returning to the caller. */
void wcn_dp_tx_batch_begin(void) { d.tx_batch=1; }
void wcn_dp_tx_batch_end(void) {
    if (d.tx_pending) dp_publish(dp_find(16));
    d.tx_pending=d.tx_batch=0;
}
int wcn_dp_transmit(const void *frame,unsigned bytes,unsigned lmac) {
    if (!wcn_dp_ready() || !frame || bytes<24 || bytes>RX_BYTES || lmac>1) return -1;
    struct dp_ring *r=dp_find(16);
    /* Both of these used to drop the frame silently, which reads as a dead
     * uplink with no trace at all. */
    if (!dp_space(r)) {
        d.tx_block_space++;
        if (d.tx_block_space<=4)
            wcn_log("wcn: TX blocked: ring full hw=%u idx=%u\n",dp_hw(r),r->index);
        return -1;
    }
    unsigned slot;
    for (slot=0;slot<256;slot++) if (!d.tx_cookie[slot]) break;
    if (slot==256) {
        d.tx_block_slot++;
        if (d.tx_block_slot<=4)
            wcn_log("wcn: TX blocked: no free cookie slot (completions missing)\n");
        return -1;
    }
    d.tx_generation=(d.tx_generation+1)&8191; if (!d.tx_generation) d.tx_generation=1;
    unsigned cookie=(d.tx_generation<<8)|slot;
    unsigned off=d.tx_buffers+slot*RX_BYTES;
    memcpy(d.dma+off,frame,bytes);
    uint64_t pa=d.pa+off;
    uint32_t *p=dp_words(r->offset+r->index*32);
    memset(p,0,32); p[0]=(324<<1)|(28<<10); p[1]=pa; p[2]=(pa>>32)|(3u<<8)|(cookie<<11);
    /* Native Wi-Fi, peer-map v1 address-Y lookup, vdev0 metadata. */
    p[3]=(1u<<2)|(2u<<14)|(1u<<16); p[4]=bytes;
    p[5]=(1u<<21)|(lmac<<26);
    d.tx_cookie[slot]=cookie; d.tx_packets++; dp_next(r);
    if (d.tx_batch) d.tx_pending=1;
    else dp_publish(r);
    return (int)cookie;
}

static int dp_rx_filter(unsigned ring_id, unsigned pdev) {
    /* HTT_H2T_MSG_TYPE_RX_RING_SELECTION_CFG: MAC2's RXDMA buffer-ring
     * filter defaults to pass-nothing, so EAPOL/data never reach the host
     * on 2.4G. ath11k-style station FP set. NOTE the TLV set must stay rich
     * (0x7BF): trimming it to the compact MPDU/MSDU list silently breaks
     * 5G (HE160) receive while 2.4G keeps working. */
    uint32_t p[13]={0};
    p[0]=0xc | (pdev<<8) | (ring_id<<16);
    p[1]=RX_BYTES;
    p[2]=0x09249249; /* mgmt FP: assoc/reassoc/probe/beacon/atim (0-9) */
    p[3]=0x00009249; /* mgmt FP: disassoc/auth/deauth/action (10-15) */
    p[4]=0x09249249; /* ctrl subtypes 0-9: FP */
    p[5]=0x01249249; /* ctrl 10-15 FP + mcast/ucast/null data FP */
    p[6]=0x000007BF; /* MPDU/MSDU start+end, packet, attention, PPDU TLVs */
    int rc=wcn_htc_htt_send(p,sizeof(p));
    /* rc != 0 keeps rx_filter_sent cleared and the message is retried; make
     * that visible, because without this filter MAC2 delivers no data frame
     * and every 4-way handshake ends in handshake-timeout. */
    wcn_diag("dp-rx-filter ring=%u pdev=%u tlv=%08x rc=%d rings=%u/%u\n",
            ring_id,pdev,p[6],rc,d.configured,d.count);
    return rc;
}
/* Record the joining band: the MAC2 RXDMA filter is firmware-global, so the
 * DP-setup pass only opens it when the station targets 2.4G. */
void wcn_dp_mac2_filter_req(int on) { mac2_filter_req = on ? 1 : 0; }

/* Which MAC's RX ring-selection filter to open. Each MAC's RXDMA buffer-ring
 * filter defaults to pass-nothing until its own selection config is installed,
 * so a 5G association that lands on MAC1 receives *nothing* while only MAC2
 * (the 2.4G path, opened by the 2.4G fix) is configured. The set is read from
 * /etc/wlan.filter ("1", "2" or "12") so the split can be A/B'd without a
 * rebuild; default opens both. */
static unsigned dp_filter_mask(void) {
    char value[16];
    unsigned n, mask = 0;
    FILE *fp = fopen("/etc/wlan.filter", "r");

    if (!fp) return 3;
    memset(value, 0, sizeof(value));
    n = fread(value, 1, sizeof(value) - 1, fp);
    fclose(fp);
    for (unsigned i = 0; i < n; i++) {
        if (value[i] == '1') mask |= 1;
        if (value[i] == '2') mask |= 2;
    }
    return mask ? mask : 3;
}

static int dp_rx_filter(unsigned ring_id, unsigned pdev);

void wcn_dp_poll(void) {
    if (!d.active || d.failed || !wcn_htt_ready()) return;
    /* Install rings only after READY, the DBS mode and the per-pdev country
     * update have been applied. Each of those reconfigures the radio, and the
     * firmware silently drops ring/FP state installed before it settles
     * (wcn_wmi.c: "Set DBS before HTT installs RXDMA rings"). */
    if (!wcn_wmi_radio_settled()) return;
    /* Configure one ring per turn, retaining ownership until stop disables DMA. */
    while (d.configured<d.count) {
        struct dp_ring *r=&d.rings[d.configured];
        if (!r->reg && dp_configure(r)) return;
        d.configured++;
        if (!r->reg) return;
    }
    if (!d.rx_filter_sent) {
        unsigned mask = dp_filter_mask();
        d.rx_filter_sent=1;
        /* Both MACs' RXDMA buffer-ring filters default to pass-nothing, so the
         * band that lands on an unopened MAC receives nothing at all: 2.4G
         * needed MAC2's, and a 5G association on MAC1 needs MAC1's. MAC1 goes
         * first so MAC2 stays the last-installed one if firmware keeps a
         * single selection slot. ath11k-exact station FP set; the TLV list
         * must stay rich (0x7BF) or 5G RX breaks. */
        wcn_diag("dp-filter-set mask=%u\n", mask);
        if ((mask & 1) && dp_rx_filter(0,1)) d.rx_filter_sent=0;
        if (d.rx_filter_sent && (mask & 2) && dp_rx_filter(0,2)) d.rx_filter_sent=0;
        return;
    }
    unsigned budget=64;
    for (unsigned i=0;i<d.count && budget && !d.failed;i++) {
        struct dp_ring *r=&d.rings[i];
        if (!r->dest) continue;
        unsigned head=dp_hw(r),taken=0;
        while (!d.failed && r->index*r->words!=head && budget) {
            const uint32_t *p=dp_words(r->offset+r->index*r->words*4);
            if (dp_complete(r,p)) break;
            dp_next(r); budget--; taken++; d.completions++;
        }
        if (taken) dp_publish(r);
    }
    if (!d.failed) dp_refill();
}
int wcn_dp_ready(void) { return d.active && !d.failed && d.configured==d.count; }
/* A programmed ring proves nothing. The bring-up can complete with
 * dp_cfg=22/22, dp_failed=0 and rx=0/0 forever, because the firmware never
 * writes a completion into the RXDMA ring. Management frames still flow
 * (they arrive as WMI events), so the only visible symptom is a 4-way
 * handshake where M1 never reaches the host. */
unsigned wcn_dp_rx_frames(void) { return d.rx_packets; }
unsigned wcn_dp_completions(void) { return d.completions; }
unsigned wcn_dp_tx_errors(void) { return d.tx_errors; }
/* ACKed uplink completions: the station liveness watchdog counts these as
 * proof of a working radio path when the downlink is silent. */
unsigned wcn_dp_tx_done(void) { return d.tx_done - d.tx_errors; }
unsigned wcn_dp_tx_packets(void) { return d.tx_packets; }
/* All TX completions, ACKed or failed: a stalled completion stream (firmware
 * stopped recycling descriptors) is detectable even when every frame fails. */
unsigned wcn_dp_tx_completions(void) { return d.tx_done; }

void wcn_dp_hw_state(char *out, unsigned size) {
    struct dp_ring *r = dp_find(128);
    if (out == NULL || !size) return;
    snprintf(out, size,
            "reo=%08x reo_c=%08x reo630=%08x rx_hp=%u rx_tp=%u rx_words=%u refill=%u hw=%u/%u",
            wcn_soc_read(REO), wcn_soc_read(REO+0xc), wcn_soc_read(REO+0x630),
            r ? dp_hw(r) : 0u, r ? r->index : 0u, r ? r->words : 0u,
            d.refilled, d.configured, d.count);
}
void wcn_dp_release(void) {
    if (d.dma) dma_free(0,(ewokos_addr_t)d.dma);
    memset(&d,0,sizeof(d));
}
void wcn_dp_status(char *out,size_t size) {
    snprintf(out,size,"dp=%u dp_cfg=%u/%u dp_failed=%u dp_bad=%u dp_completed=%u rx=%u/%u drop=%u stale=%u refill=%u rxfree=%u tx=%u/%u tx_errors=%u tx_last_source=%u tx_last_reason=%u tx_last_count=%u tx_last_info=%08x tx_last_rate=%08x",
             d.active,d.configured,d.count,d.failed,d.bad,d.completions,
             d.rx_delivered,d.rx_packets,d.rx_dropped,d.rx_stale,d.refilled,d.rx_free_count,d.tx_done,d.tx_packets,d.tx_errors,
             d.tx_last_source,d.tx_last_reason,d.tx_last_count,d.tx_last_info,d.tx_last_rate);
}
/* Appends at pos; returns size once the text no longer fits. */
static size_t dp_text(char *out,size_t size,size_t pos,const char *fmt,...) {
    if (pos>=size) return size;
    va_list ap; va_start(ap,fmt);
    int n=vsnprintf(out+pos,size-pos,fmt,ap);
    va_end(ap);
    return n<0 || (size_t)n>=size-pos ? size : pos+(size_t)n;
}
void wcn_dp_rings(char *out,size_t size) {
    int n=snprintf(out,size,"DP active=%u failed=%u bytes=%u\nPHY tx_type=%u tx_bw=%u tx_mcs=%u tx_gi=%u HT=%u VHT=%u HE=%u rx_type=%u rx_bw=%u rx_mcs=%u rx_gi=%u HT=%u VHT=%u HE=%u\n",
                   d.active,d.failed,d.used,(d.tx_rate>>3)&15,20u<<((d.tx_rate>>1)&3),(d.tx_rate>>11)&15,(d.tx_rate>>9)&3,d.tx_ht,d.tx_vht,d.tx_he,
                   (d.rx_rate>>8)&15,20u<<((d.rx_rate>>19)&3),(d.rx_rate>>15)&15,(d.rx_rate>>13)&3,d.rx_ht,d.rx_vht,d.rx_he);
    if (n<0 || (size_t)n>=size) return;
    size_t pos=n;
    pos=dp_text(out,size,pos,"RXDROP total=%u stale=%u reo_error=%u reo_partial=%u rxdma_error=%u wbm=%u not_done=%u attention=%u length=%u decap=%u\n",
                d.rx_dropped,d.drop_why[DROP_STALE],d.drop_why[DROP_REO_ERROR],d.drop_why[DROP_REO_PARTIAL],d.drop_why[DROP_RXDMA_ERROR],d.drop_why[DROP_WBM],
                d.drop_why[DROP_NOT_DONE],d.drop_why[DROP_ATTENTION],d.drop_why[DROP_LENGTH],d.drop_why[DROP_DECAP]);
    /* code:count pairs, codes per Linux hal_desc.h; attention pairs are bit:count. */
    pos=dp_text(out,size,pos,"RXDROP reo_code");
    for (unsigned i=0;i<32;i++) if (d.reo_err[i]) pos=dp_text(out,size,pos," %u:%u",i,d.reo_err[i]);
    pos=dp_text(out,size,pos,"\nRXDROP rxdma_code");
    for (unsigned i=0;i<32;i++) if (d.rxdma_err[i]) pos=dp_text(out,size,pos," %u:%u",i,d.rxdma_err[i]);
    pos=dp_text(out,size,pos,"\nRXDROP attention_bit");
    for (unsigned i=0;i<ATTN_ERRS;i++) if (d.attn_err[i]) pos=dp_text(out,size,pos," %u:%u",attn_bits[i],d.attn_err[i]);
    pos=dp_text(out,size,pos,"\n");
    pos=dp_text(out,size,pos,"RXWBM push=%u/%u/%u/%u done=%u fc_valid=%u mgmt=%u ctrl=%u data=%u retry=%u ampdu=%u encrypted=%u from_peer=%u queue_null=%u ast_miss=%u tcp_ack=%u small=%u decap=%u/%u/%u/%u\n",
                d.wbm.push[0],d.wbm.push[1],d.wbm.push[2],d.wbm.push[3],d.wbm.done,d.wbm.fc_valid,d.wbm.type[0],d.wbm.type[1],d.wbm.type[2],
                d.wbm.retry,d.wbm.ampdu,d.wbm.encrypted,d.wbm.from_peer,d.wbm.queue_null,d.wbm.ast_miss,d.wbm.tcp_ack,d.wbm.small,
                d.wbm.decap[0],d.wbm.decap[1],d.wbm.decap[2],d.wbm.decap[3]);
    /* seq: wbm_after_delivered wbm_after_wbm delivered_after_wbm delivered_after_delivered. */
    pos=dp_text(out,size,pos,"RXWBM seq once=%u unknown=%u wbm_dup=%u wbm_again=%u delivered_late=%u delivered_dup=%u\nRXWBM subtype",
                d.wbm.seq_once,d.wbm.seq_unknown,d.wbm.seq[1][0],d.wbm.seq[1][1],d.wbm.seq[0][1],d.wbm.seq[0][0]);
    for (unsigned i=0;i<3;i++) for (unsigned j=0;j<16;j++)
        if (d.wbm.subtype[i][j]) pos=dp_text(out,size,pos," %u.%u:%u",i,j,d.wbm.subtype[i][j]);
    pos=dp_text(out,size,pos,"\nRXWBM tid");
    for (unsigned i=0;i<17;i++) if (d.wbm.tid[i]) pos=dp_text(out,size,pos," %u:%u",i,d.wbm.tid[i]);
    pos=dp_text(out,size,pos,"\n");
    if (pos>=size) return;
    for (unsigned i=0;i<d.count;i++) {
        struct dp_ring *r=&d.rings[i];
        n=snprintf(out+pos,size-pos,"ring=%u %s entries=%u words=%u sw=%u hw=%u remote_hp=%u remote_tp=%u htt=%u/%u pdev=%u\n",
                   r->id,r->dest ? "dst" : "src",r->entries,r->words,r->index*r->words,
                   r->id==104 ? wcn_soc_read(r->ptr+4) : dp_hw(r),
                   *dp_words(r->dest ? r->hw_offset : r->sw_offset),
                   *dp_words(r->dest ? r->sw_offset : r->hw_offset),r->htt_type,r->htt_id,r->pdev);
        if (n<0 || (size_t)n>=size-pos) break;
        pos+=n;
    }
}

/* Crash collection is called after the DMA engine has stopped.  The
 * investigation branch predates this entry point, so keep the same ring
 * formatter available to the persistent MHI crash snapshot path. */
void wcn_dp_crash_rings(char *out,size_t size) {
    wcn_dp_rings(out,size);
}
