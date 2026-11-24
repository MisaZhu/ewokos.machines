/*
 * host_shim.c -- OS-layer shim for the host integration dry-run.
 *
 * Compiles the REAL driver objects (ga10b_gr.c / ga10b_fifo.c /
 * ga10b_mm.c / ga10b_ce.c / ga10b_fw_parse.c) on the host.  Everything
 * below mimics the documented ga10bd dry-run semantics exactly:
 *   ga10b_power.c: rd -> log + 0, wr -> log only, poll -> immediate 0,
 *                  wr_mask = (rd & ~mask) | (val & mask)
 *   ga10b_fw.c:    zeroed page-aligned DMA buffers (fake phys addresses),
 *                  firmware read from the real blob directory
 *   device_table:  TOP reads 0 -> count 0 (dry-run fallback paths run)
 *   acr:           LS bootstrap logged, returns success
 * MMIO writes are additionally captured in shim_trace[] for assertions.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ga10b.h"
#include "ga10b_regs.h"
#include "host_shim.h"

ga10b_dev_t ga10b;

shim_wr_t shim_trace[SHIM_TRACE_CAP];
uint32_t shim_nwr;
int shim_quiet = 1;
uint32_t shim_dma_live;

int klog(const char* fmt, ...) {
    if (shim_quiet)
        return 0;
    va_list ap;
    va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

void shim_reset(void) { shim_nwr = 0; }

int shim_find_from(uint32_t from, uint32_t off, uint32_t val) {
    for (uint32_t i = from; i < shim_nwr; i++)
        if (shim_trace[i].off == off && shim_trace[i].val == val)
            return (int)i;
    return -1;
}

uint32_t shim_count_off(uint32_t off) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < shim_nwr; i++)
        if (shim_trace[i].off == off)
            n++;
    return n;
}

/* ---- MMIO (dry-run semantics verbatim from ga10b_power.c) ---- */
uint32_t ga10b_rd(uint32_t off) {
    if (!ga10b.hw || !ga10b.bar0) {
        klog("ga10b: [dry] rd  %08x\n", off);
        return 0;
    }
    return ga10b.bar0[off / 4];
}

void ga10b_wr(uint32_t off, uint32_t val) {
    if (shim_nwr < SHIM_TRACE_CAP) {
        shim_trace[shim_nwr].off = off;
        shim_trace[shim_nwr].val = val;
        shim_nwr++;
    }
    if (!ga10b.hw || !ga10b.bar0) {
        klog("ga10b: [dry] wr  %08x = %08x\n", off, val);
        return;
    }
    ga10b.bar0[off / 4] = val;
}

void ga10b_wr_mask(uint32_t off, uint32_t mask, uint32_t val) {
    ga10b_wr(off, (ga10b_rd(off) & ~mask) | (val & mask));
}

int ga10b_poll(uint32_t off, uint32_t mask, uint32_t want, uint32_t tries) {
    if (!ga10b.hw)
        return 0;
    for (uint32_t i = 0; i < tries; i++)
        if ((ga10b_rd(off) & mask) == want)
            return 0;
    klog("ga10b: poll %08x mask %08x want %08x TIMEOUT\n", off, mask, want);
    return -1;
}

int ga10b_map(void) { return 0; }
void ga10b_delay_us(uint32_t us) { (void)us; }

/* ---- power-layer stubs (dry-run outcomes) ---- */
int ga10b_device_table(ga10b_devtable_t* t) {
    memset(t, 0, sizeof(*t));   /* TOP cfg reads 0 -> n=0, same as dry-run */
    return 0;
}

int ga10b_engine_enable(uint32_t reset_id, int enable) {
    klog("ga10b: [shim] engine_enable %u = %d\n", reset_id, enable);
    return 0;
}

int ga10b_acr_bootstrap_ls(uint32_t falcon_id) {
    klog("ga10b: [shim] acr_bootstrap_ls falcon %u\n", falcon_id);
    return 0;
}

/* ---- DMA pool: zeroed aligned host memory, fake phys, leak tracking ---- */
#define SHIM_DMA_MAX 512
static struct { void* va; uint64_t phy; uint32_t size; } shim_dma[SHIM_DMA_MAX];
static uint64_t shim_dma_phy = 0x100000000ULL;

int ga10b_dma_alloc(uint32_t size, uint32_t align, void** va, uint64_t* phy) {
    if (size == 0)
        return -1;
    if (align < 4096u)          /* ga10b_fw.c rounds sub-page align up */
        align = 4096u;
    void* p = NULL;
    if (posix_memalign(&p, align, size) != 0)
        return -1;
    memset(p, 0, size);
    for (uint32_t i = 0; i < SHIM_DMA_MAX; i++) {
        if (shim_dma[i].va == NULL) {
            shim_dma[i].va = p;
            shim_dma[i].phy = shim_dma_phy;
            shim_dma[i].size = size;
            *va = p;
            *phy = shim_dma_phy;
            shim_dma_phy += (size + 4095u) & ~4095u;
            shim_dma_live++;
            return 0;
        }
    }
    free(p);
    return -1;
}

void* shim_dma_va(uint64_t phy) {
    for (uint32_t i = 0; i < SHIM_DMA_MAX; i++)
        if (shim_dma[i].va != NULL && shim_dma[i].phy == phy)
            return shim_dma[i].va;
    return NULL;
}

void ga10b_dma_free(void* va) {
    if (va == NULL)
        return;
    for (uint32_t i = 0; i < SHIM_DMA_MAX; i++) {
        if (shim_dma[i].va == va) {
            shim_dma[i].va = NULL;
            shim_dma_live--;
            free(va);
            return;
        }
    }
    klog("ga10b: [shim] dma_free of unknown pointer %p\n", va);
}

/* ---- firmware: real blobs from the staging dir ---- */
int ga10b_fw_load(const char* dir, const char* name, ga10b_fw_t* out) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        klog("ga10b: [shim] fw missing %s\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return -1; }
    void* va;
    uint64_t phy;
    if (ga10b_dma_alloc((uint32_t)len, 4096, &va, &phy) != 0) {
        fclose(f);
        return -1;
    }
    size_t got = fread(va, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) {
        ga10b_dma_free(va);
        return -1;
    }
    out->va = va;
    out->phy = phy;
    out->size = (uint32_t)len;
    return 0;
}

void ga10b_fw_free(ga10b_fw_t* fw) {
    ga10b_dma_free(fw->va);
    memset(fw, 0, sizeof(*fw));
}