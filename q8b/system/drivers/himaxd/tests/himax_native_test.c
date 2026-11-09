/* Host test for the Himax native-SPI (QUP0 SE6, mode 3) mapping layer.
 * Uses the GPI DMA TRE builders from bsp/gpi_dma.c via the include trick,
 * plus the frame constants from himax_protocol.h. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../../libs/bsp/include/bsp/gpi_dma.h"
#include "../himax_protocol.h"
#include "../native/himax_native_spi.h"
#define Q8B_GPI_DMA_TEST
#include "../../../libs/bsp/src/gpi_dma.c"

int main(void) {
    /* Mode 3 config: CPOL=1, CPHA=1, packed, CS deasserted, div ~ 2. */
    q8b_gpi_spi_cfg_t cfg;
    uint32_t hz = 0;
    assert(himax_native_spi_cfg(&cfg, &hz) == 0);
    assert(cfg.clock_pol_high == 1 && cfg.data_pol_high == 1);
    assert(cfg.clk_src == 0 && cfg.cs == 0 && cfg.fragmentation == 0);
    assert(cfg.cmd == Q8B_GPI_SPI_CMD_DUPLEX);
    assert(cfg.clk_div >= 1 && cfg.clk_div <= 4095);
    assert(hz > 0 && hz < HIMAX_SRC_HZ);
    assert(himax_native_spi_cfg(NULL, NULL) == -1);

    /* Himax read frame: [0xf3][cmd][dummy] + payload, 4-byte reg read. */
    uint8_t frame[7];
    frame[0] = HIMAX_SPI_FUNCTION_READ;      /* 0xf3 */
    frame[1] = 0x01;                          /* cmd */
    frame[2] = 0xff;                          /* dummy */
    memset(frame + 3, 0, 4);                  /* payload (rx data slots) */
    const uint32_t fl = HIMAX_BUS_R_HLEN + 4; /* 7 bytes */

    uint32_t ring[4 * 8];                    /* 4 TREs max */
    uint32_t wp = 0;
    assert(fl == 7u);
    assert(himax_native_build_xfer(ring, 4, wp, frame, fl,
            0x1000, 0x80000000u, 0x2000, 0x80000000u, &wp) == 0);
    assert(wp == 4);                          /* CONFIG0+GO+TX+RX */
    uint32_t* c0 = ring + 0u * 8u;
    uint32_t* go = ring + 1u * 8u;
    uint32_t* dtx = ring + 2u * 8u;
    uint32_t* drx = ring + 3u * 8u;
    assert(((c0[3] & Q8B_GPI_TRE_FLAGS_TYPE) >> 16) == Q8B_GPI_TRE_TYPE_CONFIG0);
    /* mode 3 must be encoded in CONFIG0: CPOL=1, CPHA=1, packed */
    assert(c0[0] & Q8B_GPI_SPI_C0_CPOL);
    assert(c0[0] & Q8B_GPI_SPI_C0_CPHA);
    assert(c0[0] & (Q8B_GPI_SPI_C0_TX_PACK | Q8B_GPI_SPI_C0_RX_PACK));
    assert(((go[3] & Q8B_GPI_TRE_FLAGS_TYPE) >> 16) == Q8B_GPI_TRE_TYPE_GO);
    assert((go[0] & Q8B_GPI_SPI_GO_CMD) == Q8B_GPI_SPI_CMD_DUPLEX);
    assert((go[2] & Q8B_GPI_GO_RX_LEN) == fl);
    assert(dtx[0] == 0x1000u && (dtx[2] & Q8B_GPI_DMA_LEN) == fl);
    assert(!(dtx[3] & Q8B_GPI_TRE_FLAGS_BEI));
    assert(drx[0] == 0x2000u && (drx[2] & Q8B_GPI_DMA_LEN) == fl);
    assert(drx[3] & Q8B_GPI_TRE_FLAGS_BEI);
    assert(drx[3] & Q8B_GPI_TRE_FLAGS_IEOT);

    /* guards */
    assert(himax_native_build_xfer(ring, 4, 1, frame, fl,
            0, 0, 0, 0, &wp) == -1);          /* partial ring */
    assert(himax_native_build_xfer(ring, 4, 0, NULL, fl,
            0, 0, 0, 0, &wp) == -1);

    /* write frame: [0xf2][cmd][4 addr][payload] */
    memset(frame, 0, sizeof(frame));
    frame[0] = HIMAX_SPI_FUNCTION_WRITE;
    frame[1] = 0x02;
    const uint32_t wl = HIMAX_BUS_W_HLEN + 4 + 2;
    wp = 0;
    assert(wl == 8u);
    assert(himax_native_build_xfer(ring, 4, 0, frame, wl,
            0x3000, 0, 0x4000, 0, &wp) == 0);
    assert(wp == 4);

    printf("himax native SPI mapping tests: ok\n");
    return 0;
}