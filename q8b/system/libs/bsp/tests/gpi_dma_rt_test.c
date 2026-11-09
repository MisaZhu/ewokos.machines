/* Runtime host test for gpi_dma_rt: validates the full open -> start ->
 * xfer (TRE build + doorbell + event) -> close lifecycle with the pin
 * accounting and the mock io/shm backends.  No hardware touched. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define Q8B_GPI_RT_TEST
#define Q8B_GPI_DMA_TEST
#include "../src/gpi_dma.c"
#include "../src/gpi_dma_rt.c"
#include <bsp/gpi_dma_rt.h>

/* ---- host shm backend ---- */
#define MOCK_SHM 4
static uint32_t mock_slab[1 << 16];   /* pretend contig backing */
typedef struct { int used, pinned; uintptr_t pa; void* va; unsigned bytes; } mshm_t;
static mshm_t mshm[MOCK_SHM];
static int m_pin_calls, m_unpin_calls;
static void m_reset(void) { memset(mshm, 0, sizeof(mshm)); m_pin_calls=m_unpin_calls=0; }

int rt_shm_alloc_contig(unsigned size, int* id, uintptr_t* pa, void** va) {
    for (int i = 0; i < MOCK_SHM; i++)
        if (!mshm[i].used) {
            mshm[i].used = 1; mshm[i].pinned = 0;
            mshm[i].pa = (uintptr_t)(0x400000u + i * 0x100000u);
            mshm[i].va = mock_slab + i * 0x4000;
            mshm[i].bytes = size;
            *id = i + 1; *pa = mshm[i].pa; *va = mshm[i].va;
            return 0;
        }
    return -1;
}
void rt_shm_detach(int id, void* va) {
    assert(va != NULL);
    int i = id - 1;
    assert(i >= 0 && i < MOCK_SHM && mshm[i].used);
    mshm[i].used = 0; mshm[i].pinned = 0;
}
int rt_shm_pin(int id) { m_pin_calls++; mshm[id-1].pinned++; return 0; }
int rt_shm_unpin(int id) { m_unpin_calls++; assert(mshm[id-1].pinned > 0); mshm[id-1].pinned--; return 0; }

/* ---- host io backend (copied minimal) ---- */
static uint32_t mmio[0x24000u / 4u];
static uint32_t chstate[2];
static unsigned cmd_count, no_irq_cmd;
static uint32_t ev_wp_value;

static uint32_t rt_io_rd(uintptr_t b, unsigned o) { (void)b;
    assert(o / 4u < 0x24000u / 4u);
    return mmio[o / 4u];
}
static void rt_io_wr(uintptr_t b, unsigned o, uint32_t v) { (void)b;
    assert(o / 4u < 0x24000u / 4u);
    if (o == Q8B_GPI_CH_IRQ_CLR) { mmio[Q8B_GPI_CH_IRQ_STS / 4u] &= ~v; return; }
    if (o == Q8B_GPI_CH_CMD) {
        uint32_t op = v >> 24, ch = v & 0xffu;
        cmd_count++;
        if (cmd_count == no_irq_cmd) return;
        if (op != 10u) {
            uint32_t st = op == 0u ? 1u : op == 1u ? 2u : op == 2u ? 3u : 0u;
            chstate[ch] = st;
            mmio[(Q8B_GPI_CH_CNTXT0 + ch * 0x80u) / 4u] =
                (mmio[(Q8B_GPI_CH_CNTXT0 + ch * 0x80u) / 4u] &
                 ~Q8B_GPI_CNTXT0_CHSTATE) | (st << 20);
        }
        mmio[Q8B_GPI_CH_IRQ_STS / 4u] |= 1u << ch;
        return;
    }
    mmio[o / 4u] = v;
}
static void rt_io_delay(unsigned us) { assert(us == 100 || us == 1000); (void)us; }

static void m_reset_all(void) {
    memset(mmio, 0, sizeof(mmio)); memset(chstate, 0, sizeof(chstate));
    cmd_count = no_irq_cmd = 0; ev_wp_value = 0;
    m_reset();
}

int main(void) {
    q8b_gpi_spi_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.word_sz = 0; cfg.clock_pol_high = 1; cfg.data_pol_high = 1;
    cfg.pack_en = 1; cfg.cmd = Q8B_GPI_SPI_CMD_DUPLEX; cfg.clk_div = 2;

    /* lifecycle */
    m_reset_all();
    q8b_gpi_rt_t rt;
    assert(q8b_gpi_rt_open(&rt, 0, 8) == 0);
    assert(rt.desc_shm && rt.ev_shm && rt.pinned == 1);
    assert(mshm[rt.desc_shm - 1].pinned == 1 && mshm[rt.ev_shm - 1].pinned == 1);
    assert(m_pin_calls == 2);
    /* start: 2 alloc + 2 start + context writes */
    assert(q8b_gpi_rt_start(&rt) == 0);
    assert(cmd_count == 4 && rt.dma.started);
    /* xfer with a fake completion */
    uint8_t ev[8 * Q8B_GPI_EVENT_SIZE];
    memset(ev, 0, sizeof(ev));
    ev[11] = Q8B_GPI_EV_CODE_SUCCESS;         /* code */
    ev[14] = Q8B_GPI_EV_TYPE_XFER_COMPLETE;   /* type */
    ev[15] = 1;                               /* chid */
    ev[8] = 7;                                /* length */
    memcpy(rt.ev_v, ev, sizeof(ev));
    ev_wp_value = 1;
    mmio[Q8B_GPI_EV_WP_LO / 4u] = 1;
    assert(q8b_gpi_rt_xfer(&rt, 0x5000, 0, 7, 0x6000, 0, 7, &cfg, 100000) == 0);
    assert(rt.doorbells == 1 && rt.dma.wp == 4);
    /* TRE content written into the pinned ring */
    {
        uint32_t* c0 = rt.desc_v + 0 * 8;
        assert(((c0[3] & Q8B_GPI_TRE_FLAGS_TYPE) >> 16) == Q8B_GPI_TRE_TYPE_CONFIG0);
        uint32_t* go = rt.desc_v + 1 * 8;
        assert((go[0] & Q8B_GPI_SPI_GO_CMD) == Q8B_GPI_SPI_CMD_DUPLEX);
        uint32_t* drx = rt.desc_v + 3 * 8;
        assert((drx[2] & Q8B_GPI_DMA_LEN) == 7u);
    }
    /* close: stop+dealloc then unpin+detach */
    assert(q8b_gpi_rt_close(&rt) == 0);
    assert(m_unpin_calls == 2 && mshm[0].used == 0 && !mshm[0].pinned);
    assert(rt.desc_shm == 0);

    /* guards */
    m_reset_all();
    memset(&rt, 0, sizeof(rt));
    assert(q8b_gpi_rt_xfer(&rt, 0, 0, 0, 0, 0, 0, &cfg, 100) == -1); /* unopened */
    assert(q8b_gpi_rt_open(&rt, 12, 8) == -1);                       /* bad instance */
    assert(q8b_gpi_rt_open(&rt, 0, 9) == -1);                        /* not pow2 */
    assert(q8b_gpi_rt_open(&rt, 0, 8) == 0);
    assert(q8b_gpi_rt_xfer(&rt, 0, 0, 7, 0, 0, 7, &cfg, 100) == -1); /* not started */
    /* command timeout -> poisoned: close refuses further use */
    m_reset_all();
    memset(&rt, 0, sizeof(rt));
    assert(q8b_gpi_rt_open(&rt, 0, 8) == 0);
    no_irq_cmd = 1;
    assert(q8b_gpi_rt_start(&rt) == -1 && rt.dma.poisoned);
    assert(q8b_gpi_rt_close(&rt) == 0);   /* poisoned: stop skipped, unpin+detach */
    assert(mshm[0].used == 0 && !mshm[0].pinned);

    puts("q8b GPI DMA runtime tests: ok");
    return 0;
}