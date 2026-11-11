#ifndef HIMAX_NATIVE_SPI_H
#define HIMAX_NATIVE_SPI_H
#include <bsp/gpi_dma.h>

/*
 * Native-SPI bring-up layer for the gaokun3 Himax HX83121A (host tests only,
 * not linked into himaxd).  Maps QUP0 SE6 constants and the SPI mode-3
 * configuration onto the host-tested GPI DMA TRE sequence builder.  No MMIO
 * here: everything is pure encoding, so the host test can validate the frame
 * -> TRE mapping before any hardware write is attempted.
 *
 * Hardware facts (sc8280xp.dtsi + gcc-sc8280xp.c + gaokun3 board):
 *  - SE6 @ 0x00998000, wrapper qup0 @ 0x009c0000
 *  - GCC votes via 0x52008 (M BIT6 / S BIT7 / S6 BIT16), halts
 *    0x17004 / 0x17008 / 0x17864; gpi_dma0 @ 0x900000, SID 0x576
 *  - pins gpio154/155/156/157 as qup6 function 1 (PINGROUP index 0)
 *  - SPI mode 3 (CPOL=1, CPHA=1), 8-bit, 12 MHz in Linux; CS asserted
 *    per access, host-interface select gpio174 low selects SPI.
 *  - Himax read frame is [0xf3][cmd][dummy(0xff)] then len payload bytes;
 *    write is [0xf2][cmd][4-byte addr] then payload.  The bus layer in
 *    himax_protocol.c drives these through a himax_bus_t.xfer callback.
 */
#define HIMAX_Q0SE6_SE_PA      0x00998000U
#define HIMAX_Q0SE6_WRAPPER_PA 0x009c0000U
#define HIMAX_Q0SE6_GCC_PA     0x00100000U
#define HIMAX_Q0SE6_GPI_SID    0x576U
#define HIMAX_PIN_MISO 154U
#define HIMAX_PIN_MOSI 155U
#define HIMAX_PIN_CLK  156U
#define HIMAX_PIN_CS   157U
#define HIMAX_QUP6_MUX 1U         /* qup6 = PINGROUP function 1 */
#define HIMAX_SPI_HZ   12000000U
#define HIMAX_SRC_HZ   19200000U

/* Fill the GPI DMA SPI config for Himax mode 3 (CPOL=1, CPHA=1), packed
 * 8-bit, CS0.  clk_div selects the nearest integer divider of the 19.2 MHz
 * source; 12/19.2 is not exact, so return the real speed in *out_hz. */
int himax_native_spi_cfg(q8b_gpi_spi_cfg_t* cfg, uint32_t* out_hz);

/* Encode one Himax bus transfer (tx_len frame bytes in, rx_len=tx_len for a
 * full-duplex bus read; the payload is at frame offset HIMAX_BUS_R_HLEN)
 * into the descriptor ring as CONFIG0 -> GO(DUPLEX) -> DMA_TX -> DMA_RX.
 * tx/rx must both be non-zero (the bus is full duplex). */
int himax_native_build_xfer(uint32_t* ring, uint32_t entries, uint32_t wp,
                            const uint8_t* frame, uint32_t frame_len,
                            uint32_t tx_lo, uint32_t tx_hi, uint32_t rx_lo,
                            uint32_t rx_hi, uint32_t* out_wp);

#endif