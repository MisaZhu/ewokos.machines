/*
 * ga10b_pmem.c -- device-lifetime memory (see ga10b.h).
 *
 * One contiguous segment: a header page followed by a bump arena.  Nothing
 * is ever freed individually; the arena is dropped as a whole when the
 * hardware no longer references it (PMU RTOS not running).
 */
#include <string.h>
#include <ewoksys/klog.h>

#include "ga10b.h"

#define PMEM_HDR_SPAN  4096u

static struct {
    uint8_t* base;
    uint64_t phy;
} pm;

static void pmem_format(ga10b_pmem_hdr_t* h, uint64_t phy,
                        uint32_t owner_pid, uint32_t owner_uuid) {
    memset(h, 0, GA10B_PMEM_SIZE);
    h->version = GA10B_PMEM_VERSION;
    h->size = GA10B_PMEM_SIZE;
    h->cursor = PMEM_HDR_SPAN;
    h->phy_base = phy;
    h->owner_pid = owner_pid;
    h->owner_uuid = owner_uuid;
    h->magic = GA10B_PMEM_MAGIC;
}

ga10b_pmem_hdr_t* ga10b_pmem_hdr(void) {
    if (pm.base != NULL)
        return (ga10b_pmem_hdr_t*)pm.base;
    void* va = NULL;
    uint64_t phy = 0;
    int r = ga10b.hw ? ga10b_os_pmem_map(GA10B_PMEM_SIZE, &va, &phy)
                     : ga10b_dma_alloc(GA10B_PMEM_SIZE, 4096, &va, &phy);
    if (r != 0 || va == NULL) {
        klog("ga10b: device-lifetime memory unavailable\n");
        return NULL;
    }
    ga10b_pmem_hdr_t* h = va;
    if (h->magic != GA10B_PMEM_MAGIC || h->version != GA10B_PMEM_VERSION ||
        h->size != GA10B_PMEM_SIZE || h->phy_base != phy ||
        h->cursor < PMEM_HDR_SPAN || h->cursor > GA10B_PMEM_SIZE)
        pmem_format(h, phy, 0, 0);
    pm.base = va;
    pm.phy = phy;
    return h;
}

int ga10b_pmem_alloc(uint32_t size, uint32_t align, void** va, uint64_t* phy) {
    ga10b_pmem_hdr_t* h = ga10b_pmem_hdr();
    if (h == NULL || size == 0)
        return -1;
    if (align < 4096u)
        align = 4096u;
    uint64_t p = (pm.phy + h->cursor + align - 1u) & ~((uint64_t)align - 1u);
    uint64_t off = p - pm.phy;
    if (off + size > GA10B_PMEM_SIZE) {
        klog("ga10b: device-lifetime memory exhausted (%u at %llx)\n", size,
             (unsigned long long)off);
        return -1;
    }
    memset(pm.base + off, 0, size);
    h->cursor = (uint32_t)off + size;
    *va = pm.base + off;
    *phy = p;
    return 0;
}

uint32_t ga10b_pmem_off(const void* va) {
    return (uint32_t)((const uint8_t*)va - pm.base);
}

void* ga10b_pmem_va(uint32_t off, uint32_t size) {
    if (pm.base == NULL || off < PMEM_HDR_SPAN || size > GA10B_PMEM_SIZE ||
        off > GA10B_PMEM_SIZE - size)
        return NULL;
    return pm.base + off;
}

uint64_t ga10b_pmem_phy(uint32_t off) {
    return pm.phy + off;
}

void ga10b_pmem_reset(void) {
    ga10b_pmem_hdr_t* h = ga10b_pmem_hdr();
    if (h != NULL)
        pmem_format(h, pm.phy, h->owner_pid, h->owner_uuid);
}
