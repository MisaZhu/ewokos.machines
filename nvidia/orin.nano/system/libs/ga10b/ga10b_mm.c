/*
 * ga10b_mm.c -- GA10B GMMU ver2 page tables (M0).
 *
 * Builds a 5-level page table hierarchy in the uncached sys_dma pool and
 * maps physical DMA buffers into a per-VM GPU VA space.  Entry bit layouts
 * are in ga10b_mm.h (verified against nvgpu gmmu_ga10b_fusa.c).  The map
 * logic runs fully in dry-run (CPU writes to DMA memory); only the TLB
 * invalidate is MMIO and is gated by ga10b.hw.
 *
 * Level structure (ga10b_mm_levels[] in nvgpu):
 *   lvl0 PDE3  VA[48:47]   4 entries x 8B   (root = PDB)
 *   lvl1 PDE2  VA[46:38] 512 entries x 8B
 *   lvl2 PDE1  VA[37:29] 512 entries x 8B
 *   lvl3 PDE0  VA[28:21] 256 entries x 16B  (dual; small half used)
 *   lvl4 PTE   VA[20:12] 512 entries x 8B   (4KB small pages)
 */
#include <string.h>
#include <ewoksys/klog.h>

#include "ga10b.h"
#include "ga10b_regs.h"
#include "ga10b_mm.h"

#define GA10B_MM_VA_BASE   0x10000000ULL   /* GPU VA bump-allocator start */
#define GA10B_MM_PAGE      4096u

static const uint32_t lo_bit[5]       = {47, 38, 29, 21, 12};
static const uint32_t n_entries[5]    = {4, 512, 512, 256, 512};
static const uint32_t entry_words[5]  = {2, 2, 2, 4, 2};

static int alloc_table(ga10b_gmmu_t* mm, void** va, uint64_t* phy) {
    if (mm->ntab >= GA10B_MM_MAX_TABLES) {
        klog("ga10b: gmmu table pool exhausted (%u)\n", mm->ntab);
        return -1;
    }
    if (ga10b_dma_alloc(GA10B_MM_PAGE, GA10B_MM_PAGE, va, phy) != 0)
        return -1;
    mm->tab[mm->ntab].va = *va;
    mm->tab[mm->ntab].phy = *phy;
    mm->ntab++;
    return 0;
}

static void* table_va_by_phy(ga10b_gmmu_t* mm, uint64_t phy) {
    for (uint32_t i = 0; i < mm->ntab; i++)
        if (mm->tab[i].phy == phy)
            return mm->tab[i].va;
    return NULL;
}

/* gm20b_fb_tlb_invalidate: wait for PRI fifo space, target this PDB,
 * trigger all_va, wait for the PRI fifo to drain (1000 x 2us each). */
static int tlb_invalidate(const ga10b_gmmu_t* mm) {
    uint32_t i;
    if (ga10b.hw) {
        for (i = 0; i < 1000u; i++) {
            if (ga10b_rd(GA10B_FB_MMU_CTRL) & GA10B_FB_MMU_CTRL_PRI_FIFO_SPACE)
                break;
            ga10b_delay_us(2);
        }
        if (i == 1000u) {
            klog("ga10b: tlb invalidate: no mmu fifo space\n");
            return -1;
        }
    }
    ga10b_wr(GA10B_FB_MMU_INVALIDATE_PDB,
             GA10B_FB_INV_PDB_APERTURE_SYS |
             GA10B_FB_INV_PDB_ADDR(mm->pdb_phy >> 12));
    ga10b_wr(GA10B_FB_MMU_INVALIDATE,
             GA10B_FB_INV_ALL_VA | GA10B_FB_INV_TRIGGER);
    if (ga10b.hw) {
        for (i = 0; i < 1000u; i++) {
            if (ga10b_rd(GA10B_FB_MMU_CTRL) & GA10B_FB_MMU_CTRL_PRI_FIFO_EMPTY)
                break;
            ga10b_delay_us(2);
        }
        if (i == 1000u) {
            klog("ga10b: tlb invalidate: mmu fifo not drained\n");
            return -1;
        }
    }
    return 0;
}

int ga10b_gmmu_init(ga10b_gmmu_t* mm) {
    memset(mm, 0, sizeof(*mm));
    if (ga10b_dma_alloc(GA10B_MM_PAGE, GA10B_MM_PAGE,
                        &mm->pdb_va, &mm->pdb_phy) != 0)
        return -1;
    mm->va_next = GA10B_MM_VA_BASE;
    mm->aperture = GA10B_GMMU_PDE_APERTURE_NCOH;
    klog("ga10b: gmmu init pdb phy=%llx va_base=%llx (5-level ver2, ncoh)\n",
         (unsigned long long)mm->pdb_phy,
         (unsigned long long)mm->va_next);
    return 0;
}

/* PTE table covering page_va's 2MB span; with `create` the missing
 * directory levels are allocated, otherwise NULL means nothing was ever
 * mapped there */
static uint32_t* pte_table(ga10b_gmmu_t* mm, uint64_t page_va, int create) {
    void* cur_va = mm->pdb_va;

    /* walk PDE3 -> PDE2 -> PDE1 -> PDE0 */
    for (uint32_t lvl = 0; lvl < 4; lvl++) {
        uint32_t idx = (uint32_t)(page_va >> lo_bit[lvl]) &
                       (n_entries[lvl] - 1);
        uint32_t* ent = (uint32_t*)cur_va + idx * entry_words[lvl];
        /* PDE0 small half lives in w2/w3; other levels in w0/w1 */
        uint32_t w0 = (lvl == 3) ? ent[2] : ent[0];
        uint32_t w1 = (lvl == 3) ? ent[3] : ent[1];
        void* child_va;
        uint64_t child_phy;

        if (!ga10b_gmmu_pde_valid(w0)) {
            if (!create || alloc_table(mm, &child_va, &child_phy) != 0)
                return NULL;
            if (lvl == 3)
                ga10b_gmmu_pde0_small(ent, child_phy, mm->aperture);
            else
                ga10b_gmmu_pde(ent, child_phy, mm->aperture);
        } else {
            child_phy = ga10b_gmmu_pde_pa(w0, w1);
            child_va = table_va_by_phy(mm, child_phy);
            if (child_va == NULL) {
                klog("ga10b: gmmu lost child table phy=%llx\n",
                     (unsigned long long)child_phy);
                return NULL;
            }
        }
        cur_va = child_va;
    }
    return cur_va;
}

/* map [phy, phy+size) at a caller-chosen VA (4KB small pages).  Remapping a
 * range that is already mapped just rewrites its PTEs. */
int ga10b_gmmu_map_at(ga10b_gmmu_t* mm, uint64_t va, uint64_t phy,
                      uint32_t size, int cacheable) {
    uint32_t npages = (size + GA10B_MM_PAGE - 1) / GA10B_MM_PAGE;
    uint32_t* tab = NULL;
    uint64_t tab_span = ~0ULL;
    for (uint32_t p = 0; p < npages; p++) {
        uint64_t page_va = va + (uint64_t)p * GA10B_MM_PAGE;
        if ((page_va >> 21) != tab_span) {
            tab = pte_table(mm, page_va, 1);
            if (tab == NULL)
                return -1;
            tab_span = page_va >> 21;
        }
        uint32_t idx = (uint32_t)(page_va >> 12) & (n_entries[4] - 1);
        ga10b_gmmu_pte(tab + idx * entry_words[4],
                       phy + (uint64_t)p * GA10B_MM_PAGE, mm->aperture,
                       cacheable ? 0u : GA10B_GMMU_PTE_VOL);
    }
    /* flush GPU TLB for this PDB (MMIO; dry-run logs only).  Like nvgpu,
     * a timeout is reported but does not fail the mapping. */
    (void)tlb_invalidate(mm);
    return 0;
}

/* invalidate the PTEs of [va, va+size); the page tables stay for reuse */
void ga10b_gmmu_unmap(ga10b_gmmu_t* mm, uint64_t va, uint32_t size) {
    uint32_t npages = (size + GA10B_MM_PAGE - 1) / GA10B_MM_PAGE;
    uint32_t* tab = NULL;
    uint64_t tab_span = ~0ULL;
    for (uint32_t p = 0; p < npages; p++) {
        uint64_t page_va = va + (uint64_t)p * GA10B_MM_PAGE;
        if ((page_va >> 21) != tab_span) {
            tab = pte_table(mm, page_va, 0);
            tab_span = page_va >> 21;
        }
        if (tab != NULL) {
            uint32_t idx = (uint32_t)(page_va >> 12) & (n_entries[4] - 1);
            tab[idx * entry_words[4]] = 0;
            tab[idx * entry_words[4] + 1] = 0;
        }
    }
    (void)tlb_invalidate(mm);
}

/* flush_gk20a: write pending, wait for pending and outstanding to drop */
static int l2_op(uint32_t reg, const char* what) {
    ga10b_wr(reg, GA10B_FLUSH_PENDING_BUSY);
    if (!ga10b.hw)
        return 0;
    for (uint32_t i = 0; i < 200000u; i++)
        if ((ga10b_rd(reg) &
             (GA10B_FLUSH_PENDING_BUSY | GA10B_FLUSH_OUTSTANDING)) == 0u)
            return 0;
    klog("ga10b: %s did not complete (%08x)\n", what, ga10b_rd(reg));
    return -1;
}

/* CPU -> GPU handoff: drop the clean L2 lines so the GPU re-reads what the
 * CPU wrote into cacheable mappings (dirty lines are not affected) */
int ga10b_l2_invalidate(void) {
    return l2_op(GA10B_FLUSH_L2_SYSTEM_INVALIDATE, "l2 invalidate");
}

/* GPU -> CPU handoff: write the dirty L2 lines back to DRAM */
int ga10b_l2_flush(void) {
    return l2_op(GA10B_FLUSH_L2_FLUSH_DIRTY, "l2 flush");
}

uint64_t ga10b_gmmu_map_attr(ga10b_gmmu_t* mm, uint64_t phy, uint32_t size,
                             int cacheable) {
    if (size == 0)
        return 0;
    uint32_t npages = (size + GA10B_MM_PAGE - 1) / GA10B_MM_PAGE;
    uint64_t base = mm->va_next;

    if (ga10b_gmmu_map_at(mm, base, phy, size, cacheable) != 0)
        return 0;
    mm->va_next = base + (uint64_t)npages * GA10B_MM_PAGE;

    if (!ga10b.quiet)
        klog("ga10b: gmmu map phy=%llx size=%u -> va=%llx (%u pages, %u "
             "tables)\n", (unsigned long long)phy, size,
             (unsigned long long)base, npages, mm->ntab);
    return base;
}

/* uncached (vol): control structures the CPU and GPU both update */
uint64_t ga10b_gmmu_map(ga10b_gmmu_t* mm, uint64_t phy, uint32_t size) {
    return ga10b_gmmu_map_attr(mm, phy, size, 0);
}

void ga10b_gmmu_destroy(ga10b_gmmu_t* mm) {
    for (uint32_t i = 0; i < mm->ntab; i++)
        ga10b_dma_free(mm->tab[i].va);
    mm->ntab = 0;
    if (mm->pdb_va != NULL) {
        ga10b_dma_free(mm->pdb_va);
        mm->pdb_va = NULL;
    }
}

/* CPU-side walk of the page tables for `va`, printing each level's entry so
 * a GPU translation fault can be matched against what SW actually wrote. */
void ga10b_gmmu_dump(const ga10b_gmmu_t* mm, uint64_t va) {
    static const uint32_t lo[5] = {47, 38, 29, 21, 12};
    static const uint32_t ne[5] = {4, 512, 512, 256, 512};
    static const uint32_t ew[5] = {2, 2, 2, 4, 2};
    void* cur = mm->pdb_va;
    klog("ga10b: ptwalk va=%llx pdb=%llx\n", (unsigned long long)va,
         (unsigned long long)mm->pdb_phy);
    for (uint32_t lvl = 0; lvl < 5; lvl++) {
        uint32_t idx = (uint32_t)(va >> lo[lvl]) & (ne[lvl] - 1);
        uint32_t* ent = (uint32_t*)cur + idx * ew[lvl];
        if (lvl == 3)
            klog("ga10b:  L%u idx=%u big=%08x.%08x small=%08x.%08x\n",
                 lvl, idx, ent[1], ent[0], ent[3], ent[2]);
        else
            klog("ga10b:  L%u idx=%u ent=%08x.%08x\n",
                 lvl, idx, ent[1], ent[0]);
        if (lvl == 4)
            break;
        uint32_t w0 = (lvl == 3) ? ent[2] : ent[0];
        uint32_t w1 = (lvl == 3) ? ent[3] : ent[1];
        if (!ga10b_gmmu_pde_valid(w0)) {
            klog("ga10b:  L%u INVALID pde\n", lvl);
            return;
        }
        uint64_t child_phy = ga10b_gmmu_pde_pa(w0, w1);
        cur = NULL;
        for (uint32_t i = 0; i < mm->ntab; i++)
            if (mm->tab[i].phy == child_phy) {
                cur = mm->tab[i].va;
                break;
            }
        if (cur == NULL) {
            klog("ga10b:  L%u child phy=%llx not in pool\n", lvl,
                 (unsigned long long)child_phy);
            return;
        }
    }
}