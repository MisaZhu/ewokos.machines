#ifndef GA10B_NETLIST_H
#define GA10B_NETLIST_H

/*
 * NETC netlist region ids + typed list accessors -- pure logic,
 * host-unit-testable against the real NETC blob.
 *
 * VERIFIED against nvgpu rel-36 common/netlist/netlist.c + netlist_priv.h:
 * container = {u32 version=0; u32 regions} then {id, data_size, data_offset}
 * x regions; payload at image_base+offset is raw little-endian u32 words
 * (no transforms).  List element layouts (include/nvgpu/netlist.h):
 *   av   = {addr, value}          2 words/entry
 *   aiv  = {addr, index, value}   3 words/entry
 *   av64 = {addr, value_lo, value_hi} 3 words/entry
 * MAJORV / BUFFER_SIZE / CTXSW_REG_BASE_INDEX / NETLIST_NUM are single u32.
 * Unknown regions (debugger etc.) are simply never requested.
 *
 * Container location/bounds checking is ga10b_fw_netlist_find
 * (ga10b_fw_parse.c); this layer adds word alignment + element typing.
 */

#include <stdint.h>
#include "ga10b_fw.h"

/* region ids (netlist_priv.h enum netlist_region_id) */
#define GA10B_NETC_SW_BUNDLE_INIT        4u
#define GA10B_NETC_SW_CTX_LOAD           5u
#define GA10B_NETC_SW_NON_CTX_LOAD       6u
#define GA10B_NETC_SW_METHOD_INIT        7u
#define GA10B_NETC_MAJORV                15u
#define GA10B_NETC_BUFFER_SIZE           16u
#define GA10B_NETC_REG_BASE_INDEX        17u
#define GA10B_NETC_NETLIST_NUM           18u
#define GA10B_NETC_SW_VEID_BUNDLE_INIT   28u
#define GA10B_NETC_SW_BUNDLE64_INIT      34u
#define GA10B_NETC_NON_CTX_LOCAL_COMPUTE 48u
#define GA10B_NETC_NON_CTX_LOCAL_GFX     49u
#define GA10B_NETC_NON_CTX_GLOBAL_COMPUTE 50u
#define GA10B_NETC_NON_CTX_GLOBAL_GFX    51u

/* words view of a region; fails if absent/misaligned/empty */
static inline int ga10b_netlist_words(const void* img, uint32_t size,
                                      uint32_t id, const uint32_t** w,
                                      uint32_t* nwords) {
    uint32_t off, sz;
    if (ga10b_fw_netlist_find(img, size, id, &off, &sz) != 0)
        return -1;
    if ((off & 3u) != 0 || (sz & 3u) != 0)
        return -1;
    *w = (const uint32_t*)((const uint8_t*)img + off);
    *nwords = sz / 4u;
    return 0;
}

/* single-u32 region (MAJORV, BUFFER_SIZE, ...) */
static inline int ga10b_netlist_u32(const void* img, uint32_t size,
                                    uint32_t id, uint32_t* val) {
    const uint32_t* w;
    uint32_t n;
    if (ga10b_netlist_words(img, size, id, &w, &n) != 0 || n != 1u)
        return -1;
    *val = w[0];
    return 0;
}

/* entry count of a typed list; 0 when the region is absent */
static inline uint32_t ga10b_netlist_count(const void* img, uint32_t size,
                                           uint32_t id, uint32_t elem_words) {
    const uint32_t* w;
    uint32_t n;
    if (ga10b_netlist_words(img, size, id, &w, &n) != 0)
        return 0;
    return n / elem_words;
}

static inline int ga10b_netlist_av(const void* img, uint32_t size,
                                   uint32_t id, uint32_t idx,
                                   uint32_t* addr, uint32_t* val) {
    const uint32_t* w;
    uint32_t n;
    if (ga10b_netlist_words(img, size, id, &w, &n) != 0 || idx >= n / 2u)
        return -1;
    *addr = w[2u * idx];
    *val  = w[2u * idx + 1u];
    return 0;
}

static inline int ga10b_netlist_aiv(const void* img, uint32_t size,
                                    uint32_t id, uint32_t idx,
                                    uint32_t* addr, uint32_t* index,
                                    uint32_t* val) {
    const uint32_t* w;
    uint32_t n;
    if (ga10b_netlist_words(img, size, id, &w, &n) != 0 || idx >= n / 3u)
        return -1;
    *addr  = w[3u * idx];
    *index = w[3u * idx + 1u];
    *val   = w[3u * idx + 2u];
    return 0;
}

static inline int ga10b_netlist_av64(const void* img, uint32_t size,
                                     uint32_t id, uint32_t idx,
                                     uint32_t* addr, uint32_t* lo,
                                     uint32_t* hi) {
    const uint32_t* w;
    uint32_t n;
    if (ga10b_netlist_words(img, size, id, &w, &n) != 0 || idx >= n / 3u)
        return -1;
    *addr = w[3u * idx];
    *lo   = w[3u * idx + 1u];
    *hi   = w[3u * idx + 2u];
    return 0;
}

#endif