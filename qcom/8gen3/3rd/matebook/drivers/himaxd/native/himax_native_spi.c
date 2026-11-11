#include "himax_native_spi.h"
#include <string.h>

int himax_native_spi_cfg(q8b_gpi_spi_cfg_t* cfg, uint32_t* out_hz) {
    if (!cfg)
        return -1;
    memset(cfg, 0, sizeof(*cfg));
    cfg->word_sz = 0;                       /* 8-bit */
    cfg->loopback = 0;
    cfg->clock_pol_high = 1;                /* CPOL 1 (idle high) */
    cfg->data_pol_high = 1;                 /* CPHA 1 (mode 3) */
    cfg->pack_en = 1;                       /* packed 8-bit transfers */
    cfg->cs = 0;
    cfg->fragmentation = 0;                 /* deassert CS after each access */
    cfg->cmd = Q8B_GPI_SPI_CMD_DUPLEX;
    /* Nearest integer divider: 19.2/12 = 1.6 -> 2 => 9.6 MHz safe upper
     * bound?  Himax data sheet allows >= ~3 MHz; the Linux driver uses
     * 12 MHz via a non-integer ratio.  Use div 2 and report the real rate. */
    uint32_t div = (HIMAX_SRC_HZ + HIMAX_SPI_HZ * 2U - 1U) / (HIMAX_SPI_HZ * 2U);
    if (div < 1 || div > 4095)
        return -1;
    cfg->clk_div = (uint16_t)div;
    cfg->clk_src = 0;                       /* TCXO table entry 0 */
    if (out_hz)
        *out_hz = HIMAX_SRC_HZ / (div * 2U);
    return 0;
}

int himax_native_build_xfer(uint32_t* ring, uint32_t entries, uint32_t wp,
                            const uint8_t* frame, uint32_t frame_len,
                            uint32_t tx_lo, uint32_t tx_hi, uint32_t rx_lo,
                            uint32_t rx_hi, uint32_t* out_wp) {
#ifdef HIMAX_NATIVE_NO_GPI
    /* FIFO-only deployment build: the GPI DMA path is not linked. */
    (void)ring; (void)entries; (void)wp; (void)frame; (void)frame_len;
    (void)tx_lo; (void)tx_hi; (void)rx_lo; (void)rx_hi; (void)out_wp;
    return -1;
#else
    if (!frame || frame_len == 0 || !out_wp)
        return -1;
    q8b_gpi_spi_cfg_t cfg;
    uint32_t hz;
    if (himax_native_spi_cfg(&cfg, &hz) != 0)
        return -1;
    cfg.rx_len = frame_len;                 /* full-duplex: same length */
    return q8b_gpi_dma_build_xfer(ring, entries, wp, &cfg,
            tx_lo, tx_hi, frame_len, rx_lo, rx_hi, frame_len, out_wp);
#endif
}