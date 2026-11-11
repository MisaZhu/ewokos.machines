/* Portable decoding of legacy and page-aligned Q8B FastRPC packets. */
#ifndef Q8B_FASTRPC_WIRE_H
#define Q8B_FASTRPC_WIRE_H

#include <stdint.h>
#include <string.h>
#include <q8b_npu.h>

/* The legacy packet has metadata after its 16-byte private prefix. The
 * optional format pads that prefix to a DSP page, keeping msg.addr aligned
 * as in Linux. Session bit31 is private to AP and never goes to the DSP. */
static inline uint32_t q8b_frpc_metadata_offset(const uint8_t *payload,
        uint32_t size) {
    uint32_t session;
    if (!payload || size < 16u) return 0;
    memcpy(&session, payload, sizeof(session));
    uint32_t offset = session & Q8B_NPU_FRPC_PACKET_PAGE ?
        Q8B_NPU_FRPC_METADATA_PAGE : 16u;
    return offset <= size ? offset : 0;
}

static inline int q8b_frpc_host_pointers(const uint8_t *payload) {
    uint32_t session;
    memcpy(&session, payload, sizeof(session));
    return !!(session & Q8B_NPU_FRPC_PACKET_HOST);
}

/* Check one virtual byte range against its exact DMA pages in one owned
 * mapping. A pointer in mapping A must never authorize pages from B. */
static inline int q8b_frpc_buffer_matches(uint64_t ptr, uint64_t len,
        uint64_t page, uint64_t page_size, uint64_t host_base,
        uint64_t dma_base, uint64_t dsp_base, uint64_t map_size) {
    uint64_t offset;
    if (!ptr || !len || !map_size || !dma_base || (dma_base & 4095u)) return 0;
    if (host_base && ptr >= host_base && ptr - host_base < map_size)
        offset = ptr - host_base;
    else if (ptr >= dma_base && ptr - dma_base < map_size)
        offset = ptr - dma_base;
    else if (dsp_base && ptr >= dsp_base && ptr - dsp_base < map_size)
        offset = ptr - dsp_base;
    else return 0;
    if (len > map_size - offset || offset > UINT64_MAX - dma_base) return 0;
    uint64_t byte = dma_base + offset;
    if ((ptr & 4095u) != (byte & 4095u) || len > UINT64_MAX - 8190u) return 0;
    uint64_t needed = ((byte & 4095u) + len + 4095u) & ~UINT64_C(4095);
    return page == (byte & ~UINT64_C(4095)) && page_size == needed;
}

/* The DSP echoes ctx; the low nibble is reserved for the PD. Keep the
 * counter outside resettable channel/session state so a delayed response
 * cannot complete a request from a later DSP boot. Preserve the existing
 * 32-bit sequence width and refuse exhaustion instead of reusing an ID. */
static inline int q8b_frpc_next_context(uint32_t *sequence, uint32_t pd,
        uint64_t *ctx) {
    if (!sequence || !ctx || *sequence == UINT32_MAX) return -1;
    *ctx = ((uint64_t)++*sequence << 4) | (pd & 1u);
    return 0;
}

/* fastrpc_invoke_send(..., kernel=true) uses client_id=0 in the wire
 * message.  The root request's first input still carries fl->client_id=1.
 * Q8B currently supports one user session and the INIT root handle only. */
static inline int32_t q8b_frpc_message_client_id(uint32_t handle) {
    return handle == 1u ? 0 : 1;
}

/* Stable DSP worker identities for this single-user transport.  The
 * listener's blocking next2 must not monopolize the application's worker. */
static inline int32_t q8b_frpc_message_tid(uint32_t handle, int listener) {
    return listener || handle == 3u ? 2 : 1;
}

/* Return an inline argument, not the page containing it.  pages[].addr is
 * page-aligned DMA backing; remote_buf.pv selects the bytes within it. */
static inline uint8_t *q8b_frpc_inline_arg(uint8_t *payload,
        uint64_t payload_iova, uint32_t payload_size, uint32_t index,
        uint32_t min_size) {
    uint32_t sc, count, meta;
    uint64_t ptr, len, offset;

    meta = q8b_frpc_metadata_offset(payload, payload_size);
    if (!meta) return 0;
    memcpy(&sc, payload + 12u, sizeof(sc));
    count = ((sc >> 16) & 0xffu) + ((sc >> 8) & 0xffu);
    if ((sc & 0xffu) || index >= count || count > 32u ||
            (uint64_t)meta + (uint64_t)count * 40u + 384u > payload_size)
        return 0;
    memcpy(&ptr, payload + meta + index * 16u, sizeof(ptr));
    memcpy(&len, payload + meta + 8u + index * 16u, sizeof(len));
    uint64_t pointer_base = q8b_frpc_host_pointers(payload) ?
        (uint64_t)(uintptr_t)payload : payload_iova;
    if (len < min_size || ptr < pointer_base)
        return 0;
    offset = ptr - pointer_base;
    if (offset < (uint64_t)meta + (uint64_t)count * 40u + 384u ||
            offset > payload_size || len > payload_size - offset)
        return 0;
    return payload + offset;
}

struct q8b_frpc_map_reply {
    uint64_t ap_addr;
    uint64_t size;
    uint64_t dsp_vaddr;
};

/* Called only after a successful DSP response.  INIT_MMAP (4) and
 * INIT_MEM_MAP (10) both pass their phy-page data as input argument 1. */
static inline int q8b_frpc_decode_map_reply(uint8_t *payload,
        uint64_t payload_iova, uint32_t payload_size,
        struct q8b_frpc_map_reply *reply) {
    uint32_t handle, sc, method, rsp_index, num;
    int32_t client_id;
    uint8_t *req, *page, *rsp;

    if (!payload || !reply || payload_size < 16u) return -1;
    memcpy(&handle, payload + 8u, sizeof(handle));
    memcpy(&sc, payload + 12u, sizeof(sc));
    method = (sc >> 24) & 0x1fu;
    if (handle != 1u || (sc & 0xffu) ||
            !((method == 4u && ((sc >> 8) & 0xffffu) == 0x0201u) ||
              (method == 10u && ((sc >> 8) & 0xffffu) == 0x0301u)))
        return -1;
    rsp_index = method == 10u ? 3u : 2u;
    req = q8b_frpc_inline_arg(payload, payload_iova, payload_size,
            0, method == 10u ? 32u : 24u);
    page = q8b_frpc_inline_arg(payload, payload_iova, payload_size, 1, 16u);
    rsp = q8b_frpc_inline_arg(payload, payload_iova, payload_size, rsp_index, 8u);
    if (!req || !page || !rsp) return -1;
    memcpy(&client_id, req, sizeof(client_id));
    memcpy(&num, req + (method == 10u ? 24u : 16u), sizeof(num));
    if (client_id != 1 || num != 16u) return -1;
    memcpy(&reply->ap_addr, page, sizeof(reply->ap_addr));
    memcpy(&reply->size, page + 8u, sizeof(reply->size));
    memcpy(&reply->dsp_vaddr, rsp, sizeof(reply->dsp_vaddr));
    if (!reply->size || !reply->dsp_vaddr ||
            reply->dsp_vaddr > UINT64_MAX - reply->size)
        return -1;
    return 0;
}

#endif
