#include <bsp/gpi_dma.h>
#include <string.h>

/*
 * Host-tested GPI DMA candidate (see gpi_dma.h).  No direct MMIO here: every
 * register access goes through q8b_gpi_io_t; the host test supplies a
 * simulated GPI register file and rings.  Command acknowledgement requires a
 * fresh control-IRQ bit (stale IRQs poison the handle); DEALLOC may leave the
 * hardware CHSTATE unchanged, so it is acknowledged by its IRQ alone.
 */

static int dma_alive(const q8b_gpi_dma_t* dma) {
    return dma && dma->base && !dma->poisoned && !dma->retired;
}

static int check_rings(const q8b_gpi_rings_t* rings) {
    if (!rings || rings->entries < 2 || (rings->entries & (rings->entries - 1)))
        return -1;                          /* power-of-two ring */
    if (rings->el_size != Q8B_GPI_TRE_SIZE || rings->er_size != Q8B_GPI_EVENT_SIZE)
        return -1;
    return (rings->desc_phys && rings->ev_phys) ? 0 : -1;
}

int q8b_gpi_dma_init(q8b_gpi_dma_t* dma, uintptr_t regs,
                     const q8b_gpi_rings_t* rings, const q8b_gpi_io_t* io) {
    if (!dma || !io || !io->rd || !io->wr || !io->delay)
        return -1;
    if (check_rings(rings) != 0)
        return -1;
    memset(dma, 0, sizeof(*dma));
    dma->io = io;
    dma->base = regs;
    dma->el_size = rings->el_size;
    dma->er_size = rings->er_size;
    dma->entries = rings->entries;
    dma->stage = "initialized";
    return 0;
}

/* Program the channel/event context and ring base/limit words.  Only the
 * el_size/ERIDX/dir/proto bits are set in CNTXT0 (hardware owns CHSTATE).
 * Runs before ALLOCATE/START; nothing here starts a channel. */
int q8b_gpi_dma_program_rings(q8b_gpi_dma_t* dma) {
    if (!dma_alive(dma))
        return -1;
    if (dma->allocated || dma->started)
        return -1;                          /* only before allocate/start */
    for (unsigned ch = 0; ch < Q8B_GPI_CH_MAX; ch++) {
        uint32_t dir = ch == 0 ? 0u : Q8B_GPI_CNTXT0_DIR; /* 0=RX, 1=TX */
        uint32_t c0 = (dma->el_size << 24) | (0u << 14) | dir |
                      Q8B_GPI_CNTXT0_DMA_PROTO;
        dma->io->wr(dma->base, Q8B_GPI_CH_CNTXT0 + ch * 0x80u, c0);
        dma->io->wr(dma->base, Q8B_GPI_CH_LENGTH + ch * 0x80u, dma->entries);
        dma->io->wr(dma->base, Q8B_GPI_CH_BASE_LO + ch * 0x80u, 0); /* set by caller */
        dma->io->wr(dma->base, Q8B_GPI_CH_BASE_HI + ch * 0x80u, 0);
        dma->io->wr(dma->base, Q8B_GPI_CH_WP_LO + ch * 0x80u, 0);
        uint32_t ev_c0 = (dma->er_size << 24) | Q8B_GPI_EV_CNTXT0_INTYPE |
                         2u;               /* inttype=IRQ, chtype=GPI_EV */
        dma->io->wr(dma->base, Q8B_GPI_EV_CNTXT0 + ch * 0x80u, ev_c0);
        dma->io->wr(dma->base, Q8B_GPI_EV_RP_LO + ch * 0x80u, 0);
    }
    dma->programmed = 1;
    dma->stage = "rings-programmed";
    return 0;
}

/* One control command.  opcode: 0=allocate,1=start,2=stop,9=reset,10=dealloc.
 * expected_state is checked against CNTXT0 CHSTATE unless opcode==10 (DEALLOC
 * keeps the hardware CHSTATE).  Acknowledge only a fresh per-channel IRQ. */
static int dma_cmd(q8b_gpi_dma_t* dma, unsigned opcode, unsigned ch,
                   unsigned expected_state) {
    if (!dma_alive(dma))
        return -1;
    if (ch >= Q8B_GPI_CH_MAX)
        return -1;
    uint32_t bit = 1u << ch;
    if (dma->io->rd(dma->base, Q8B_GPI_CH_IRQ_STS) & bit) {
        dma->poisoned = 1;
        dma->stage = "stale-irq";
        return -1;
    }
    dma->io->wr(dma->base, Q8B_GPI_CH_CMD, (opcode << 24) | ch);
    dma->cmd_count++;
    for (unsigned i = 0; i < 1000; i++) {
        if (dma->io->rd(dma->base, Q8B_GPI_CH_IRQ_STS) & bit) {
            uint32_t st = (dma->io->rd(dma->base,
                    Q8B_GPI_CH_CNTXT0 + ch * 0x80u) >> 20) & 15u;
            dma->io->wr(dma->base, Q8B_GPI_CH_IRQ_CLR, bit);
            if ((dma->io->rd(dma->base, Q8B_GPI_CH_IRQ_STS) & bit) ||
                    (opcode != 10 && st != expected_state)) {
                dma->poisoned = 1;
                dma->stage = "cmd-bad-ack";
                return -1;
            }
            return 0;
        }
        dma->io->delay(100);
    }
    dma->poisoned = 1;
    dma->stage = "cmd-timeout";
    return -1;
}

int q8b_gpi_dma_start(q8b_gpi_dma_t* dma) {
    if (!dma_alive(dma))
        return -1;
    if (dma->started)
        return 0;
    if (!dma->programmed)
        return -1;                          /* context/rings first */
    if (dma->allocated)
        return -1;                          /* START once, after ALLOCATE */
    for (unsigned ch = 0; ch < Q8B_GPI_CH_MAX; ch++)
        if (dma_cmd(dma, 0, ch, 1) != 0)    /* allocate -> state 1 */
            return -1;
    dma->allocated = 1;
    for (unsigned ch = 0; ch < Q8B_GPI_CH_MAX; ch++)
        if (dma_cmd(dma, 1, ch, 2) != 0)    /* start -> state 2 */
            return -1;
    dma->started = 1;
    dma->stage = "started";
    return 0;
}

int q8b_gpi_dma_commit(q8b_gpi_dma_t* dma, uint32_t new_wp) {
    /* A doorbell may only be written for a started channel; the new write
     * pointer must stay inside the ring and only move forward. */
    if (!dma_alive(dma) || !dma->started)
        return -1;
    if (new_wp > dma->entries || new_wp < dma->wp)
        return -1;
    for (unsigned ch = 0; ch < Q8B_GPI_CH_MAX; ch++)
        dma->io->wr(dma->base, Q8B_GPI_CH_DOORBELL + ch * 8u, new_wp);
    dma->wp = new_wp;
    dma->stage = "committed";
    return 0;
}

int q8b_gpi_dma_wait_event(q8b_gpi_dma_t* dma, const uint8_t* ev_ring,
                           uint64_t budget_us, uint32_t* out_code,
                           uint32_t* out_chid, uint32_t* out_length) {
    if (!dma_alive(dma) || !dma->started || !ev_ring)
        return -1;
    uint64_t used = 0;
    /* The hardware advances the event ring WP on completion; poll it with a
     * bounded budget (matches the polled style of the other BSP paths). */
    while (used < budget_us) {
        uint32_t ev_wp = dma->io->rd(dma->base, Q8B_GPI_EV_WP_LO);
        if (ev_wp > dma->rp) {
            if (ev_wp <= dma->entries) {
                uint32_t code = 0, type = 0, chid = 0, len = 0;
                const uint8_t* ev = ev_ring + (size_t)dma->rp * dma->er_size;
                if (q8b_gpi_event_decode(ev, &code, &type, &chid, &len) != 0)
                    return -1;
                if (code == Q8B_GPI_EV_CODE_SUCCESS ||
                        code == Q8B_GPI_EV_CODE_EOT) {
                    dma->rp = ev_wp & (dma->entries - 1u);
                    if (out_code) *out_code = code;
                    if (out_chid) *out_chid = chid;
                    if (out_length) *out_length = len;
                    dma->stage = "event-ok";
                    return 0;
                }
                /* Unexpected error event poisons the handle. */
                dma->poisoned = 1;
                dma->stage = "event-error";
                return -1;
            }
            return -1;                      /* firmware wrote an invalid WP */
        }
        dma->io->delay(100);
        used += 100;
    }
    dma->poisoned = 1;                      /* hardware state unknown */
    dma->stage = "event-timeout";
    return -1;
}

int q8b_gpi_dma_stop(q8b_gpi_dma_t* dma) {
    if (!dma_alive(dma))
        return -1;
    if (!dma->started)
        return -1;
    for (unsigned ch = 0; ch < Q8B_GPI_CH_MAX; ch++)
        if (dma_cmd(dma, 2, ch, 3) != 0)    /* stop -> state 3 */
            return -1;
    dma->started = 0;
    dma->stage = "stopped";
    return 0;
}

int q8b_gpi_dma_reset(q8b_gpi_dma_t* dma) {
    if (!dma_alive(dma))
        return -1;
    if (dma->started)
        return -1;
    for (unsigned ch = 0; ch < Q8B_GPI_CH_MAX; ch++)
        if (dma_cmd(dma, 9, ch, 0) != 0)    /* reset -> state 0 */
            return -1;
    dma->stage = "reset";
    return 0;
}

int q8b_gpi_dma_dealloc(q8b_gpi_dma_t* dma) {
    if (!dma_alive(dma))
        return -1;
    if (dma->started || !dma->allocated)
        return -1;
    for (unsigned ch = 0; ch < Q8B_GPI_CH_MAX; ch++)
        if (dma_cmd(dma, 10, ch, 0) != 0)   /* dealloc; IRQ-only ack */
            return -1;
    dma->allocated = 0;
    dma->retired = 1;
    dma->stage = "deallocated";
    return 0;
}

/* --------- TRE builders --------- */

int q8b_gpi_tre_config0(uint32_t* tre, const q8b_gpi_spi_cfg_t* cfg) {
    if (!tre || !cfg || cfg->word_sz > 31)
        return -1;
    memset(tre, 0, Q8B_GPI_TRE_SIZE);
    tre[0] = cfg->word_sz;
    if (cfg->loopback) tre[0] |= Q8B_GPI_SPI_C0_LOOPBACK;
    if (cfg->clock_pol_high) tre[0] |= Q8B_GPI_SPI_C0_CPOL;
    if (cfg->data_pol_high) tre[0] |= Q8B_GPI_SPI_C0_CPHA;
    if (cfg->pack_en) tre[0] |= Q8B_GPI_SPI_C0_TX_PACK | Q8B_GPI_SPI_C0_RX_PACK;
    tre[2] = (cfg->clk_div & Q8B_GPI_SPI_C0_CLK_DIV) |
             ((cfg->clk_src & 0xfu) << 16);
    tre[3] = (Q8B_GPI_TRE_TYPE_CONFIG0 << 16) | Q8B_GPI_TRE_FLAGS_CHAIN;
    return 0;
}

int q8b_gpi_tre_go(uint32_t* tre, const q8b_gpi_spi_cfg_t* cfg,
                   uint32_t rx_len) {
    if (!tre || !cfg || cfg->cmd > Q8B_GPI_SPI_CMD_DUPLEX)
        return -1;
    memset(tre, 0, Q8B_GPI_TRE_SIZE);
    tre[0] = cfg->cmd & Q8B_GPI_SPI_GO_CMD;
    tre[0] |= (cfg->cs & 7u) << 8;
    if (cfg->fragmentation) tre[0] |= Q8B_GPI_SPI_GO_FRAG;
    tre[2] = rx_len & Q8B_GPI_GO_RX_LEN;
    tre[3] = (Q8B_GPI_TRE_TYPE_GO << 16) | Q8B_GPI_TRE_FLAGS_CHAIN;
    return 0;
}

int q8b_gpi_tre_dma(uint32_t* tre, uint32_t addr_lo, uint32_t addr_hi,
                    uint32_t len, bool rx) {
    if (!tre || len == 0 || len > Q8B_GPI_DMA_LEN)
        return -1;
    memset(tre, 0, Q8B_GPI_TRE_SIZE);
    tre[0] = addr_lo;
    tre[1] = addr_hi;
    tre[2] = len;
    tre[3] = (Q8B_GPI_TRE_TYPE_DMA << 16) | Q8B_GPI_TRE_FLAGS_IEOT;
    if (rx)
        tre[3] |= Q8B_GPI_TRE_FLAGS_BEI;
    return 0;
}

int q8b_gpi_event_decode(const uint8_t* ev, uint32_t* code, uint32_t* type,
                         uint32_t* chid, uint32_t* length) {
    /* 16-byte little-endian event: ptr[0..7] length[8..10] code[11]
     * status[12..13] type[14] chid[15]. */
    if (!ev)
        return -1;
    uint32_t len = (uint32_t)ev[8] | ((uint32_t)ev[9] << 8) |
                   ((uint32_t)ev[10] << 16);
    if (code) *code = ev[11];
    if (type) *type = ev[14];
    if (chid) *chid = ev[15];
    if (length) *length = len;
    return 0;
}
int q8b_gpi_dma_xfer_slots(const q8b_gpi_spi_cfg_t* cfg,
                           uint32_t tx_len, uint32_t rx_len) {
    if (!cfg || (tx_len == 0 && rx_len == 0))
        return -1;
    if (tx_len > Q8B_GPI_DMA_LEN || rx_len > Q8B_GPI_DMA_LEN)
        return -1;
    unsigned slots = 2;                    /* CONFIG0 + GO */
    if (tx_len) slots++;
    if (rx_len) slots++;
    return (int)slots;
}

int q8b_gpi_dma_build_xfer(uint32_t* ring, uint32_t entries, uint32_t wp,
                           const q8b_gpi_spi_cfg_t* cfg,
                           uint32_t tx_lo, uint32_t tx_hi, uint32_t tx_len,
                           uint32_t rx_lo, uint32_t rx_hi, uint32_t rx_len,
                           uint32_t* out_wp) {
    int slots = q8b_gpi_dma_xfer_slots(cfg, tx_len, rx_len);
    if (!ring || !cfg || !out_wp || slots < 0 || entries == 0 ||
            (entries & (entries - 1)) || wp + (uint32_t)slots > entries)
        return -1;
    /* Derive the SPI command from the supplied lengths: a caller may pass a
     * GO cmd for duplex, but the TRE lengths must stay consistent. */
    uint32_t seq_cmd = cfg->cmd;
    if (seq_cmd > Q8B_GPI_SPI_CMD_DUPLEX)
        return -1;
    if (seq_cmd == Q8B_GPI_SPI_CMD_DUPLEX && (!tx_len || !rx_len))
        return -1;
    if (seq_cmd != Q8B_GPI_SPI_CMD_DUPLEX) {
        if (tx_len && rx_len)
            seq_cmd = Q8B_GPI_SPI_CMD_DUPLEX;      /* lengths win */
        else if (rx_len)
            seq_cmd = Q8B_GPI_SPI_CMD_RX;
        else
            seq_cmd = Q8B_GPI_SPI_CMD_TX;
    }

    q8b_gpi_spi_cfg_t go = *cfg;
    go.cmd = (uint8_t)seq_cmd;
    if (q8b_gpi_tre_config0(ring + wp * 8u, cfg) != 0)
        return -1;
    if (q8b_gpi_tre_go(ring + (wp + 1u) * 8u, &go, rx_len) != 0)
        return -1;
    unsigned slot = wp + 2u;
    if (tx_len) {
        if (q8b_gpi_tre_dma(ring + slot * 8u, tx_lo, tx_hi, tx_len, false) != 0)
            return -1;
        slot++;
    }
    if (rx_len) {
        if (q8b_gpi_tre_dma(ring + slot * 8u, rx_lo, rx_hi, rx_len, true) != 0)
            return -1;
    }
    *out_wp = wp + (uint32_t)slots;
    return 0;
}
