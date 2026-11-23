#ifndef ORIN_BSP_VIC_H
#define ORIN_BSP_VIC_H

#include <stdint.h>
#include <ewoksys/ewokdef.h>

/*
 * Tegra234 VIC (Video Image Compositor) 2D engine bring-up, milestone 1:
 * power -> CCPLEX reachability -> host1x channel/syncpt -> plaintext
 * vic.bin falcon boot -> method routing.  All userspace, polled, klog
 * evidence at every step.  Sequences mirror mainline host1x08 +
 * drm/tegra vic/falcon and Orin TRM DP-10508-002 §7.3.
 *
 * Each step returns 0 on success, -1 on failure (reason klogged).
 * Steps are cumulative: later steps map/power what they need, so a
 * probe can run any single step cold.
 */

/* BPMP: PG_VIC on, CLK_HOST1X + CLK_VIC enable, RESET_VIC deassert. */
int bsp_vic_power_on(void);

/* Read falcon CPUCTL/IDLESTATE/DMACTL -- proves CCPLEX can reach VIC
 * (a firewall would SError here, as DCE does).  Powers + maps first. */
int bsp_vic_regs_probe(void);

/* host1x: read-only SMMU stream-policy check for the VIC/host1x SIDs
 * (fails unless both bypass: every buffer is a physical address), then
 * hypervisor SID/class/mmio VM tables, syncpt channel protection and the
 * channel's fetch stream ID. */
int bsp_vic_host1x_init(void);

/* Syncpoint CPU_INCR round-trip on a spare syncpt (no channel needed). */
int bsp_vic_syncpt_test(void);

/* Host1x-class pushbuffer through a spare channel: INCR_SYNCPT
 * cond=IMMEDIATE, poll syncpt + command FIFO.  No VIC involvement. */
int bsp_vic_channel_test(void);

/* Load vic.bin (bin-header-v1, plaintext) into the falcon via DMA,
 * STARTCPU, wait IDLESTATE==0, then one MMIO Nop method. */
int bsp_vic_falcon_boot(const char* fw_path);

/* Channel pushbuffer in VIC class with the mainline job framing: MLOCK
 * acquire, ENGINE_IDLE fence + 32-bit syncpt wait, a Nop method via
 * the THI METHOD0/1 pair, second idle fence, MLOCK release.  Boots the
 * falcon first when needed. */
int bsp_vic_method_test(void);

/*
 * Milestone 2: ConfigStruct-based 2D ops (TRM §7.3.2).  All surfaces are
 * pitch-linear ARGB8888 addressed by PHYSICAL base (SMMU bypass): base
 * and pitch 256B aligned (hard engine requirements).  Heights that are a
 * multiple of 4 lines and even rect coordinates are always accepted;
 * other heights and odd rects only when the selftest proved them (never
 * for rotation), else the op declines with -1.  The kernel maps
 * contig shm canvases, the DMA pool and the scan-out FB uncached, so no
 * cache maintenance is needed.  Ops are serialized internally and may be
 * called from several threads.
 *
 * Op return codes: 0 = done (syncpt retired, firmware status 0), -1 =
 * declined before submission or rejected by the firmware (the engine is
 * idle, CPU replay safe), -3 = submitted but not retired (terminal: the
 * engine may still own the surface, never replay).
 */

/* Full bring-up (power -> regs -> host1x -> syncpt -> channel -> falcon
 * boot -> method routing) plus a DMA readback selftest that also settles
 * the encodings only hardware can (pixel format, chroma convention,
 * alpha-fill mode).  Idempotent + latched; returns 0 when fill is pixel
 * exact.  The alpha-exact copy, the colour-exact copy and rotation are
 * latched separately: an op that was not proven declines with -1. */
int bsp_vic_2d_enable(void);

/* 1 when the selftest also proved 90/180/270 rotation pixel exact. */
int bsp_vic_2d_can_rotate(void);

/* Latch query: 1 after bsp_vic_2d_enable() success, else 0.  No probe. */
int bsp_vic_2d_available(void);

/* VIC clock rate in Hz as confirmed at bring-up (0 when unconfirmed). */
uint32_t bsp_vic_2d_clock_hz(void);

/* Solid fill of a sub-rect with the VIC background colour (no slot).
 * dst_pitch in bytes; rect must be inside the surface.  Color is
 * 0xAARRGGBB, written verbatim; only alpha 0xff is selftest-proven. */
int bsp_vic_fill(ewokos_addr_t dst_phy, uint32_t dst_pitch,
		int32_t dst_w, int32_t dst_h,
		int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);

/* 1:1 copy of a src crop to (dx,dy) of dst, optionally rotated clockwise
 * by degree (0/90/180/270) via output flip/transpose.  The rotated crop
 * lands with its top-left at (dx,dy); dst must have room for the rotated
 * (sw,sh).  No scaling, no blending: pixels (incl. alpha) copy verbatim.
 * Overlapping src/dst surfaces are declined, as is any rotation when
 * bsp_vic_2d_can_rotate() is 0. */
int bsp_vic_blit(ewokos_addr_t src_phy, uint32_t src_pitch,
		int32_t src_w, int32_t src_h,
		int32_t sx, int32_t sy, int32_t sw, int32_t sh,
		ewokos_addr_t dst_phy, uint32_t dst_pitch,
		int32_t dst_w, int32_t dst_h,
		int32_t dx, int32_t dy, int32_t degree);

/* Same op for targets whose alpha byte is not displayed (the scan-out
 * framebuffer): colour bytes copy verbatim, the alpha byte is whatever the
 * selftest-chosen mode produces. */
int bsp_vic_blit_xrgb(ewokos_addr_t src_phy, uint32_t src_pitch,
		int32_t src_w, int32_t src_h,
		int32_t sx, int32_t sy, int32_t sw, int32_t sh,
		ewokos_addr_t dst_phy, uint32_t dst_pitch,
		int32_t dst_w, int32_t dst_h,
		int32_t dx, int32_t dy, int32_t degree);

#endif
