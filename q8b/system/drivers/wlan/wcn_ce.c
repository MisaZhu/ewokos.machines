/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/* WCN6855 copy-engine rings. Register/protocol reference: Linux ath11k
 * hal.c, hw.c, ce.c and htc.c, Copyright (c) 2018-2019 The Linux Foundation,
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * Single controller owner, noncached DMA, completion polling. */
#include "wcn_ce.h"
#include "wcn_mhi.h"
#include "wcn_htc.h"
#include <ewoksys/dma.h>
#include <ewoksys/klog.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ARENA_SIZE (8u * 1024 * 1024)
#define CE_BASE 0x1b80000u
struct ring {
    uint32_t reg, ptr_reg, offset, entries, words, index, loop;
    volatile uint32_t *hw_ptr;
};
struct pipe {
    struct ring src, dst, status;
    unsigned buffer, bytes, rx_index;
};
static struct {
    uint8_t *dma;
    uint64_t pa;
    unsigned used, active, rx, tx, bad;
    struct pipe pipe[9];
} c;

static void barrier(void) { __asm__ volatile("dsb sy" ::: "memory"); }
static unsigned reserve(unsigned bytes) {
    unsigned pos = (c.used + 63) & ~63u;
    if (pos > ARENA_SIZE || bytes > ARENA_SIZE - pos) return 0;
    c.used = pos + bytes;
    return pos;
}

static int init_ring(struct ring *r, unsigned id, unsigned kind, unsigned entries) {
    unsigned base = CE_BASE + id * 0x2000 + (kind ? 0x1000 : 0);
    r->reg = base + (kind == 2 ? 0x58 : 0);
    r->ptr_reg = base + (kind == 2 ? 0x408 : 0x400);
    r->entries = entries;
    r->words = kind == 1 ? 2 : 4;
    r->loop = 1;
    r->offset = reserve(entries * r->words * 4);
    unsigned ptr = reserve(4);
    if (!r->offset || !ptr) return -1;
    r->hw_ptr = (volatile uint32_t *)(c.dma + ptr);
    uint64_t pa = c.pa + r->offset;
    wcn_soc_write(r->reg + 0x10, 0);
    wcn_soc_write(r->reg, (uint32_t)pa);
    wcn_soc_write(r->reg + 4, ((uint32_t)(pa >> 32) & 255) | (entries * r->words << 8));
    wcn_soc_write(r->reg + 8, r->words | (kind == 2 ? (80 + id) << 8 : 0));
    unsigned ptr_offset = kind == 2 ? 0x14 : 0x1c;
    wcn_soc_write(r->reg + ptr_offset, (uint32_t)(c.pa + ptr));
    wcn_soc_write(r->reg + ptr_offset + 4, 0);
    wcn_soc_write(r->reg + (kind == 2 ? 0x24 : 0x30), 0);
    if (kind != 2) wcn_soc_write(r->reg + 0x34, 0);
    /* No MSI for CE rings: hardware writes progress into the DMA word. */
    wcn_soc_write(r->reg + 0x48, 0);
    wcn_soc_write(r->reg + 0x4c, 0);
    wcn_soc_write(r->ptr_reg, 0);
    wcn_soc_write(r->ptr_reg + 4, 0);
    barrier();
    wcn_soc_write(r->reg + 0x10, kind == 2 ? 0x40 : 0x42);
    return 0;
}

static void post_rx(struct pipe *p, unsigned buffer) {
    struct ring *r = &p->dst;
    uint32_t *d = (uint32_t *)(c.dma + r->offset) + r->index * 2;
    uint64_t pa = c.pa + p->buffer + buffer * p->bytes;
    d[0] = pa; d[1] = pa >> 32;
    r->index = (r->index + 1) % r->entries;
    barrier();
    wcn_soc_write(r->ptr_reg, r->index * r->words);
}

int wcn_ce_init(void) {
    if (c.active) return 0;
    if (c.dma) return -1;
    c.dma = (uint8_t *)dma_alloc(0, ARENA_SIZE);
    if (!c.dma) return -1;
    c.pa = dma_phy_addr(0, (ewokos_addr_t)c.dma);
    if (!c.pa || c.pa + ARENA_SIZE > 0x100000000ULL) return -1;
    memset(c.dma, 0, ARENA_SIZE); c.used = 64;
    const unsigned tx_entries[9] = {16,0,0,32,2048,0,0,32,0};
    for (unsigned i = 0; i < 9; i++) {
        struct pipe *p = &c.pipe[i];
        p->bytes = i == 4 ? 256 : 2048;
        if (tx_entries[i]) {
            if (init_ring(&p->src, i, 0, tx_entries[i])) return -1;
            p->buffer = reserve(tx_entries[i] * p->bytes);
            if (!p->buffer) return -1;
        } else if (i == 1 || i == 2 || i == 5) {
            if (init_ring(&p->dst, i, 1, 512) || init_ring(&p->status, i, 2, 512)) return -1;
            p->buffer = reserve(512 * p->bytes);
            if (!p->buffer) return -1;
            uint32_t ctrl = wcn_soc_read(CE_BASE + i * 0x2000 + 0x10b0);
            wcn_soc_write(CE_BASE + i * 0x2000 + 0x10b0, (ctrl & ~0xffffu) | p->bytes);
            for (unsigned j = 0; j < 511; j++) post_rx(p, j);
        }
    }
    c.active = 1;
    wcn_htc_reset();
    klog("wcn: CE initialized pa=%llx bytes=%u\n", (unsigned long long)c.pa, c.used);
    return 0;
}

int wcn_ce_send(unsigned pipe, unsigned endpoint, const void *packet, unsigned bytes) {
    if (!c.active || pipe >= 9 || !packet || !bytes) return -1;
    struct pipe *p = &c.pipe[pipe];
    struct ring *r = &p->src;
    if (!r->entries || bytes > p->bytes) return -1;
    unsigned tp = *r->hw_ptr;
    barrier();
    if (tp >= r->entries * r->words || tp % r->words) return -1;
    unsigned next = (r->index + 1) % r->entries;
    if (next * r->words == tp) return -1;
    unsigned offset = p->buffer + r->index * p->bytes;
    memcpy(c.dma + offset, packet, bytes);
    uint32_t *d = (uint32_t *)(c.dma + r->offset) + r->index * 4;
    uint64_t pa = c.pa + offset;
    d[0] = pa; d[1] = (pa >> 32) | (bytes << 16);
    d[2] = endpoint; d[3] = 0;
    r->index = next;
    barrier();
    wcn_soc_write(r->ptr_reg, next * r->words);
    c.tx++;
    return 0;
}

void wcn_ce_poll(void) {
    if (!c.active) return;
    for (unsigned i = 0; i < 9; i++) {
        struct pipe *p = &c.pipe[i];
        struct ring *r = &p->status;
        if (!r->entries) continue;
        unsigned hp = *r->hw_ptr;
        barrier();
        if (hp >= r->entries * r->words || hp % r->words) { c.bad++; continue; }
        unsigned budget = r->entries;
        while (r->index * r->words != hp && budget--) {
            uint32_t *d = (uint32_t *)(c.dma + r->offset) + r->index * r->words;
            if ((d[3] >> 28) != r->loop) break;
            unsigned length = d[0] >> 16;
            if (length && length <= p->bytes) {
                c.rx++;
                wcn_htc_receive(i, c.dma + p->buffer + p->rx_index * p->bytes, length);
            } else c.bad++;
            /* The buffer just consumed is recycled at the source producer. */
            post_rx(p, p->rx_index);
            p->rx_index = (p->rx_index + 1) % (p->dst.entries - 1);
            if (++r->index == r->entries) { r->index = 0; r->loop = (r->loop + 1) & 15; }
            barrier();
            wcn_soc_write(r->ptr_reg + 4, r->index * r->words);
        }
    }
    wcn_htc_poll();
}

void wcn_ce_release(void) {
    if (c.dma) dma_free(0, (ewokos_addr_t)c.dma);
    memset(&c, 0, sizeof(c));
    wcn_htc_reset();
}
void wcn_ce_status(char *out, size_t size) {
    char htc[448];
    wcn_htc_status(htc, sizeof(htc));
    snprintf(out, size, "ce=%u ce_rx=%u ce_tx=%u ce_bad=%u %s",
             c.active, c.rx, c.tx, c.bad, htc);
}
