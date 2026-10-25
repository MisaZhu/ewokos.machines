#ifndef Q8B_NPU_DUMP_H
#define Q8B_NPU_DUMP_H
#include <stdint.h>
#include <q8b_npu.h>

#define Q8B_NPU_DUMP_PAGE 4096u
#define Q8B_NPU_DUMP_CHUNK 0x100000u
#define Q8B_NPU_DUMP_HEADER 32u
#define Q8B_NPU_DUMP_END UINT32_MAX

static inline int q8b_npu_dump_is_query(uint64_t op, uint64_t arg,
        uint64_t size) {
    return op == Q8B_NPU_DUMP_QUERY ||
            (op == Q8B_NPU_DUMP_QUERY_LEGACY && !arg && !size);
}

/* Capability discovery has no DSP access. Only older/unknown operation
 * replies may try the original opcode; busy/failure results must survive. */
static inline uint64_t q8b_npu_dump_query_abi(uint64_t (*query)(uint32_t)) {
    uint64_t result = query(Q8B_NPU_DUMP_QUERY);
    if ((int64_t)result == Q8B_NPU_PAS_UNSUPPORTED ||
            (int64_t)result == Q8B_NPU_PAS_INVALID)
        result = query(Q8B_NPU_DUMP_QUERY_LEGACY);
    return result;
}

static inline int q8b_npu_dump_allowed(int stopped_snapshot, int running,
        int boot_in_progress) {
    return stopped_snapshot && !running && !boot_in_progress;
}

static inline int q8b_npu_dump_range(uint64_t range) {
    uint32_t offset = (uint32_t)(range >> 32), size = (uint32_t)range;
    return size && size <= Q8B_NPU_DUMP_CHUNK &&
            !(size & (Q8B_NPU_DUMP_PAGE - 1u)) &&
            !(offset & (Q8B_NPU_DUMP_PAGE - 1u)) &&
            (uint64_t)offset + size <= Q8B_NSP0_SPAN;
}

static inline void q8b_npu_dump_le32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (i * 8));
}

/* Sparse zero-page omission only; omitted pages decode to zero. Every
 * nonzero page is stored as LE32 byte-offset + exactly4096 bytes, followed
 * by LE32 UINT32_MAX. The terminator distinguishes EOF from truncation. */
static inline void q8b_npu_dump_header(uint8_t *h) {
    const char magic[8] = {'Q','8','B','D','M','P','0','1'};
    for (unsigned i = 0; i < Q8B_NPU_DUMP_HEADER; i++) h[i] = 0;
    for (unsigned i = 0; i < 8; i++) h[i] = (uint8_t)magic[i];
    q8b_npu_dump_le32(h + 8, (uint32_t)Q8B_NSP0_CARVEOUT);
    q8b_npu_dump_le32(h + 12, (uint32_t)(Q8B_NSP0_CARVEOUT >> 32));
    q8b_npu_dump_le32(h + 16, (uint32_t)Q8B_NSP0_SPAN);
    q8b_npu_dump_le32(h + 20, Q8B_NPU_DUMP_PAGE);
    q8b_npu_dump_le32(h + 24, 1u); /* omitted pages are zero */
}

static inline int q8b_npu_dump_nonzero(const uint8_t *p) {
    for (unsigned i = 0; i < Q8B_NPU_DUMP_PAGE; i++)
        if (p[i]) return 1;
    return 0;
}
#endif
