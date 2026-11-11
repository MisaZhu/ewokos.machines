#ifndef Q8B_MDT_LOAD_H
#define Q8B_MDT_LOAD_H
#include <stdint.h>

typedef struct {
    uint32_t p_type, p_offset, p_vaddr, p_paddr;
    uint32_t p_filesz, p_memsz, p_flags, p_align;
} q8b_mdt_phdr;

/* drivers/soc/qcom/mdt_loader.c:mdt_phdr_loadable(). */
static inline int q8b_mdt_loadable(const q8b_mdt_phdr *p) {
    return p->p_type == 1u && p->p_memsz &&
            (p->p_flags & 0x07000000u) != 0x02000000u;
}

/* Validate every segment before PAS_INIT or any carveout writes. Use wide
 * additions: the ELF32 fields can wrap even though the fixed carveout cannot. */
static inline int q8b_mdt_validate_loads(uint32_t image_size,
        const q8b_mdt_phdr *ph, uint32_t count, uint64_t base,
        uint32_t capacity) {
    unsigned loads = 0;
    if (!ph || !count || count > 32u) return -1;
    for (uint32_t i = 0; i < count; i++) {
        const q8b_mdt_phdr *p = &ph[i];
        if (!q8b_mdt_loadable(p)) continue;
        if (p->p_filesz > p->p_memsz || p->p_paddr < base ||
                (uint64_t)p->p_offset + p->p_filesz > image_size ||
                (uint64_t)p->p_paddr - base + p->p_memsz > capacity)
            return -1;
        loads++;
    }
    return loads ? 0 : -1;
}

/* Match qcom_mdt_load_no_init(): copy file bytes, zero each segment's BSS,
 * and preserve bytes outside PT_LOAD ranges. Never write hash-only metadata. */
static inline int q8b_mdt_stage(volatile uint8_t *destination,
        uint32_t capacity, const uint8_t *image, uint32_t image_size,
        const q8b_mdt_phdr *ph, uint32_t count, uint64_t base) {
    if (!destination || !image ||
            q8b_mdt_validate_loads(image_size, ph, count, base, capacity))
        return -1;
    for (uint32_t i = 0; i < count; i++) {
        const q8b_mdt_phdr *p = &ph[i];
        if (!q8b_mdt_loadable(p)) continue;
        uint32_t offset = (uint32_t)((uint64_t)p->p_paddr - base);
        for (uint32_t k = 0; k < p->p_filesz; k++)
            destination[offset + k] = image[p->p_offset + k];
        for (uint32_t k = p->p_filesz; k < p->p_memsz; k++)
            destination[offset + k] = 0;
    }
    return 0;
}
#endif
