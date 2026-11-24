#ifndef GA10B_MM_H
#define GA10B_MM_H

/*
 * GMMU ver2 page-table entry encoders -- pure logic, host-unit-testable.
 * VERIFIED against nvgpu rel-36 hal/mm/gmmu/gmmu_ga10b_fusa.c
 * (ga10b_update_gmmu_pde3_locked / pde0_locked / update_pte) and
 * include/nvgpu/hw/ga10b/hw_gmmu_ga10b.h field macros.
 *
 * NOTE: the extraction ref /tmp/ga10b-ref/mm-fifo.md omitted the >>12
 * address shift and mis-stated the vol/valid bits; the encoders below
 * follow the nvgpu source, not that ref.
 *
 * Aperture choice: sys_ncoh (PDE 6 / PTE 6).  PDE always sets vol; a PTE
 * sets vol unless the mapping is cacheable (nvgpu NVGPU_VM_MAP_CACHEABLE):
 * vol makes the GPU bypass its L2 cache, so every access is a DRAM round
 * trip.  A cacheable mapping needs the L2 handoff in ga10b_l2_invalidate /
 * ga10b_l2_flush whenever the CPU touches the memory too.
 * All functions take a RAW physical address and do the >>12 internally.
 */

#include <stdint.h>

#define GA10B_GMMU_PDE_APERTURE_NCOH  6u   /* vid=2 coh=4 ncoh=6 */
#define GA10B_GMMU_PDE_APERTURE_COH   4u
#define GA10B_GMMU_PDE_VOL            8u
#define GA10B_GMMU_PTE_VALID          1u
#define GA10B_GMMU_PTE_VOL            8u
#define GA10B_GMMU_PTE_RO             0x40u
#define GA10B_GMMU_PTE_APERTURE_NCOH  6u   /* vid=0 coh=4 ncoh=6 */
#define GA10B_GMMU_PTE_APERTURE_COH   4u

/* PDE3 / PDE2 / PDE1 (8-byte entry): child page-table physical address.
 *   w0 = ap | ((pa>>12 & 0xffffff) << 8) | vol
 *   w1 = pa>>36
 * ap selects the sysmem aperture: ncoh(6) for the uncached DMA pool on
 * non-coherent paths, coh(4) when the GPU is IO-coherent (Orin dma-coherent,
 * NVGPU_USE_COHERENT_SYSMEM).  PDE and PTE share the same coh/ncoh values. */
static inline void ga10b_gmmu_pde(uint32_t* e, uint64_t child_pa, uint32_t ap) {
    uint64_t s = child_pa >> 12;
    e[0] = ap | ((uint32_t)(s & 0xffffffu) << 8) | GA10B_GMMU_PDE_VOL;
    e[1] = (uint32_t)(s >> 24);
}

/* PDE0 (16-byte dual entry), SMALL-page half points to a PTE table.
 * Big-page half (w0/w1) left invalid (0).  nvgpu pde0_locked small path. */
static inline void ga10b_gmmu_pde0_small(uint32_t* e, uint64_t child_pa,
                                         uint32_t ap) {
    uint64_t s = child_pa >> 12;
    e[0] = 0;
    e[1] = 0;
    e[2] = ap | ((uint32_t)(s & 0xffffffu) << 8) | GA10B_GMMU_PDE_VOL;
    e[3] = (uint32_t)(s >> 24);
}

/* PTE small 4KB page (8-byte entry): valid, rw, ncoh; `vol` is
 * GA10B_GMMU_PTE_VOL (L2 bypass) or 0 (cacheable).
 *   w0 = valid | ((pa>>12 & 0xffffff)<<8) | ap | vol
 *   w1 = (pa>>36) | kind<<24   (kind=0 pitch-linear)
 * NO priv bit: nvgpu maps channel gpfifo/pb/sem/runqueue with
 * gk20a_mem_flag_none (attrs->priv=false).  PBDMA's pushbuffer method fetch
 * is a non-privileged access; a priv-only PTE faults it (protection fault)
 * even though the privileged GPFIFO-ring read succeeds. */
static inline void ga10b_gmmu_pte(uint32_t* e, uint64_t page_pa, uint32_t ap,
                                  uint32_t vol) {
    uint64_t s = page_pa >> 12;
    e[0] = GA10B_GMMU_PTE_VALID | ((uint32_t)(s & 0xffffffu) << 8) |
           ap | vol;
    e[1] = (uint32_t)(s >> 24);
}

/* recover child physical address from a PDE (w0,w1) or PDE0-small (w2,w3) */
static inline uint64_t ga10b_gmmu_pde_pa(uint32_t w0, uint32_t w1) {
    return (((uint64_t)(w0 >> 8) & 0xffffffu) << 12) | ((uint64_t)w1 << 36);
}

/* a PDE is present when its aperture field (bits 2:1) is non-zero */
static inline int ga10b_gmmu_pde_valid(uint32_t w0) {
    return (w0 & 0x6u) != 0u;
}

#endif