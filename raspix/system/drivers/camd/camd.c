/*----------------------------------------------------------------------------*/
/**
 * camd - MIPI CSI-2 camera driver for BCM2711 UNICAM0 + OV5647
 *
 * Exposes /dev/cam0 as a character device.
 * read() captures one full frame (blocking) and returns raw image data.
 * fcntl(cmd=0): set mode (proto: width, height)
 * fcntl(cmd=1): get info (proto out: width, height, bpp)
**/
/*----------------------------------------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/vfs.h>
#include <ewoksys/mmio.h>
#include <ewoksys/dma.h>
#include <ewoksys/proc.h>
#include <ewoksys/ipc.h>
#include <ewoksys/proto.h>
#include <ewoksys/syscall.h>
#include <ewoksys/klog.h>
#include <arch/bcm283x/gpio.h>
#include <arch/bcm283x/mailbox.h>
#include <sysinfo.h>

#include "unicam.h"
#include "ov5647.h"

/*----------------------------------------------------------------------------*/
#define CAM_DEFAULT_WIDTH  640
#define CAM_DEFAULT_HEIGHT 480
#define CAM_FRAME_TIMEOUT_MS 500

/* VC bus address alias for DMA (coherent/uncached) */
#define DMA_VC_ALIAS 0xC0000000u

/* Mailbox property tags */
#define MBOX_TAG_SET_GPIO_STATE 0x00038041u
#define MBOX_TAG_SET_DOMAIN_STATE 0x00038030u
#define MBOX_TAG_GET_DOMAIN_STATE 0x00030030u
#define MBOX_VC_ALIAS_NC   0x40000000u

/* firmware power domain IDs = DT binding value + 1 (0 is invalid to fw).
 * dt-bindings/power/raspberrypi-power.h: ISP=11 UNICAM0=12 UNICAM1=13 */
#define PD_UNICAM0 (12u + 1u)
#define PD_UNICAM1 (13u + 1u)
#define PD_ISP     (11u + 1u)

/* firmware expander virtual GPIO: camera enable (FFC pin 11) on Pi4/CM4 */
#define VGPIO_CAM_GPIO 133u

/*----------------------------------------------------------------------------*/
static uint32_t _width = CAM_DEFAULT_WIDTH;
static uint32_t _height = CAM_DEFAULT_HEIGHT;
static uint32_t _bpp = 1; /* bytes per pixel (RAW8 Bayer) */
static int _mode = OV5647_MODE_640x480;

/* DMA capture buffer */
static uint32_t _dma_phys = 0;   /* physical/bus address for UNICAM */
static void* _dma_virt = NULL;   /* CPU-accessible virtual address */
static uint32_t _dma_size = 0;
static int _cap_idx = 0;         /* double-buffer half the hardware owns */
static int _cap_fails = 0;       /* consecutive capture timeouts */

static bool _in_use = false;

/* UNICAM instance: default CAM1 (standard Pi4 camera connector, I2C 44/45);
 * switched to UNICAM0 if the sensor is found on the CAM0 pins (GPIO0/1) */
uint32_t _unicam_off = UNICAM1_OFFSET;
uint32_t _unicam_cmi_off = UNICAM1_CMI_OFFSET;
static uint32_t _cm_cam_ctl = 0x48; /* CM_CAM1CTL */
static uint32_t _cm_cam_div = 0x4C; /* CM_CAM1DIV */

/* frame chunked-read cursor: libc dev_read clamps each IPC to 64KB and char
 * devices always pass offset 0, so a full frame is delivered in chunks */
static uint32_t _read_pos = 0;

/* frame snapshot: filled by loop_step (normal context, where proc_usleep
 * really sleeps); cam_read only copies from it and never waits. This also
 * prevents tearing: the DMA buffer is free-running and would be overwritten
 * while a client is still reading a frame in 64KB chunks. */
static uint8_t* _snap_buf = NULL;
static bool _snap_ready = false;

/*----------------------------------------------------------------------------*/
/* VC mailbox memory allocation for DMA buffers */

static int mbox_call(uint32_t* buf) {
	mail_message_t msg;
	memset(&msg, 0, sizeof(msg));
	msg.data = (dma_phy_addr(0, (ewokos_addr_t)buf) + MBOX_VC_ALIAS_NC) >> 4;
	msg.channel = 8;
	bcm283x_mailbox_call(&msg);
	return (buf[1] & 0x80000000u) ? 0 : -1;
}

/* drive firmware expander virtual GPIO (128+) - powers the camera module */
static int cam_power_set(int on) {
	uint32_t* buf;
	int res;

	buf = (uint32_t*)dma_alloc(0, 8 * 4);
	if (buf == NULL) return -1;
	buf[0] = 8 * 4;
	buf[1] = 0;
	buf[2] = MBOX_TAG_SET_GPIO_STATE;
	buf[3] = 8;
	buf[4] = 8;
	buf[5] = VGPIO_CAM_GPIO; /* CAM_GPIO: camera enable line */
	buf[6] = on ? 1 : 0;
	buf[7] = 0;
	res = mbox_call(buf);
	dma_free(0, (ewokos_addr_t)buf);
	return res;
}

/* power on a VC-firmware-managed domain (UNICAM sits in one; while it is
 * off its registers read 0 and writes are dropped). Returns the state read
 * back via GET_DOMAIN_STATE (-1 on mailbox failure). */
static int pd_power_on(uint32_t domain) {
	uint32_t* buf;
	int res, state = -1;

	buf = (uint32_t*)dma_alloc(0, 8 * 4);
	if (buf == NULL) return -1;
	buf[0] = 8 * 4;
	buf[1] = 0;
	buf[2] = MBOX_TAG_SET_DOMAIN_STATE;
	buf[3] = 8;
	buf[4] = 8;
	buf[5] = domain;
	buf[6] = 1; /* on */
	buf[7] = 0;
	res = mbox_call(buf);
	dma_free(0, (ewokos_addr_t)buf);
	if (res != 0)
		return -1;

	/* read back: old firmware silently ignores unknown tags */
	buf = (uint32_t*)dma_alloc(0, 8 * 4);
	if (buf == NULL) return -1;
	buf[0] = 8 * 4;
	buf[1] = 0;
	buf[2] = MBOX_TAG_GET_DOMAIN_STATE;
	buf[3] = 8;
	buf[4] = 8;
	buf[5] = domain;
	buf[6] = 0;
	buf[7] = 0;
	if (mbox_call(buf) == 0)
		state = (int)buf[6];
	dma_free(0, (ewokos_addr_t)buf);
	return state;
}

static int cam_dma_alloc(uint32_t size) {
	ewokos_addr_t vaddr;

	/* Allocate from the kernel DMA pool: physically contiguous low RAM
	 * and mapped non-cached into our space. UNICAM's AXI master runs
	 * behind the legacy 0xC0000000 alias which only windows the first
	 * 0x3C000000 of RAM; firmware MEM_ALLOC hands out top-of-RAM
	 * buffers (>0x3C000000) that alias into peripheral space and wedge
	 * the output engine. */
	vaddr = dma_alloc(0, size);
	if (vaddr == 0)
		return -1;
	_dma_virt = (void*)vaddr;
	_dma_phys = dma_phy_addr(0, vaddr);
	if (_dma_phys == 0 || (_dma_phys + size) > 0x3C000000u) {
		printf("camd: dma buf out of UNICAM bus window (phys=%08x)\n",
				_dma_phys);
		dma_free(0, vaddr);
		_dma_virt = NULL;
		_dma_phys = 0;
		return -1;
	}
	_dma_size = size;
	return 0;
}

static void cam_dma_free(void) {
	if (_dma_virt != NULL)
		dma_free(0, (ewokos_addr_t)_dma_virt);
	_dma_phys = 0;
	_dma_virt = NULL;
	_dma_size = 0;
}

/*----------------------------------------------------------------------------*/
/* UNICAM hardware control */

/* NOTE: on BCM2711 the CSI-2 lanes are dedicated D-PHY pins, NOT GPIO alt
 * functions. Never touch GPIO 0-5 here: they carry the camera I2C bus. */

/* ---- CAM0/CAM1 low-power clock (clock manager) ----
 * UNICAM needs its CM_CAMn "lp" clock (~100MHz) to run its LP-state
 * receiver logic; firmware leaves it disabled. */
#define CM_BASE      (_mmio_base + 0x101000u)
#define CM_PASSWD    0x5A000000u
#define CM_SRC_OSC   1u
#define CM_SRC_PLLD  6u
#define CM_ENAB      (1u << 4)
#define CM_BUSY      (1u << 7)
#define CM_FRAC      (1u << 9)

static inline uint32_t cm_readl(uint32_t off) {
	return *(volatile uint32_t*)(CM_BASE + off);
}
static inline void cm_writel(uint32_t off, uint32_t val) {
	*(volatile uint32_t*)(CM_BASE + off) = val;
}

/* (re)start the CM_CAMn generator from the given source with a 12.12
 * divider; returns 0 once BUSY confirms the generator is running.
 * Some parents refuse to feed a generator on BCM2711 (seen with
 * PLLD_PER on DSI1E: BUSY never sets), so the caller must check. */
static int cm_cam_start(uint32_t src, uint32_t div) {
	uint32_t ctl;
	int i;

	/* stop the clock, wait for BUSY to clear */
	cm_writel(_cm_cam_ctl, CM_PASSWD | src);
	for (i = 0; i < 100; i++) {
		if ((cm_readl(_cm_cam_ctl) & CM_BUSY) == 0)
			break;
		proc_usleep(100);
	}

	cm_writel(_cm_cam_div, CM_PASSWD | (div & 0x00FFFFFFu));
	ctl = src | ((div & 0xFFFu) ? CM_FRAC : 0);
	cm_writel(_cm_cam_ctl, CM_PASSWD | ctl);
	cm_writel(_cm_cam_ctl, CM_PASSWD | ctl | CM_ENAB);

	for (i = 0; i < 50; i++) {
		if (cm_readl(_cm_cam_ctl) & CM_BUSY)
			return 0;
		proc_usleep(100);
	}
	return -1;
}

static void cam_clock_enable(void) {
	sys_info_t sysinfo;
	uint32_t plld, div;
	int i;

	/* PLLD_PER: 750MHz on BCM2711 (pi4/cm4), 500MHz on earlier chips */
	syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&sysinfo);
	if (strstr(sysinfo.machine, "pi4") || strstr(sysinfo.machine, "cm4"))
		plld = 750000000u;
	else
		plld = 500000000u;

	/* divider in 12.12 fixed point: target 100MHz. PLLD start is flaky
	 * on some boots (BUSY never sets); retry so the lp clock source -
	 * and with it the D-PHY LP timing - is the same on every boot. */
	div = (uint32_t)(((uint64_t)plld << 12) / 100000000u);
	for (i = 0; i < 3; i++) {
		if (cm_cam_start(CM_SRC_PLLD, div) == 0)
			break;
	}
	if (i >= 3) {
		/* PLLD_PER refused to feed this generator; fall back to the
		 * 54MHz crystal. The LP receiver logic only samples LP-state
		 * transitions, so a slower lp clock still works. */
		if (cm_cam_start(CM_SRC_OSC, 1u << 12) == 0)
			printf("camd: cam lp clock on XOSC fallback\n");
		else
			printf("camd: warning: cam lp clock stuck off\n");
	}
	printf("camd: cam clk ctl=%08x div=%08x\n",
			cm_readl(_cm_cam_ctl), cm_readl(_cm_cam_div));
}

/* ---- UNICAM receiver bring-up (upstream bcm2835-unicam unicam_start_rx) ----
 * 2 data lanes, CSI-2 D-PHY, RAW8, continuous clock; frames stream into the
 * single buffer 0 which is re-armed automatically (MISC FL0|FL1). */
static void unicam_start_rx(void) {
	uint32_t val;
	uint32_t bus_addr = _dma_phys | DMA_VC_ALIAS;
	uint32_t frame_size = _width * _height * _bpp;
	uint32_t line_int_freq = _height >> 2;

	if (line_int_freq < 128)
		line_int_freq = 128;

	/* enable lane clock gates: clock lane + 2 data lanes */
	unicam_clk_gate_write(0x15); /* 0b010101 */

	/* basic init: memory (AXI) output mode */
	unicam_writel(UNICAM_CTRL, UNICAM_MEM);

	/* analogue PHY: enable with reset held, then release */
	val = UNICAM_AR | (7u << UNICAM_CTATADJ_SHIFT) | (7u << UNICAM_PTATADJ_SHIFT);
	unicam_writel(UNICAM_ANA, val);
	proc_usleep(2000);
	unicam_writel(UNICAM_ANA, val & ~UNICAM_AR);

	/* peripheral reset pulse */
	unicam_writel(UNICAM_CTRL, unicam_readl(UNICAM_CTRL) | UNICAM_CPR);
	unicam_writel(UNICAM_CTRL, unicam_readl(UNICAM_CTRL) & ~UNICAM_CPR);
	unicam_writel(UNICAM_CTRL, unicam_readl(UNICAM_CTRL) & ~UNICAM_CPE);

	/* Rx control: CSI-2 mode, packet framer timeout, output engine timeout */
	val = unicam_readl(UNICAM_CTRL);
	val &= ~((1u << 3) | (1u << 5)); /* CPM=CSI2, DCM=0 */
	val |= (0xFu << UNICAM_PFT_SHIFT) | (128u << UNICAM_OET_SHIFT);
	unicam_writel(UNICAM_CTRL, val);

	unicam_writel(UNICAM_IHWIN, 0);
	unicam_writel(UNICAM_IVWIN, 0);

	/* AXI bus access QoS */
	val = unicam_readl(UNICAM_PRI);
	val &= ~0x3FFFFu;
	val |= UNICAM_PE | (2u << UNICAM_PT_SHIFT) | (8u << UNICAM_NP_SHIFT) |
	       (0xEu << UNICAM_PP_SHIFT);
	unicam_writel(UNICAM_PRI, val);

	/* enable data lanes (clear DDL) */
	unicam_writel(UNICAM_ANA, unicam_readl(UNICAM_ANA) & ~UNICAM_DDL);

	/* interrupt control: frame start/end + buffer wrap; we poll ISTA */
	val = UNICAM_FSIE | UNICAM_FEIE | UNICAM_IBOB |
	      (line_int_freq << UNICAM_LCIE_SHIFT);
	unicam_writel(UNICAM_ICTL, val);
	unicam_writel(UNICAM_STA, UNICAM_STA_MASK_ALL);
	unicam_writel(UNICAM_ISTA, UNICAM_ISTA_MASK_ALL);

	/* D-PHY timing: tclk_term_en=2, tclk_settle=6 */
	unicam_writel(UNICAM_CLT, (2u << UNICAM_CLT1_SHIFT) | (6u << UNICAM_CLT2_SHIFT));
	/* td_term_en=2, ths_settle=6, trx_enable=0 */
	unicam_writel(UNICAM_DLT, (2u << UNICAM_DLT1_SHIFT) | (6u << UNICAM_DLT2_SHIFT));

	unicam_writel(UNICAM_CTRL, unicam_readl(UNICAM_CTRL) & ~UNICAM_SOE);

	/* packet compare 0: match frame-end short packets (avoids missed FE) */
	val = UNICAM_PCE | UNICAM_GI | UNICAM_CPH |
	      (0u << UNICAM_PCVC_SHIFT) | (CSI2_DT_FE << UNICAM_PCDT_SHIFT);
	unicam_writel(UNICAM_CMP0, val);

	/* clock lane: enable + LP + termination + HS (continuous clock) */
	unicam_writel(UNICAM_CLK, UNICAM_CLE | UNICAM_CLLPE | UNICAM_CLTRE | UNICAM_CLHSE);

	/* data lanes 0/1: enable + LP + termination + HS; 2/3 unused */
	val = UNICAM_DLE | UNICAM_DLLPE | UNICAM_DLTRE | UNICAM_DLHSE;
	unicam_writel(UNICAM_DAT0, val);
	unicam_writel(UNICAM_DAT1, val);
	unicam_writel(UNICAM_DAT2, 0);
	unicam_writel(UNICAM_DAT3, 0);

	/* image buffer 0: stride, start/end address (double-buffer half 0;
	 * unicam_capture_frame flips IBSA0 between the two halves) */
	unicam_writel(UNICAM_IBLS, _width * _bpp);
	unicam_writel(UNICAM_IBSA0, bus_addr);
	unicam_writel(UNICAM_IBEA0, bus_addr + frame_size);
	_cap_idx = 0;

	/* RAW8: no unpack/repack */
	unicam_writel(UNICAM_IPIPE,
			(UNICAM_PUM_NONE << UNICAM_PUM_SHIFT) |
			(UNICAM_PPM_NONE << UNICAM_PPM_SHIFT));

	/* image identifier: virtual channel 0, data type RAW8 */
	unicam_writel(UNICAM_IDI0, (0u << 6) | CSI2_DT_RAW8);

	/* auto re-arm frame buffers */
	unicam_writel(UNICAM_MISC, unicam_readl(UNICAM_MISC) | UNICAM_FL0 | UNICAM_FL1);

	/* enable peripheral, load image pointers */
	unicam_writel(UNICAM_CTRL, unicam_readl(UNICAM_CTRL) | UNICAM_CPE);
	unicam_writel(UNICAM_ICTL, unicam_readl(UNICAM_ICTL) | (1u << UNICAM_LIP_SHIFT));
}

static void unicam_stop_rx(void) {
	/* disable data lanes analogue, stop output engine */
	unicam_writel(UNICAM_ANA, unicam_readl(UNICAM_ANA) | UNICAM_DDL);
	unicam_writel(UNICAM_CTRL, unicam_readl(UNICAM_CTRL) | UNICAM_SOE);
	unicam_writel(UNICAM_DAT0, 0);
	unicam_writel(UNICAM_DAT1, 0);
	/* peripheral reset pulse, then disable */
	unicam_writel(UNICAM_CTRL, unicam_readl(UNICAM_CTRL) | UNICAM_CPR);
	proc_usleep(100);
	unicam_writel(UNICAM_CTRL, unicam_readl(UNICAM_CTRL) & ~UNICAM_CPR);
	unicam_writel(UNICAM_CTRL, unicam_readl(UNICAM_CTRL) & ~UNICAM_CPE);
	unicam_writel(UNICAM_DCS, 0);
	unicam_clk_gate_write(0);
}

/* wait for the next complete frame.
 * UNICAM is free-running and FL0 latches IBSA0 at every frame start, so:
 * wait for frame start (hardware begins filling _cap_idx), re-point IBSA0
 * at the other half for the following frame, wait for frame end, then wait
 * for the NEXT frame start: once hardware has begun the following frame in
 * the other half, the completed half is idle and fully drained.
 * (Never poll IBWP for drain: at 60fps the inter-frame blanking is shorter
 * than one scheduler tick, so IBWP is re-armed to the other half before we
 * can observe it reaching the window end.)
 * Entry invariant: IBSA0/IBEA0 point at half[_cap_idx]; every failure path
 * restores it so buffer tracking never desyncs from hardware.
 * Returns the completed buffer index (0/1) or -1 on timeout/bad frame. */
static int unicam_capture_frame(void) {
	uint32_t frame_size = _width * _height * _bpp;
	uint32_t bus_addr, base, wrote, sta;
	uint32_t timeout;
	int done, next;

	if (_dma_phys == 0 || _dma_virt == NULL)
		return -1;

	/* clear frame status, wait for the next frame start.
	 * NOTE: proc_usleep granularity is a scheduler tick (~1ms), so poll
	 * once per ms: CAM_FRAME_TIMEOUT_MS iterations total */
	unicam_writel(UNICAM_ISTA, UNICAM_ISTA_MASK_ALL);
	unicam_writel(UNICAM_STA, UNICAM_STA_MASK_ALL);

	timeout = CAM_FRAME_TIMEOUT_MS;
	while (timeout > 0) {
		if (unicam_readl(UNICAM_ISTA) & UNICAM_ISTA_FSI)
			break;
		proc_usleep(1000);
		timeout--;
	}
	if (timeout == 0)
		return -1;

	/* next frame goes to the other half (latched at its frame start) */
	next = _cap_idx ^ 1;
	bus_addr = (_dma_phys + (uint32_t)next * frame_size) | DMA_VC_ALIAS;
	unicam_writel(UNICAM_IBSA0, bus_addr);
	unicam_writel(UNICAM_IBEA0, bus_addr + frame_size);

	/* CRITICAL: the previous frame may have ended while we waited for
	 * this frame's start, leaving FEI/PI0 already set. Clear them now or
	 * the wait below returns instantly and the snapshot copy races the
	 * ongoing DMA write into the same buffer (torn, mixed frames). */
	unicam_writel(UNICAM_ISTA, UNICAM_ISTA_MASK_ALL);
	unicam_writel(UNICAM_STA, UNICAM_STA_MASK_ALL);

	/* wait for this frame's end. The FEI interrupt can be missed by
	 * hardware, so (like the upstream ISR) also accept the CMP0
	 * frame-end packet match (STA PI0). */
	timeout = CAM_FRAME_TIMEOUT_MS;
	while (timeout > 0) {
		if ((unicam_readl(UNICAM_ISTA) & UNICAM_ISTA_FEI) ||
		    (unicam_readl(UNICAM_STA) & UNICAM_STA_PI0))
			break;
		proc_usleep(1000);
		timeout--;
	}
	if (timeout == 0)
		goto fail_rearm;

	/* short-frame gate (hardware ground truth): until the next frame
	 * start re-arms it, IBWP still points inside this half, so sampled
	 * right at FE it gives the bytes actually written for this frame.
	 * A marginal link/sensor can deliver frames SHORTER than the window
	 * with no STA error bits; UNICAM then packs them back-to-back into
	 * the window (N stacked, vertically squashed copies) - such frames
	 * must never reach the client. DMA can lag FE by a few lines, so
	 * allow generous slack: a real short frame is hundreds of lines
	 * short and unambiguous. If the following frame start already
	 * re-armed IBWP the subtraction wraps huge and the gate passes -
	 * correct, since the window was closed at that frame start. */
	base = (_dma_phys + (uint32_t)_cap_idx * frame_size) | DMA_VC_ALIAS;
	wrote = unicam_readl(UNICAM_IBWP) - base;
	if (wrote + 64u * _width * _bpp < frame_size) {
		printf("camd: short frame dropped (wrote=%u)\n", wrote);
		goto fail_rearm;
	}

	/* integrity gate: never serve a frame received with rx errors */
	sta = unicam_readl(UNICAM_STA);
	if (sta & (UNICAM_STA_CRCE | UNICAM_STA_SBE | UNICAM_STA_PBE |
			UNICAM_STA_HOE | UNICAM_STA_PLE | UNICAM_STA_IFO |
			UNICAM_STA_OFO | UNICAM_STA_BFO)) {
		printf("camd: frame dropped, rx errors sta=%08x\n", sta);
		goto fail_rearm;
	}

	/* wait for the next frame start: hardware latches IBSA0 (other
	 * half) and the completed half becomes idle and fully drained */
	timeout = 100;
	while (timeout > 0) {
		if (unicam_readl(UNICAM_ISTA) & UNICAM_ISTA_FSI)
			break;
		proc_usleep(1000);
		timeout--;
	}
	if (timeout == 0)
		goto fail_rearm;
	proc_usleep(1000); /* margin for the last lines still in flight */

	done = _cap_idx;
	_cap_idx = next;
	return done;

fail_rearm:
	/* restore the entry invariant: hardware keeps (re)filling the half
	 * we still consider its own, never the one a client may hold */
	bus_addr = (_dma_phys + (uint32_t)_cap_idx * frame_size) | DMA_VC_ALIAS;
	unicam_writel(UNICAM_IBSA0, bus_addr);
	unicam_writel(UNICAM_IBEA0, bus_addr + frame_size);
	return -1;
}

/* bring up one UNICAM instance and check for a real frame.
 * D-PHY receivers can only sync while the clock lane is in LP-11, so the
 * sensor is stopped before enabling rx and restarted afterwards.
 * The LP-11 -> HS handshake can marginally fail (clock lane syncs but the
 * data lanes never deliver packets, sta=CS only), especially after a warm
 * reboot with the sensor left mid-stream, so retry the whole cycle and as
 * a last resort reprogram the sensor mode. */
static int unicam_probe_instance(uint32_t pd) {
	int pds, try;

	pds = pd_power_on(pd);
	cam_clock_enable();

	for (try = 0; try < 3; try++) {
		if (try > 0) {
			/* the sensor may stream with a stale/partial config
			 * (marginal bit-bang I2C); reprogram the full mode
			 * table before every retry */
			printf("camd: reprogramming sensor mode\n");
			ov5647_set_mode(_mode);
		}
		ov5647_stream_off();  /* clock lane -> LP-11 */
		proc_usleep(10000);
		unicam_start_rx();
		ov5647_stream_on();

		if (unicam_capture_frame() >= 0)
			return 0;

		printf("camd: unicam @%08x: no frame try=%d (pd=%d ctrl=%08x sta=%08x)\n",
				_unicam_off, try, pds,
				unicam_readl(UNICAM_CTRL), unicam_readl(UNICAM_STA));
		ov5647_stream_off();
		unicam_stop_rx();
		proc_usleep(10000);
	}
	return -1;
}

/* probe both UNICAM instances (the camera I2C pins do not identify the
 * CSI connector: BSC0 muxes to several pin sets) and keep whichever
 * actually delivers a frame. Used at boot and for runtime recovery. */
static int unicam_probe_all(void) {
	static const struct {
		uint32_t off, cmi, ctl, div, pd;
	} inst[2] = {
		{ UNICAM1_OFFSET, UNICAM1_CMI_OFFSET, 0x48, 0x4C, PD_UNICAM1 },
		{ UNICAM0_OFFSET, UNICAM0_CMI_OFFSET, 0x40, 0x44, PD_UNICAM0 },
	};
	int i;

	for (i = 0; i < 2; i++) {
		_unicam_off = inst[i].off;
		_unicam_cmi_off = inst[i].cmi;
		_cm_cam_ctl = inst[i].ctl;
		_cm_cam_div = inst[i].div;
		printf("camd: probing unicam @%08x\n", _unicam_off);
		if (unicam_probe_instance(inst[i].pd) == 0) {
			printf("camd: unicam @%08x active\n", _unicam_off);
			return 0;
		}
	}
	printf("camd: warning: no unicam instance delivered a frame\n");
	return -1;
}

/* camera I2C pins, kept for runtime sensor re-init (set in main) */
static int32_t _i2c_sda = 0;
static int32_t _i2c_scl = 1;

/* big hammer: some boots come up with the D-PHY completely dead (not even
 * clock-lane sync, sta=0) and no rx re-init or sensor reprogramming heals
 * it - only a real power cycle of the sensor module does. */
static void cam_hard_reset(void) {
	printf("camd: hard reset: power-cycling camera module\n");
	ov5647_stream_off();
	cam_power_set(0);
	proc_usleep(200000);
	cam_power_set(1);
	proc_usleep(300000); /* sensor power-up settle */
	if (ov5647_init(_i2c_sda, _i2c_scl) != 0) {
		printf("camd: hard reset: sensor re-init failed\n");
		return;
	}
	ov5647_set_mode(_mode); /* leaves sensor in stream-off (LP-11) */
}

/* full bring-up with escalation: rx probe first; if a full probe (3 tries
 * on both instances) delivered nothing, the link is dead - power-cycle the
 * camera and probe once more. Used at boot and for runtime recovery. */
static int cam_bringup(void) {
	if (unicam_probe_all() == 0)
		return 0;
	cam_hard_reset();
	return unicam_probe_all();
}

/*----------------------------------------------------------------------------*/
/* vdevice callbacks */

static int cam_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
		void* buf, int size, int offset, void* p) {
	(void)dev; (void)fd; (void)from_pid; (void)node; (void)offset; (void)p;

	/* libc dev_read clamps each IPC to 64KB and char devices always pass
	 * offset 0, so a full frame is delivered in chunks via _read_pos.
	 * NEVER wait here: this runs in IPC context where proc_usleep degrades
	 * to yield; frames are captured by cam_loop_step into _snap_buf. */
	uint32_t frame_size = _width * _height * _bpp;
	uint32_t remain, n;

	if (size <= 0)
		return 0;

	if (_read_pos == 0 && !_snap_ready) {
		/* no frame yet; return 0 (not VFS_ERR_RETRY: we publish no RD
		 * events, so a blocking client would sleep forever in vfs_block) */
		return 0;
	}

	remain = frame_size - _read_pos;
	n = (uint32_t)size < remain ? (uint32_t)size : remain;
	memcpy(buf, _snap_buf + _read_pos, n);
	_read_pos += n;
	if (_read_pos >= frame_size) {
		_read_pos = 0;
		_snap_ready = false; /* consumed: wait for a fresh snapshot */
	}
	return (int)n;
}

static int cam_open(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
		int oflag, void* p) {
	(void)dev; (void)fd; (void)from_pid; (void)node; (void)oflag; (void)p;
	if (_in_use) {
		return VFS_ERR_RETRY;
	}
	_in_use = true;
	_read_pos = 0;       /* fresh client: restart frame chunking */
	_snap_ready = false; /* and force a fresh, un-consumed snapshot */
	/* NOTE: sensor streaming is started once in main(); I2C bit-bang
	 * timing relies on proc_usleep which degrades inside IPC handlers */
	return 0;
}

static int cam_close(vdevice_t* dev, int fd, int from_pid, uint32_t node,
		fsinfo_t* fsinfo, void* p) {
	(void)dev; (void)fd; (void)from_pid; (void)node; (void)fsinfo; (void)p;
	_in_use = false;
	return 0;
}

static int cam_fcntl(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info,
		int cmd, proto_t* in, proto_t* out, void* p) {
	(void)dev; (void)fd; (void)from_pid; (void)info; (void)p; (void)in;

	if (cmd == 1) {
		/* get info: out = (width, height, bpp) */
		if (out != NULL) {
			PF->addi(out, (int)_width);
			PF->addi(out, (int)_height);
			PF->addi(out, (int)_bpp);
		}
		return 0;
	}
	if (cmd == 2) {
		/* resync: a chunked-read return was lost on the client side, so
		 * both ends restart the frame from position 0. Also invalidate
		 * the snapshot: its content was partially consumed. */
		_read_pos = 0;
		_snap_ready = false;
		return 0;
	}
	return -1;
}

/* frame pump: runs in device_run's main loop (normal context, proc_usleep
 * really sleeps). Captures into the DMA buffer, then snapshots it so
 * cam_read (IPC context) never has to wait on hardware. */
static int cam_loop_step(vdevice_t* dev, void* p) {
	(void)dev; (void)p;

	if (_snap_buf == NULL || _snap_ready) {
		/* idle until the last snapshot is consumed by the client */
		proc_usleep(10000);
		return 0;
	}

	{
		uint32_t frame_size = _width * _height * _bpp;
		int idx = unicam_capture_frame();
		if (idx >= 0) {
			memcpy(_snap_buf, (uint8_t*)_dma_virt + (uint32_t)idx * frame_size,
					frame_size);
			_snap_ready = true;
			_cap_fails = 0;
		} else if (++_cap_fails >= 3) {
			/* stream wedged (or boot probe never succeeded): rerun
			 * the full bring-up, escalating to a camera power-cycle
			 * if the rx probe alone cannot recover the link. Safe
			 * here: loop_step is normal context. */
			printf("camd: stream stalled, re-probing\n");
			cam_bringup();
			_cap_fails = 0;
		}
	}
	proc_usleep(10000);
	return 0;
}

/*----------------------------------------------------------------------------*/
int main(int argc, char** argv) {
	const char* mnt_point = "/dev/cam0";
	int32_t i2c_sda = 0; /* CM4 IO board CAM0: GPIO0=SDA, GPIO1=SCL */
	int32_t i2c_scl = 1;
	uint32_t frame_size;
	vdevice_t dev;

	/* parse args: camd [mnt_point] [i2c_sda] [i2c_scl] */
	if (argc > 1) mnt_point = argv[1];
	if (argc > 2) i2c_sda = atoi(argv[2]);
	if (argc > 3) i2c_scl = atoi(argv[3]);

	printf("camd: start mnt=%s i2c_sda=%d i2c_scl=%d\n", mnt_point, i2c_sda, i2c_scl);

	_mmio_base = mmio_map();

	/* init mailbox (needed for camera power & DMA alloc) */
	bcm283x_mailbox_init();

	/* raise CAM_GPIO (firmware expander) to power the camera module */
	if (cam_power_set(1) != 0)
		printf("camd: warning: cam power-on mailbox call failed\n");
	proc_usleep(300000); /* sensor power-up settle */

	/* init sensor via I2C; auto-probe pin pairs if default fails */
	if (ov5647_init(i2c_sda, i2c_scl) != 0) {
		int32_t pairs[][2] = {{0, 1}, {44, 45}, {28, 29}};
		int found = 0;
		for (uint32_t i = 0; i < sizeof(pairs)/sizeof(pairs[0]); i++) {
			if (pairs[i][0] == i2c_sda && pairs[i][1] == i2c_scl)
				continue;
			printf("camd: probing i2c sda=%d scl=%d\n", pairs[i][0], pairs[i][1]);
			if (ov5647_init(pairs[i][0], pairs[i][1]) == 0) {
				i2c_sda = pairs[i][0];
				i2c_scl = pairs[i][1];
				found = 1;
				break;
			}
		}
		if (!found) {
			printf("camd: ov5647 init failed\n");
			return -1;
		}
	}
	printf("camd: ov5647 detected (sda=%d scl=%d)\n", i2c_sda, i2c_scl);
	_i2c_sda = i2c_sda; /* keep for runtime hard-reset re-init */
	_i2c_scl = i2c_scl;

	/* configure sensor: RAW8 640x480, leaves sensor in stream-off (LP-11) */
	printf("camd: configure default mode\n");
	if (ov5647_set_mode(_mode) != 0) {
		printf("camd: ov5647 set_mode failed\n");
		return -1;
	}

	/* allocate DMA capture buffer: two halves for tear-free double
	 * buffering (hardware fills one while the other is snapshotted) */
	frame_size = _width * _height * _bpp;
	if (cam_dma_alloc(frame_size * 2) != 0) {
		printf("camd: dma alloc failed (%u bytes)\n", frame_size * 2);
		return -1;
	}
	printf("camd: dma buf phys=%08x size=%u\n", _dma_phys, _dma_size);

	/* snapshot buffer served to clients by cam_read */
	_snap_buf = (uint8_t*)malloc(frame_size);
	if (_snap_buf == NULL) {
		printf("camd: snap buf alloc failed\n");
		return -1;
	}

	/* bring up UNICAM (loop_step re-probes at runtime if this fails) */
	printf("camd: setup unicam\n");
	cam_bringup();

	/* prime the snapshot with the first frame */
	{
		int idx = unicam_capture_frame();
		if (idx >= 0) {
			memcpy(_snap_buf, (uint8_t*)_dma_virt + (uint32_t)idx * frame_size,
					frame_size);
			_snap_ready = true;
			printf("camd: first frame captured\n");
		}
	}

	/* register vdevice */
	memset(&dev, 0, sizeof(dev));
	strcpy(dev.name, "cam");
	dev.read = cam_read;
	dev.open = cam_open;
	dev.close = cam_close;
	dev.fcntl = cam_fcntl;
	dev.loop_step = cam_loop_step;

	printf("camd: running %ux%u bpp=%u\n", _width, _height, _bpp);
	return device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666);
}
