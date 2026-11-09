/* Host test for the GPI DMA candidate (gpi_dma.c).  Simulates a small GPI
 * register file plus descriptor/event rings and validates TRE building,
 * event decoding, ring boundaries, command acknowledgement and the full
 * start/commit/wait/stop/reset/dealloc lifecycle including every poisoned
 * error path.  Passing this does NOT validate hardware access. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define Q8B_GPI_DMA_TEST
#include "../src/gpi_dma.c"

#define REGS 0x24000u / 4u
static uint32_t mmio[REGS];
static uint32_t mmio_ro[REGS];     /* 1 = read-only (context/doorbell) */
static unsigned cmd_count, no_irq_cmd, bad_state_cmd, stuck_clear, io_writes;
static uint32_t chstate[2];


static uint32_t rd(uintptr_t base, unsigned off) {
    assert(base == 0x9000000U || base == 0);
    assert(off / 4u < REGS);
    return mmio[off / 4u];
}
static void wr(uintptr_t base, unsigned off, uint32_t value) {
    assert(base == 0x9000000U || base == 0);
    assert(off / 4u < REGS);
    io_writes++;
    if (mmio_ro[off / 4u]) {                 /* protected register */
        assert(0 && "write to read-only/context register");
    }
    if (off == Q8B_GPI_CH_IRQ_CLR) {
        mmio[Q8B_GPI_CH_IRQ_STS / 4u] &= ~value;
        return;
    }
    if (off == Q8B_GPI_CH_CMD) {
        uint32_t op = value >> 24;
        uint32_t ch = value & 0xffu;
        assert(ch < 2);
        cmd_count++;
        if (cmd_count == no_irq_cmd)
            return;
        /* Command completion semantics: allocate/start/stop/reset update the
         * context state; DEALLOC leaves it (hardware behaviour). */
        if (op != 10u) {
            uint32_t st = op == 0u ? 1u : op == 1u ? 2u : op == 2u ? 3u : 0u;
            chstate[ch] = st;
            mmio[(Q8B_GPI_CH_CNTXT0 + ch * 0x80u) / 4u] =
                    (mmio[(Q8B_GPI_CH_CNTXT0 + ch * 0x80u) / 4u] &
                     ~Q8B_GPI_CNTXT0_CHSTATE) | (st << 20);
        }
        if (cmd_count != bad_state_cmd) {
            mmio[Q8B_GPI_CH_IRQ_STS / 4u] |= 1u << ch;
        }
        if (stuck_clear)
            mmio[Q8B_GPI_CH_IRQ_STS / 4u] |= 1u << ch; /* clear cannot remove */
        return;
    }
    mmio[off / 4u] = value;
}
static void delay(unsigned us) { assert(us == 100); }

static q8b_gpi_io_t io = { rd, wr, delay };

/* wait_event polls EV_WP_LO.  Let the first n reads return 0, then the
 * programmed ev_wp_value, simulating hardware completing a transfer. */
static void reset(void) {
    memset(mmio, 0, sizeof(mmio));
    memset(mmio_ro, 0, sizeof(mmio_ro));
    memset(chstate, 0, sizeof(chstate));
    cmd_count = no_irq_cmd = bad_state_cmd = stuck_clear = io_writes = 0;
}

static void build_event(uint8_t* ev, uint32_t code, uint32_t type,
                        uint32_t chid, uint32_t len) {
    memset(ev, 0, Q8B_GPI_EVENT_SIZE);
    ev[8] = (uint8_t)len;
    ev[9] = (uint8_t)(len >> 8);
    ev[10] = (uint8_t)(len >> 16);
    ev[11] = (uint8_t)code;
    ev[14] = (uint8_t)type;
    ev[15] = (uint8_t)chid;
}

static void new_dma(q8b_gpi_dma_t* d, uint32_t entries) {
    q8b_gpi_rings_t r = { entries, Q8B_GPI_TRE_SIZE, Q8B_GPI_EVENT_SIZE,
                          0x100000, 0x200000 };
    assert(q8b_gpi_dma_init(d, 0x9000000U, &r, &io) == 0);
}

int main(void) {
    /* ---- TRE builders ---- */
    {
        q8b_gpi_spi_cfg_t cfg = { 0, 1, 1, 1, 1, 0, 0, 3, 31, 2, 512 };
        uint32_t tre[32] = {0};
        assert(q8b_gpi_tre_config0(tre, &cfg) == 0);
        assert(tre[0] == 0u + Q8B_GPI_SPI_C0_LOOPBACK + Q8B_GPI_SPI_C0_CPOL +
               Q8B_GPI_SPI_C0_CPHA + Q8B_GPI_SPI_C0_TX_PACK +
               Q8B_GPI_SPI_C0_RX_PACK);
        assert((tre[2] & Q8B_GPI_SPI_C0_CLK_DIV) == 31u);
        assert(((tre[2] & Q8B_GPI_SPI_C0_CLK_SRC) >> 16) == 2u);
        assert(((tre[3] & Q8B_GPI_TRE_FLAGS_TYPE) >> 16) == Q8B_GPI_TRE_TYPE_CONFIG0);
        assert(tre[3] & Q8B_GPI_TRE_FLAGS_CHAIN);
        assert(q8b_gpi_tre_go(tre + 8, &cfg, 512) == 0);
        uint32_t* go = tre + 8;
        assert((go[0] & Q8B_GPI_SPI_GO_CMD) == Q8B_GPI_SPI_CMD_DUPLEX);
        assert(((go[0] & Q8B_GPI_SPI_GO_CS) >> 8) == 0);
        assert((go[2] & Q8B_GPI_GO_RX_LEN) == 512u);
        assert(((go[3] & Q8B_GPI_TRE_FLAGS_TYPE) >> 16) == Q8B_GPI_TRE_TYPE_GO);
        assert(go[3] & Q8B_GPI_TRE_FLAGS_CHAIN);
        assert(q8b_gpi_tre_dma(tre + 16, 0x12345, 0x80000000u, 64, true) == 0);
        uint32_t* dma_tre = tre + 16;
        assert(dma_tre[0] == 0x12345u && dma_tre[1] == 0x80000000u);
        assert((dma_tre[2] & Q8B_GPI_DMA_LEN) == 64u);
        assert(((dma_tre[3] & Q8B_GPI_TRE_FLAGS_TYPE) >> 16) == Q8B_GPI_TRE_TYPE_DMA);
        assert(dma_tre[3] & Q8B_GPI_TRE_FLAGS_IEOT);
        assert(dma_tre[3] & Q8B_GPI_TRE_FLAGS_BEI);
        assert(q8b_gpi_tre_dma(tre + 24, 0, 0, 0, false) == -1);   /* len 0 */
        q8b_gpi_spi_cfg_t bad = cfg;
        bad.word_sz = 32;
        assert(q8b_gpi_tre_config0(tre, &bad) == -1);
        bad = cfg; bad.cmd = 9;
        assert(q8b_gpi_tre_go(tre, &bad, 0) == -1);
    }
    /* ---- event decode ---- */
    {
        uint8_t ev[16];
        uint32_t code, type, chid, len;
        build_event(ev, Q8B_GPI_EV_CODE_EOT, Q8B_GPI_EV_TYPE_XFER_COMPLETE, 1, 4095);
        assert(q8b_gpi_event_decode(ev, &code, &type, &chid, &len) == 0);
        assert(code == Q8B_GPI_EV_CODE_EOT && type == Q8B_GPI_EV_TYPE_XFER_COMPLETE);
        assert(chid == 1 && len == 4095);
        assert(q8b_gpi_event_decode(NULL, NULL, NULL, NULL, NULL) == -1);
    }
    /* ---- ring validation ---- */
    {
        q8b_gpi_dma_t d;
        q8b_gpi_rings_t r;
        assert(q8b_gpi_dma_init(&d, 0x9000000U, NULL, &io) == -1);
        r = (q8b_gpi_rings_t){ 8, 32, 16, 0x100000, 0x200000 };
        assert(q8b_gpi_dma_init(&d, 0x9000000U, &r, &io) == 0);
        r.entries = 9;                      /* not power of two */
        assert(q8b_gpi_dma_init(&d, 0x9000000U, &r, &io) == -1);
        r.entries = 8; r.el_size = 16;      /* wrong element sizes */
        assert(q8b_gpi_dma_init(&d, 0x9000000U, &r, &io) == -1);
        r.el_size = 32; r.ev_phys = 0;      /* missing ring */
        assert(q8b_gpi_dma_init(&d, 0x9000000U, &r, &io) == -1);
        assert(q8b_gpi_dma_init(NULL, 0x9000000U, &r, &io) == -1);
    }
    /* ---- life cycle / errors ---- */
    for (unsigned entries = 0; entries < 2; entries++) {
        reset();
        q8b_gpi_dma_t d;
        q8b_gpi_rings_t r = { 8, 32, 16, 0x100000, 0x200000 };
        assert(q8b_gpi_dma_init(&d, 0x9000000U, &r, &io) == 0);
        assert(q8b_gpi_dma_start(&d) == -1);        /* not programmed */
        assert(!d.poisoned);
        assert(q8b_gpi_dma_program_rings(&d) == 0);
        /* idempotent re-program before start is allowed */
        assert(q8b_gpi_dma_program_rings(&d) == 0);
        assert(q8b_gpi_dma_program_rings(&(q8b_gpi_dma_t){0}) == -1);
        assert(q8b_gpi_dma_commit(&d, 1) == -1);    /* not started yet */
        assert(q8b_gpi_dma_start(&d) == 0);
        assert(d.started && d.allocated && cmd_count == 4); /* 2 alloc + 2 start */
        assert(q8b_gpi_dma_program_rings(&d) == -1); /* no reconfig while started */
        assert(q8b_gpi_dma_start(&d) == 0);         /* idempotent */
        assert(cmd_count == 4);
        assert(q8b_gpi_dma_commit(&d, 4) == 0);
        assert(d.wp == 4);
        assert(q8b_gpi_dma_commit(&d, 2) == -1);    /* WP goes backwards */
        assert(q8b_gpi_dma_commit(&d, 9) == -1);    /* past the ring end */
        assert(q8b_gpi_dma_commit(&d, 4) == 0);     /* no-op re-commit ok */
        /* event completes */
        uint8_t ev[8 * Q8B_GPI_EVENT_SIZE];
        memset(ev, 0, sizeof(ev));
        build_event(ev, Q8B_GPI_EV_CODE_SUCCESS, Q8B_GPI_EV_TYPE_XFER_COMPLETE, 1, 64);
        mmio[Q8B_GPI_EV_WP_LO / 4u] = 1;   /* hardware completed one event */
        uint32_t code, chid, len;
        assert(q8b_gpi_dma_wait_event(&d, ev, 100000, &code, &chid, &len) == 0);
        assert(code == Q8B_GPI_EV_CODE_SUCCESS && chid == 1 && len == 64);
        assert(d.rp == (1 & 7));
        assert(q8b_gpi_dma_wait_event(&d, ev, 100, NULL, NULL, NULL) == -1); /* timeout */
        assert(d.poisoned);                         /* unknown state */
        (void)entries;
    }
    reset();
    {
        q8b_gpi_dma_t d;
        new_dma(&d, 8);
        assert(q8b_gpi_dma_program_rings(&d) == 0);
        assert(q8b_gpi_dma_start(&d) == 0);
        assert(q8b_gpi_dma_stop(&d) == 0);
        assert(!d.started);
        assert(q8b_gpi_dma_commit(&d, 1) == -1);    /* stopped: no doorbell */
        assert(q8b_gpi_dma_reset(&d) == 0);
        assert(q8b_gpi_dma_dealloc(&d) == 0);
        assert(d.retired && !d.allocated);
        /* retired handle refuses everything */
        assert(q8b_gpi_dma_start(&d) == -1);
        assert(q8b_gpi_dma_stop(&d) == -1);
    }
    /* error events poison */
    for (unsigned code = 0; code < 4; code++) {
        reset();
        q8b_gpi_dma_t d;
        new_dma(&d, 8);
        assert(q8b_gpi_dma_program_rings(&d) == 0);
        assert(q8b_gpi_dma_start(&d) == 0);
        uint8_t ev[8 * Q8B_GPI_EVENT_SIZE];
        memset(ev, 0, sizeof(ev));
        uint32_t ec = code == 0 ? Q8B_GPI_EV_CODE_UNEXP_ERR :
                        code == 1 ? 0x20u : Q8B_GPI_EV_TYPE_STALE;
        build_event(ev, ec, Q8B_GPI_EV_TYPE_XFER_COMPLETE, 0, 1);
        mmio[Q8B_GPI_EV_WP_LO / 4u] = 1;   /* one (bad) event pending */
        assert(q8b_gpi_dma_wait_event(&d, ev, 100000, NULL, NULL, NULL) == -1);
        assert(d.poisoned);
    }
    /* command error paths */
    for (unsigned fail = 1; fail <= 4; fail++) {
        reset();
        q8b_gpi_dma_t d;
        new_dma(&d, 8);
        assert(q8b_gpi_dma_program_rings(&d) == 0);
        no_irq_cmd = fail;
        int r = q8b_gpi_dma_start(&d);
        assert(r == -1 && d.poisoned && cmd_count == fail);
    }
    reset();
    {
        q8b_gpi_dma_t d;
        new_dma(&d, 8);
        assert(q8b_gpi_dma_program_rings(&d) == 0);
        bad_state_cmd = 2;                  /* second command (start ch0) returns wrong state */
        assert(q8b_gpi_dma_start(&d) == -1 && d.poisoned);
    }
    reset();
    {
        /* allocate ok, then a STALE completion IRQ appears before stop */
        q8b_gpi_dma_t d;
        new_dma(&d, 8);
        assert(q8b_gpi_dma_program_rings(&d) == 0);
        assert(q8b_gpi_dma_start(&d) == 0);
        unsigned before = io_writes;
        mmio[Q8B_GPI_CH_IRQ_STS / 4u] = 1;  /* stale completion IRQ appears */
        assert(q8b_gpi_dma_stop(&d) == -1 && d.poisoned);
        assert(io_writes == before);        /* refused before any register write */
    }
    reset();
    {
        /* the context registers may only be written by program_rings, before
         * allocate; a started handle must never rewrite them */
        q8b_gpi_dma_t d;
        new_dma(&d, 8);
        assert(q8b_gpi_dma_program_rings(&d) == 0);
        assert(q8b_gpi_dma_start(&d) == 0);
        unsigned before = io_writes;
        assert(q8b_gpi_dma_program_rings(&d) == -1);
        assert(io_writes == before);
        assert(q8b_gpi_dma_commit(&d, 8) == 0);   /* full ring commit is legal */
        assert(q8b_gpi_dma_commit(&d, 9) == -1);  /* past the end */
    }
    /* ---- SPI transfer TRE sequence layer ---- */
    {
        /* Goodix-style read: 5-byte prefix + dummy + value, all RX after
         * a TX command frame.  Here we just check generic RX and duplex
         * sequences: CONFIG0 -> GO -> DMA_RX / TX,RX with correct flags. */
        uint32_t ring[8 * 8];   /* ring of 8 TREs */
        memset(ring, 0xA5, sizeof(ring));
        q8b_gpi_spi_cfg_t cfg = { 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0 };
        uint32_t wp = 0;
        int slots = q8b_gpi_dma_xfer_slots(&cfg, 0, 12);
        assert(slots == 3);
        assert(q8b_gpi_dma_build_xfer(ring, 8, wp, &cfg,
                0, 0, 0, 0xcafe0000u, 0, 12, &wp) == 0);
        assert(wp == 3);
        uint32_t* c0 = ring + 0u * 8u;
        uint32_t* go = ring + 1u * 8u;
        uint32_t* dma = ring + 2u * 8u;
        assert(((c0[3] & Q8B_GPI_TRE_FLAGS_TYPE) >> 16) == Q8B_GPI_TRE_TYPE_CONFIG0);
        assert(c0[3] & Q8B_GPI_TRE_FLAGS_CHAIN);
        assert(((go[3] & Q8B_GPI_TRE_FLAGS_TYPE) >> 16) == Q8B_GPI_TRE_TYPE_GO);
        assert((go[0] & Q8B_GPI_SPI_GO_CMD) == Q8B_GPI_SPI_CMD_RX);
        assert((go[2] & Q8B_GPI_GO_RX_LEN) == 12u);
        assert(go[3] & Q8B_GPI_TRE_FLAGS_CHAIN);
        assert(((dma[3] & Q8B_GPI_TRE_FLAGS_TYPE) >> 16) == Q8B_GPI_TRE_TYPE_DMA);
        assert(dma[0] == 0xcafe0000u);
        assert((dma[2] & Q8B_GPI_DMA_LEN) == 12u);
        assert(dma[3] & Q8B_GPI_TRE_FLAGS_IEOT);
        assert(dma[3] & Q8B_GPI_TRE_FLAGS_BEI);
        /* full duplex: CONFIG0 -> GO -> DMA_TX -> DMA_RX */
        cfg.cmd = Q8B_GPI_SPI_CMD_DUPLEX;
        assert(q8b_gpi_dma_xfer_slots(&cfg, 5, 12) == 4);
        assert(q8b_gpi_dma_build_xfer(ring, 8, 0, &cfg,
                0x11000000, 0, 5, 0x22000000u, 0, 12, &wp) == 0);
        assert(wp == 4);
        uint32_t* txd = ring + 2u * 8u;
        uint32_t* rxd = ring + 3u * 8u;
        assert(((go[3] & Q8B_GPI_TRE_FLAGS_TYPE) >> 16) == Q8B_GPI_TRE_TYPE_GO);
        assert((go[0] & Q8B_GPI_SPI_GO_CMD) == Q8B_GPI_SPI_CMD_DUPLEX);
        assert(txd[0] == 0x11000000u && (txd[2] & Q8B_GPI_DMA_LEN) == 5u);
        assert(!(txd[3] & Q8B_GPI_TRE_FLAGS_BEI));
        assert(rxd[0] == 0x22000000u && (rxd[2] & Q8B_GPI_DMA_LEN) == 12u);
        assert(rxd[3] & Q8B_GPI_TRE_FLAGS_BEI);
        assert(rxd[3] & Q8B_GPI_TRE_FLAGS_IEOT);
        /* space and argument guards */
        assert(q8b_gpi_dma_build_xfer(ring, 8, 6, &cfg,
                0, 0, 5, 0, 0, 12, &wp) == -1);   /* partial ring */
        assert(q8b_gpi_dma_xfer_slots(&cfg, 0, 0) == -1);
        assert(q8b_gpi_dma_xfer_slots(NULL, 1, 1) == -1);
        assert(q8b_gpi_dma_build_xfer(NULL, 8, 0, &cfg, 0, 0, 5, 0, 0, 12, &wp) == -1);
        assert(q8b_gpi_dma_build_xfer(ring, 9, 0, &cfg, 0, 0, 5, 0, 0, 12, &wp) == -1);
    }
    puts("q8b GPI DMA tests: ok");
    return 0;
}