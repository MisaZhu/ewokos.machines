#include <bsp/gpi_dma_rt.h>
#include <string.h>

#ifdef Q8B_GPI_RT_TEST
/* host test injects io/shm */
static uint32_t rt_io_rd(uintptr_t b, unsigned o);
static void rt_io_wr(uintptr_t b, unsigned o, uint32_t v);
static void rt_io_delay(unsigned us);
extern int rt_shm_alloc_contig(unsigned size, int* id, uintptr_t* pa,
                               void** va);
extern void rt_shm_detach(int id, void* va);
extern int rt_shm_pin(int id);
extern int rt_shm_unpin(int id);
#else
#include <ewoksys/shm.h>
#include <ewoksys/syscall.h>
#include <unistd.h>
static uint32_t rt_io_rd(uintptr_t b, unsigned o) {
    uint32_t v = *(volatile uint32_t*)(b + o);
    __asm__ volatile("dmb oshld" ::: "memory");
    return v;
}
static void rt_io_wr(uintptr_t b, unsigned o, uint32_t v) {
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint32_t*)(b + o) = v;
    __asm__ volatile("dsb sy" ::: "memory");
}
static void rt_io_delay(unsigned us) { usleep(us); }
static int rt_shm_alloc_contig(unsigned size, int* id, uintptr_t* pa,
                               void** va) {
    /* key: small per-process unique tag; IPC_CONTIG requires the slab. */
    static key_t key = 0x51525430u;   /* "0TRQ" */
    int sid = shmget(key++, ((size + 4095u) & ~4095u), 0666 | IPC_CREAT | IPC_EXCL | IPC_CONTIG);
    if (sid <= 0)
        return -1;
    void* v = shmat(sid, 0, 0);
    if (v == (void*)-1) {
        shmctl(sid, IPC_RMID, NULL);
        return -1;
    }
    *id = sid;
    *va = v;
    *pa = shm_contig_phy_addr(sid, (ewokos_addr_t)v);
    if (*pa == 0) {
        shmdt(v);
        shmctl(sid, IPC_RMID, NULL);
        return -1;
    }
    return 0;
}
static void rt_shm_detach(int id, void* va) {
    if (id > 0 && va) shmdt(va);
    if (id > 0) shmctl(id, IPC_RMID, NULL);
}
static int rt_shm_pin(int id) { return shm_dma_pin(id); }
static int rt_shm_unpin(int id) { return shm_dma_unpin(id); }
#endif

/* One io backend shared by real (MMIO) and host-test builds. */
static const q8b_gpi_io_t rt_io = { rt_io_rd, rt_io_wr, rt_io_delay };

int q8b_gpi_rt_open(q8b_gpi_rt_t* rt, unsigned instance, unsigned entries) {
    if (!rt || instance >= 12 || entries < 2 || (entries & (entries - 1)))
        return -1;
    memset(rt, 0, sizeof(*rt));
    rt->instance = instance;
    rt->entries = entries;

#ifdef Q8B_GPI_RT_TEST
    rt->dma.base = 0x9000000u;   /* symbolic; host io ignores the value */
    rt->dma.io = &rt_io;
#else
    uintptr_t pa = 0x00900000u + instance * Q8B_GPI_REG_STRIDE; /* qup0 gpi */
    uintptr_t va = 0x5b108000u;
    if (syscall3(SYS_MEM_MAP, va, pa, Q8B_GPI_REG_STRIDE) != va)
        return -1;
    rt->dma.base = va;
    rt->dma.io = &rt_io;
#endif
    rt->stage = "rings-alloc";
    if (rt_shm_alloc_contig(entries * Q8B_GPI_TRE_SIZE, &rt->desc_shm,
                            &rt->desc_pa, (void**)&rt->desc_v) != 0 ||
            rt_shm_alloc_contig(entries * Q8B_GPI_EVENT_SIZE, &rt->ev_shm,
                                &rt->ev_pa, (void**)&rt->ev_v) != 0)
        goto fail;
    if (rt_shm_pin(rt->desc_shm) != 0 || rt_shm_pin(rt->ev_shm) != 0)
        goto fail;
    rt->pinned = 1;
    rt->rings.entries = entries;
    rt->rings.el_size = Q8B_GPI_TRE_SIZE;
    rt->rings.er_size = Q8B_GPI_EVENT_SIZE;
    rt->rings.desc_phys = rt->desc_pa;
    rt->rings.ev_phys = rt->ev_pa;
    if (q8b_gpi_dma_init(&rt->dma, rt->dma.base, &rt->rings, &rt_io) != 0)
        goto fail;
    rt->stage = "opened";
    return 0;
fail:
    if (rt->pinned) {
        rt_shm_unpin(rt->desc_shm);
        rt_shm_unpin(rt->ev_shm);
        rt->pinned = 0;
    }
    if (rt->desc_shm) rt_shm_detach(rt->desc_shm, rt->desc_v);
    if (rt->ev_shm) rt_shm_detach(rt->ev_shm, rt->ev_v);
    memset(rt, 0, sizeof(*rt));
    rt->stage = "open-failed";
    return -1;
}

int q8b_gpi_rt_start(q8b_gpi_rt_t* rt) {
    if (!rt || rt->pinned == 0 || rt->dma.stage == NULL)
        return -1;
    if (q8b_gpi_dma_program_rings(&rt->dma) != 0)
        return -1;
    if (q8b_gpi_dma_start(&rt->dma) != 0)
        return -1;
    rt->stage = "started";
    return 0;
}

int q8b_gpi_rt_xfer(q8b_gpi_rt_t* rt, uint32_t tx_pa, uint32_t tx_hi,
                    uint32_t tx_len, uint32_t rx_pa, uint32_t rx_hi,
                    uint32_t rx_len, const q8b_gpi_spi_cfg_t* cfg,
                    uint64_t budget_us) {
    if (!rt || !rt->pinned || !rt->dma.started)
        return -1;
    rt->stage = "xfer-build";
    uint32_t wp = rt->dma.wp;
    if (q8b_gpi_dma_build_xfer(rt->desc_v, rt->entries, wp, cfg,
                               tx_pa, tx_hi, tx_len, rx_pa, rx_hi, rx_len,
                               &wp) != 0)
        return -1;
    rt->stage = "xfer-doorbell";
    if (q8b_gpi_dma_commit(&rt->dma, wp) != 0)
        return -1;
    rt->doorbells++;
    rt->stage = "xfer-wait";
    uint32_t code = 0, chid = 0, length = 0;
    if (q8b_gpi_dma_wait_event(&rt->dma, rt->ev_v, budget_us, &code, &chid,
                               &length) != 0)
        return -1;
    rt->stage = "xfer-ok";
    return 0;
}

int q8b_gpi_rt_close(q8b_gpi_rt_t* rt) {
    if (!rt)
        return -1;
    if (rt->dma.started) {
        if (q8b_gpi_dma_stop(&rt->dma) != 0)
            return -1;             /* poisoned: caller must not reuse */
        if (q8b_gpi_dma_dealloc(&rt->dma) != 0)
            return -1;
    }
    if (rt->pinned) {
        rt_shm_unpin(rt->desc_shm);
        rt_shm_unpin(rt->ev_shm);
        rt->pinned = 0;
    }
    if (rt->desc_shm) rt_shm_detach(rt->desc_shm, rt->desc_v);
    if (rt->ev_shm) rt_shm_detach(rt->ev_shm, rt->ev_v);
    rt->desc_shm = rt->ev_shm = 0;
    rt->stage = "closed";
    return 0;
}