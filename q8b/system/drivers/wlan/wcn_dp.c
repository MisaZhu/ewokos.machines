/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/* WCN6855 SRNG/HTT reference: Linux ath11k/hal.c, hw.c, dp.c, dp_tx.c.
 * Copyright (c) 2018-2019 The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. */
#include "wcn_dp.h"
#include "wcn_mhi.h"
#include "wcn_htc.h"
#include "wcn_htt.h"
#include "wcn_sta.h"
#include "wcn_wpa.h"
#include <ewoksys/dma.h>
#include <ewoksys/klog.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DP_BYTES (16u * 1024 * 1024)
#define WBM 0xa34000u
#define REO 0xa38000u
#define TCL 0xa44000u
#define LINK_COUNT 4096u
#define RX_COUNT 1024u
#define RX_BYTES 2048u
#define MAX_RINGS 24u

struct dp_ring {
    unsigned id, reg, ptr, entries, words, dest, offset, hw_offset, sw_offset;
    unsigned index, loop, consumed, htt_type, htt_id, pdev;
};
static struct {
    uint8_t *dma;
    uint64_t pa;
    unsigned used, active, failed, count, configured, bad, completions;
    unsigned links, rx_buffers, tx_buffers;
    unsigned rx_packets,rx_delivered,rx_dropped,refilled,tx_packets,tx_done,tx_errors,tx_generation;
    unsigned tx_batch,tx_pending;
    unsigned tx_cookie[256];
    unsigned rx_rate,tx_rate,rx_ht,rx_vht,rx_he,tx_ht,tx_vht,tx_he;
    uint8_t rx_owner[RX_COUNT];
    struct dp_ring rings[MAX_RINGS];
} d;
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
    if (dp_lmac(128,RX_COUNT,2,0,2,5,0)) return -1;
    for (unsigned i=0;i<2;i++) {
        /* WCN6855 has 15 IDs per LMAC. FW fills each MAC's RX buffer ring. */
        if (dp_lmac(129+i*15,1024,2,0,1,0,i+1) ||
            dp_lmac(133+i*15,512,8,1,0,7,i+1) ||
            dp_lmac(132+i*15,128,2,0,1,1,i+1)) return -1;
    }
    d.links=dp_reserve(LINK_COUNT*128);
    d.rx_buffers=dp_reserve((RX_COUNT-1)*RX_BYTES);
    d.tx_buffers=dp_reserve(256*RX_BYTES);
    if (!d.links || !d.rx_buffers || !d.tx_buffers) return -1;
    for (unsigned i=1;i<RX_COUNT;i++) d.rx_owner[i]=2;
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
    wcn_soc_write(REO,wcn_soc_read(REO)|12);
    wcn_soc_write(REO+0x630,wcn_soc_read(REO+0x630)&~(15u<<17));
    for (unsigned i=0;i<4;i++) wcn_soc_write(REO+0x5bc+i*4,100000);
    /* Every hash maps to REO2SW1. The remaining rings are still valid. */
    wcn_soc_write(REO+0xc,0x11111111); wcn_soc_write(REO+0x10,0x11111111);
    d.active=1;
    klog("wcn: DP rings=%u bytes=%u pa=%llx\n",d.count,d.used,(unsigned long long)d.pa);
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
    klog("wcn: HTT ring id=%u pdev=%u type=%u words=%u\n",r->id,r->pdev,r->htt_type,r->entries*r->words);
    return 0;
}

static struct dp_ring *dp_find(unsigned id) {
    for (unsigned i=0;i<d.count;i++) if (d.rings[i].id==id) return &d.rings[i];
    return NULL;
}
static int dp_fault(const char *why) {
    d.bad++; d.failed=1; klog("wcn: DP invalid %s; stop/start required\n",why); return -1;
}
static unsigned dp_space(struct dp_ring *r) {
    unsigned tail=dp_hw(r)/r->words;
    return d.failed ? 0 : (tail+r->entries-r->index-1)%r->entries;
}
static void dp_next(struct dp_ring *r) { r->index=(r->index+1)%r->entries; }
static uint64_t dp_address(const uint32_t *p) { return p[0]|((uint64_t)(p[1]&255)<<32); }
static int dp_rx_id(const uint32_t *p) {
    unsigned id=p[1]>>11;
    if (!id || id>=RX_COUNT || dp_address(p)!=d.pa+d.rx_buffers+(id-1)*RX_BYTES ||
        d.rx_owner[id]!=1) return dp_fault("RX address/cookie/owner");
    return (int)id;
}
static void dp_refill(void) {
    struct dp_ring *r=dp_find(128);
    unsigned space=dp_space(r),posted=0;
    if (!space) return;
    for (unsigned i=1;i<RX_COUNT && space;i++) if (d.rx_owner[i]==2) {
        unsigned off=d.rx_buffers+(i-1)*RX_BYTES;
        memset(d.dma+off,0,RX_BYTES);
        uint64_t pa=d.pa+off;
        uint32_t *p=dp_words(r->offset+r->index*8);
        p[0]=pa; p[1]=(pa>>32)|(4u<<8)|(i<<11);
        d.rx_owner[i]=1; dp_next(r); space--; posted++;
    }
    if (posted) { dp_publish(r); d.refilled+=posted; }
}
static void dp_rx_buffer(unsigned id,unsigned deliver) {
    const uint32_t *p=dp_words(d.rx_buffers+(id-1)*RX_BYTES);
    const uint8_t *b=(const void *)p;
    d.rx_packets++;
    /* WCN6855 RX descriptor is 392 bytes, per Linux rx_desc.h. */
    unsigned len=p[26]&0x3fff,pad=(p[11]>>26)&3,decap=(p[27]>>8)&3;
    unsigned err=p[22]&((1u<<12)|(1u<<13)|(1u<<16)|(1u<<17)|(1u<<26)|(1u<<27)|(15u<<28));
    if (deliver && (p[23]>>31) && !err && len>=14 && len<=RX_BYTES-392-pad && decap==2) {
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
        d.rx_dropped++;
        if (d.rx_dropped<=8) klog("wcn: RX drop len=%u pad=%u decap=%u attention=%x/%x deliver=%u\n",len,pad,decap,p[22],p[23],deliver);
    }
    d.rx_owner[id]=2;
}
/* Validate a complete bounded link chain before returning any child buffers. */
static int dp_link_drop(const uint32_t *entry) {
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
            for (unsigned k=0;k<nids;k++) if (ids[k]==(unsigned)id) return dp_fault("duplicate RX link buffer");
            ids[nids++]=id;
        }
        address[0]=q[1]; address[1]=q[2];
    }
    struct dp_ring *r=dp_find(105);
    if (dp_space(r)<count) return 1; /* Retain destination until release space exists. */
    for (unsigned i=0;i<nids;i++) dp_rx_buffer(ids[i],0);
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
        if (p[7]&(1u<<8)) return dp_link_drop(p);
        int id=dp_rx_id(p); if (id<0) return -1;
        dp_rx_buffer(id,((p[7]>>9)&3)==1 && !(p[4]&((1u<<2)|(1u<<22))));
    } else if (r->id==133 || r->id==148) return dp_link_drop(p);
    else if (r->id==109) {
        if (((p[2]>>6)&7)==1) return dp_link_drop(p);
        if (((p[2]>>6)&7)!=0) return dp_fault("WBM RX descriptor type");
        int id=dp_rx_id(p); if (id<0) return -1;
        dp_rx_buffer(id,0);
    } else if (r->id==106) {
        unsigned cookie=p[1]>>11,slot=cookie&255;
        if (!cookie || d.tx_cookie[slot]!=cookie || dp_address(p)!=d.pa+d.tx_buffers+slot*RX_BYTES)
            return dp_fault("TX completion address/cookie");
        d.tx_cookie[slot]=0; d.tx_done++;
        unsigned src=p[2]&7;
        unsigned success=(src==0 && !((p[2]>>13)&15)) || (src==3 && !((p[2]>>9)&15));
        if (!success) d.tx_errors++;
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
    if (!dp_space(r)) return -1;
    unsigned slot;
    for (slot=0;slot<256;slot++) if (!d.tx_cookie[slot]) break;
    if (slot==256) return -1;
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

void wcn_dp_poll(void) {
    if (!d.active || d.failed || !wcn_htt_ready()) return;
    /* Configure one ring per turn, retaining ownership until stop disables DMA. */
    while (d.configured<d.count) {
        struct dp_ring *r=&d.rings[d.configured];
        if (!r->reg && dp_configure(r)) return;
        d.configured++;
        if (!r->reg) return;
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
void wcn_dp_release(void) {
    if (d.dma) dma_free(0,(ewokos_addr_t)d.dma);
    memset(&d,0,sizeof(d));
}
void wcn_dp_status(char *out,size_t size) {
    snprintf(out,size,"dp=%u dp_cfg=%u/%u dp_failed=%u dp_bad=%u dp_completed=%u rx=%u/%u drop=%u refill=%u tx=%u/%u tx_errors=%u",
             d.active,d.configured,d.count,d.failed,d.bad,d.completions,
             d.rx_delivered,d.rx_packets,d.rx_dropped,d.refilled,d.tx_done,d.tx_packets,d.tx_errors);
}
void wcn_dp_rings(char *out,size_t size) {
    int n=snprintf(out,size,"DP active=%u failed=%u bytes=%u\nPHY tx_type=%u tx_bw=%u tx_mcs=%u tx_gi=%u HT=%u VHT=%u HE=%u rx_type=%u rx_bw=%u rx_mcs=%u rx_gi=%u HT=%u VHT=%u HE=%u\n",
                   d.active,d.failed,d.used,(d.tx_rate>>3)&15,20u<<((d.tx_rate>>1)&3),(d.tx_rate>>11)&15,(d.tx_rate>>9)&3,d.tx_ht,d.tx_vht,d.tx_he,
                   (d.rx_rate>>8)&15,20u<<((d.rx_rate>>19)&3),(d.rx_rate>>15)&15,(d.rx_rate>>13)&3,d.rx_ht,d.rx_vht,d.rx_he);
    if (n<0 || (size_t)n>=size) return;
    size_t pos=n;
    for (unsigned i=0;i<d.count;i++) {
        struct dp_ring *r=&d.rings[i];
        n=snprintf(out+pos,size-pos,"ring=%u %s entries=%u words=%u sw=%u hw=%u remote_hp=%u remote_tp=%u\n",
                   r->id,r->dest ? "dst" : "src",r->entries,r->words,r->index*r->words,
                   r->id==104 ? wcn_soc_read(r->ptr+4) : dp_hw(r),
                   *dp_words(r->dest ? r->hw_offset : r->sw_offset),
                   *dp_words(r->dest ? r->sw_offset : r->hw_offset));
        if (n<0 || (size_t)n>=size-pos) break;
        pos+=n;
    }
}
