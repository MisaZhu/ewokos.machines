#ifndef UC_HVS_H
#define UC_HVS_H

#include <stdint.h>

/*
 * HVS (Hardware Video Scaler) — BCM2711 (HVS5) is at 0xfe400000, that
 * is at offset 0x400000 inside the mmio window.  DSI1 gets its pixels
 * from PixelValve1, which pulls from HVS channel 1's FIFO.
 */
#define UC_HVS_OFFSET   0x00400000U

int uc_hvs_bringup(uint32_t phy_fb, uint32_t width, uint32_t height,
		uint32_t depth);

/*
 * Read an arbitrary HVS register (offset within the HVS block, e.g.
 * SCALER_DISPCTRL1=0x50). Returns 0 if MMIO isn't mapped yet.
 * Exposed strictly for the observability log so we can dump whatever
 * state stage3.bin/VC firmware left behind before we touch anything.
 */
uint32_t uc_hvs_read_raw(uint32_t off);

#endif
