#define Q8B_SPI_TEST
#include "../src/spi.c"
#include <assert.h>
#include <stdio.h>

static uint32_t gcc[0x100000 / 4], se[0x4000 / 4], tlmm[0x300000 / 4];
static uint32_t rx_bytes, command;
/* Emulated MISO stream: the engine accumulates every byte it shifts out and
 * the driver pops it back word by word (a loopback device). */
static uint8_t rx_bytes_buf[8192];
static uint32_t rx_shifted;   /* bytes the engine has shifted out */
static uint32_t rx_poped;     /* bytes already moved out by the driver */

static unsigned written, wanted, opcode, se_accesses, maps;
static unsigned vote_writes;
static uint64_t now;
static bool done, stalled, illegal, short_rx, bad_abort, stall_command;
/* TX FIFO occupancy: words written by the driver minus words the engine has
 * shifted out.  TX_LEN is the byte count the engine clocks, so an RX-only
 * command that leaves TX_LEN at 0 clocks nothing. */
static unsigned tx_words_in, tx_words_out;
static uint32_t* bank(uintptr_t base) {
    if (base == GCC_VA) return gcc;
    if (base == SE_VA) { se_accesses++; return se; }
    if (base == TLMM_VA) return tlmm;
    assert(base == WRAPPER_VA);
    static uint32_t wrapper[0x2000 / 4];
    wrapper[1] = 0x30000000;
    return wrapper;
}
static uint32_t hw_rd(uintptr_t base, unsigned off) {
    assert(base != SE_VA || off < 0x1000U);
    uint32_t* regs = bank(base);
    if (base != SE_VA) return regs[off / 4];
    if (off == STATUS) return (stalled || (command && !done)) ? ACTIVE : 0;
    if (off == M_IRQ) {
        if (illegal && command) return 4;
        return done ? CMD_DONE : 0;
    }
    if (off == TX_STATUS) return tx_words_in > tx_words_out
            ? tx_words_in - tx_words_out : 0;
    if (off == RX_STATUS) {
        /* Every word the engine shifts lands in the FIFO immediately (four
         * bytes, or fewer for the final partial word).  RX_STATUS[30:28] is
         * the valid-byte count of the LAST word. */
        uint32_t effective = rx_shifted - rx_poped;
        uint32_t v = (effective + 3U) / 4U;
        uint32_t valid = effective % 4U;
        if (valid) v |= (1U << 31) | (valid << 28);
        return v;
    }
    if (off == RX_FIFO) {
        uint32_t effective = rx_shifted - rx_poped;
        assert(effective);
        uint32_t take = effective < 4U ? effective : 4U;
        uint32_t word = 0;
        for (uint32_t k = 0; k < take; k++)
            word |= (uint32_t)rx_bytes_buf[rx_poped + k] << (k * 8);
        rx_poped += take;
        return word;
    }
    return regs[off / 4];
}
static void hw_wr(uintptr_t base, unsigned off, uint32_t val) {
    assert(base != SE_VA || off < 0x1000U);
    uint32_t* regs = bank(base);
    if (base == GCC_VA && (off == QUP_VOTE || off == SE6_VOTE))
        vote_writes++;
    regs[off / 4] = val;
    if (base == GCC_VA && off == ROOT) regs[off / 4] &= ~1U;
    if (base != SE_VA) return;
    if (off == M_CTRL && !bad_abort) { stalled = false; command = 0; }
    if (off == M_CMD) {
        command = val;
        opcode = val >> 27;
        /* The SPI sequencer clocks exactly TX_LEN bytes.  One FIFO word
         * carries four bytes, so a word handed over for a short tail is
         * only partly used. */
        wanted = regs[TX_LEN / 4];
        written = rx_bytes = rx_shifted = rx_poped = 0;
        done = false;
        if (stall_command) stalled = true;
    }
    if (off == TX_FIFO && command && !stalled && !illegal) {
        /* One FIFO word = four clocked bytes; a tail word still consumes
         * the remaining 1..3 bytes.  Every word the engine shifts with RX
         * enabled enqueues one RX word; RX_STATUS[30:28] says how many bytes
         * of the last one are valid. */
        unsigned bytes = written < wanted
                ? ((wanted - written) < 4U ? wanted - written : 4U) : 0;
        written += bytes;
        tx_words_in++;
        tx_words_out++;
        if (opcode & 2) {
            /* Each shifted word yields one RX word; the tail may be partial,
             * and RX_STATUS[30:28] reports how many of its bytes are valid.
             * The word is visible as soon as it is shifted (real FIFOs do not
             * wait for all four bytes). */
            if (!short_rx || written < wanted) {
                /* Loopback: MISO carries the same bytes MOSI did. */
                for (unsigned k = 0; k < bytes; k++) {
                    uint32_t pos = written - bytes + k;
                    assert(pos < sizeof(rx_bytes_buf));
                    rx_bytes_buf[pos] = (uint8_t)(val >> (k * 8));
                }
                rx_shifted = written;
            }
        }
        done = written == wanted;
    }
}
static int hw_map(uintptr_t va, uintptr_t pa, unsigned size) {
    (void)va; (void)pa; (void)size; maps++; return 0;
}
static void hw_delay(unsigned us) { now += us; }
static uint64_t hw_time(void) { return now++; }
static void reset(void) {
    memset(gcc, 0, sizeof(gcc)); memset(se, 0, sizeof(se));
    memset(tlmm, 0, sizeof(tlmm));
    se[FW_REVISION / 4] = 0x102;
    se[HW_PARAM0 / 4] = (32U << 24) | (16U << 16);
    se[HW_PARAM1 / 4] = 16U << 16;
    command = rx_bytes = rx_shifted = rx_poped = 0;
    memset(rx_bytes_buf, 0, sizeof(rx_bytes_buf));
    tx_words_in = tx_words_out = 0;
    done = stalled = illegal = short_rx = bad_abort = stall_command = false;
    now = maps = se_accesses = vote_writes = 0;
}
int main(void) {
    q8b_spi_t spi, m3;
    reset();
    assert(q8b_spi_prepare(&spi, 21, 2400000) != 0 && maps == 0);
    assert(q8b_spi_open(&spi, 22, 0, 0) != 0 && maps == 0);
    assert(q8b_spi_open(&spi, 22, 2400000, 32) != 0 && maps == 0);
    gcc[0x1e864 / 4] = CLOCK_OFF;
    assert(q8b_spi_open(&spi, 22, 2400000, 0) != 0);
    assert(se_accesses == 0); /* Never touch an unclocked serial engine. */

    reset();
    gcc[0x1e008 / 4] = CLOCK_OFF | 2U; /* AHB idle under HWCG is valid. */
    gcc[QUP_VOTE / 4] = 1U << 5; /* retain UART17 vote */
    gcc[(ROOT + 0x14) / 4] = 1;
    for (unsigned i = 0; i < 8; i++) gcc[(ROOT + 0x1c) / 4 + i] = 0x100;
    gcc[(ROOT + 0x1c) / 4 + 3] = 1;
    assert(q8b_spi_open(&spi, 22, 2400000, 0) == 0);
    assert(spi.clock_index == 3 && spi.speed_hz == 2400000);
    assert(gcc[QUP_VOTE / 4] == ((1U << 5) | 15U));
    assert(gcc[ROOT / 4] == 0); /* DFS root was not rewritten. */
    assert(vote_writes == 2);
    q8b_spi_t prepared;
    assert(q8b_spi_prepare(&prepared, 22, 2400000) == 0 && vote_writes == 2);
    assert(se[TX_PACK0 / 4] == 0x7f8fe && se[TX_PACK1 / 4] == 0xffefe);
    uint8_t tx[1025], rx[1025];
    for (unsigned i = 0; i < sizeof(tx); i++) tx[i] = (uint8_t)(i * 37 + 19);
    const unsigned sizes[] = {1, 3, 4, 13, 64, 65, 257, 1025};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        memset(rx, 0, sizeof(rx));
        assert(q8b_spi_transfer(&spi, tx, rx, sizes[i]) == 0);
        assert(opcode == 3 && memcmp(tx, rx, sizes[i]) == 0);
    }
    assert(q8b_spi_transfer(&spi, tx, NULL, 13) == 0 && opcode == 1);
    /* RX-only read: every byte must be clocked and the RX data returned.
     * The engine clocks TX_LEN bytes and only produces RX data for bytes it
     * actually shifted, so an RX-only command that leaves TX_LEN 0 completes
     * at once with nothing received. */
    memset(rx, 0, sizeof(rx));
    assert(q8b_spi_transfer(&spi, NULL, rx, 1025) == 0);
    /* The mock is a loopback device: it returns what was clocked out, so an
     * RX-only transfer reads back the 0xff filler it drove.  What matters is
     * that all 1025 bytes came back (TX_LEN was len, not 0). */
    assert(opcode == 2 && spi.received == 1025);
    for (unsigned i = 0; i < 1025; i++)
        assert(rx[i] == 0xffU);
    /* A Himax-style read: 3-byte header + 5132-byte frame in one CS window,
     * the size the touch driver reads per frame. */
    static uint8_t frame_tx[3 + 5132], frame_rx[3 + 5132];
    for (unsigned i = 0; i < sizeof(frame_tx); i++) frame_tx[i] = (uint8_t)(i * 7 + 3);
    memset(frame_rx, 0, sizeof(frame_rx));
    assert(q8b_spi_transfer(&spi, frame_tx, frame_rx, sizeof(frame_rx)) == 0);
    assert(memcmp(frame_tx, frame_rx, sizeof(frame_rx)) == 0);
    /* Zero TX words outstanding: the polled loop never sees a stuck FIFO. */
    assert(tx_words_in == tx_words_out);
    assert(q8b_spi_transfer(&spi, tx, rx, 0) != 0);
    assert(q8b_spi_transfer(&spi, NULL, NULL, 13) != 0);
    assert(q8b_spi_transfer(&spi, tx, rx, Q8B_SPI_MAX_XFER + 1) != 0);
    memcpy(rx, tx, sizeof(tx));
    assert(q8b_spi_transfer(&spi, rx, rx, sizeof(rx)) == 0);
    assert(memcmp(tx, rx, sizeof(rx)) == 0);

    short_rx = true;
    assert(q8b_spi_transfer(&spi, tx, rx, 13) != 0 && !spi.ready);
    /* The failed command must not leave RX words behind for the next one. */
    assert((hw_rd(SE_VA, RX_STATUS) & 0x1ffffffU) == 0);
    reset();
    assert(q8b_spi_open(&spi, 22, 2400000, 0) == 0);
    illegal = true;
    assert(q8b_spi_transfer(&spi, tx, rx, 13) != 0 && !spi.ready);
    reset();
    assert(q8b_spi_open(&spi, 22, 2400000, 0) == 0);
    /* Start idle, then emulate a sequencer that never completes or aborts. */
    stall_command = bad_abort = true;
    assert(q8b_spi_transfer(&spi, tx, rx, 13) != 0 && !spi.ready);
    assert(now < 50000); /* transfer and recovery are both bounded */
    reset();
    se[IF_DISABLE / 4] = 1;
    assert(q8b_spi_open(&spi, 22, 2400000, 0) != 0 && !spi.ready);
    assert(strcmp(spi.stage, "fifo-disabled-requires-gpi") == 0);
    /* ---- gaokun3 QUP0 SE6 (bus 60) ---- */
    reset();
    gcc[0x17008 / 4] = CLOCK_OFF | 2U;    /* S AHB idle under HWCG is valid */
    gcc[0x52008 / 4] = 1u << 5;           /* retain an unrelated vote bit */
    assert(q8b_spi_prepare(&prepared, 60, 19200000) != 0);  /* > 12 MHz */
    assert(q8b_spi_prepare(&prepared, 60, 3000000) == 0);
    /* vote register 0x52008 gains M(6)|S(7)|S6(16) without losing bit 5 */
    assert((gcc[0x52008 / 4] & 0x100c0u) == 0x100c0u);
    assert((gcc[0x52008 / 4] & (1u << 5)) != 0u);
    assert(q8b_spi_open(&spi, 60, 3000000, 0) == 0);
    assert(spi.se_pa == 0x00998000U);
    assert(spi.speed_hz <= 12000000U);
    /* qup6 function 1 on gpio154-157 */
    for (unsigned pin = 154; pin <= 157; pin++)
        assert(((tlmm[pin * 0x1000u / 4u] >> 2) & 15u) == 1u);
    memset(rx, 0, sizeof(rx));
    assert(q8b_spi_transfer(&spi, tx, rx, 13) == 0 && memcmp(tx, rx, 13) == 0);
    /* SPI mode 3 on bus 60: CPHA=1, CPOL=1 (0x230 bit2). */
    reset();
    gcc[0x17008 / 4] = CLOCK_OFF | 2U;
    assert(q8b_spi_open(&m3, 60, 3000000, 3) == 0);
    assert(se[CPHA / 4] == 1u && se[CPOL / 4] == 4u);
    /* Software CS: the CS pad (gpio157) must stay a plain GPIO while the
     * engine takes MISO/MOSI/CLK, and the engine's demux must not point at
     * the caller-owned channel. */
    reset();
    gcc[0x17008 / 4] = CLOCK_OFF | 2U;
    tlmm[157 * 0x1000u / 4u] = 0x6u;   /* pull-up, fn0, input: caller's pin */
    q8b_spi_t swcs;
    assert(q8b_spi_open(&swcs, 60, 3000000,
                Q8B_SPI_CPHA | Q8B_SPI_CPOL | Q8B_SPI_SW_CS) == 0);
    assert(swcs.sw_cs);
    for (unsigned pin = 154; pin <= 156; pin++)
        assert(((tlmm[pin * 0x1000u / 4u] >> 2) & 15u) == 1u);
    assert(tlmm[157 * 0x1000u / 4u] == 0x6u);   /* untouched */
    assert(se[CS_SELECT / 4] != 0u);
    /* ... and without the flag CS is muxed to qup6 like the others. */
    reset();
    gcc[0x17008 / 4] = CLOCK_OFF | 2U;
    assert(q8b_spi_open(&m3, 60, 3000000, 3) == 0);
    assert(!m3.sw_cs);
    assert(((tlmm[157 * 0x1000u / 4u] >> 2) & 15u) == 1u);
    assert(se[CS_SELECT / 4] == 0u);

    /* FIFO-disabled engine is refused on both buses. */
    reset();
    se[IF_DISABLE / 4] = 1;
    assert(q8b_spi_open(&spi, 60, 3000000, 0) != 0);
    assert(spi.stage != 0 && ~0);
    puts("q8b SPI controller tests: ok");
    return 0;
}
