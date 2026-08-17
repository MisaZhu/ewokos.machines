#include "dsi1_internal.h"

#include <string.h>
#include <unistd.h>

#include <ewoksys/dma.h>
#include <ewoksys/mmio.h>
#include <ewoksys/sys.h>
#include <sysinfo.h>

#include <arch/bcm283x/mailbox.h>

/*
 * Generation detection + shared block accessors + STC busy-wait +
 * VC firmware power-domain helpers for the DSI1 pipeline.
 *
 * The generation is detected from sysinfo.mmio.phy_base exactly the
 * way the rest of this library does (i2s.c): 0xfe000000 is the
 * BCM2711 (gen5) peripheral window, 0x3f000000 the BCM2835/2837 one.
 */

int bcm283x_dsi1_is_gen5(void) {
	sys_info_t sysinfo;

	sys_get_sys_info(&sysinfo);
	return (sysinfo.mmio.phy_base == 0xfe000000u) ? 1 : 0;
}

uint32_t dsi1_xosc_hz(void) {
	return bcm283x_dsi1_is_gen5() ?
			DSI1_XOSC_GEN5_HZ : DSI1_XOSC_GEN4_HZ;
}

/* ---------- raw block accessors ---------- */

/* Selected DSI port; the display connector wiring is board dependent
 * (Pi3: DSI0, Pi4/CM4: DSI1), so the daemon probes and sets this. */
static int _dsi_port = 1;

int dsi1_port(void) {
	return _dsi_port;
}

void dsi1_set_port(int port) {
	_dsi_port = (port == 0) ? 0 : 1;
}

int bcm283x_dsi1_port(void) {
	return dsi1_port();
}

void bcm283x_dsi1_set_port(int port) {
	dsi1_set_port(port);
}

static volatile uint32_t* _block(uint32_t offset) {
	if (_mmio_base == 0) {
		return 0;
	}
	return (volatile uint32_t*)(uintptr_t)(_mmio_base + offset);
}

uint32_t dsi1_cprman_read(uint32_t off) {
	volatile uint32_t* base = _block(DSI1_CPRMAN_OFFSET);
	return base ? base[off / 4] : 0;
}

void dsi1_cprman_write(uint32_t off, uint32_t val) {
	volatile uint32_t* base = _block(DSI1_CPRMAN_OFFSET);
	if (base) {
		base[off / 4] = DSI1_CM_PASSWORD | (val & 0x00ffffffU);
	}
}

uint32_t dsi1_dsi_read(uint32_t off) {
	volatile uint32_t* base =
			_block(_dsi_port ? DSI1_DSI_OFFSET : DSI0_DSI_OFFSET);
	return base ? base[off / 4] : 0;
}

/* Raw per-port readers (no port routing) for the firmware snapshot. */
uint32_t dsi1_dsi_read_port(int port, uint32_t off) {
	volatile uint32_t* base =
			_block(port ? DSI1_DSI_OFFSET : DSI0_DSI_OFFSET);
	return base ? base[off / 4] : 0;
}

uint32_t dsi1_pv_read_port(int port, uint32_t off) {
	volatile uint32_t* base =
			_block(port ? DSI1_PV_OFFSET : DSI0_PV_OFFSET);
	return base ? base[off / 4] : 0;
}

void dsi1_dsi_write(uint32_t off, uint32_t val) {
	volatile uint32_t* base =
			_block(_dsi_port ? DSI1_DSI_OFFSET : DSI0_DSI_OFFSET);
	if (base) {
		base[off / 4] = val;
	}
}

uint32_t dsi1_hvs_read(uint32_t off) {
	volatile uint32_t* base = _block(DSI1_HVS_OFFSET);
	return base ? base[off / 4] : 0;
}

void dsi1_hvs_write(uint32_t off, uint32_t val) {
	volatile uint32_t* base = _block(DSI1_HVS_OFFSET);
	if (base) {
		base[off / 4] = val;
	}
}

uint32_t dsi1_pv_read(uint32_t off) {
	volatile uint32_t* base =
			_block(_dsi_port ? DSI1_PV_OFFSET : DSI0_PV_OFFSET);
	return base ? base[off / 4] : 0;
}

void dsi1_pv_write(uint32_t off, uint32_t val) {
	volatile uint32_t* base =
			_block(_dsi_port ? DSI1_PV_OFFSET : DSI0_PV_OFFSET);
	if (base) {
		base[off / 4] = val;
	}
}

/*
 * Port liveness probe.  The ID register reads fine even on a powered
 * down port (observed on Pi3 DSI1), so it cannot discriminate.  A
 * write-readback can: with the firmware power domain off the register
 * bus silently drops writes (also observed on Pi3 DSI1).
 *
 * The probe register is INT_EN (0x28 DSI0 / 0x34 DSI1): a plain latch
 * with no side effect on the pipeline.  CTRL bit 10 (SOFT_RESET_CFG)
 * looked tempting but does NOT read back as written on a live port
 * (both ports failed that test on a Pi 3A+ where DSI0 is demonstrably
 * running), so a RW latch is the only reliable witness.  INT_EN
 * defaults to 0, so restoring it to 0 leaves the port exactly as
 * found even when the readback is trusted.
 */
int bcm283x_dsi1_probe_port(int port) {
	volatile uint32_t* base;
	uint32_t old;
	uint32_t off;
	uint32_t test;

	if (_mmio_base == 0) {
		return -1;
	}
	base = _block(port ? DSI1_DSI_OFFSET : DSI0_DSI_OFFSET);
	if (base == 0) {
		return -1;
	}
	off = port ? 0x34U : 0x28U;   /* INT_EN */

	old = base[off / 4];
	test = old ^ 0x00000001U;     /* toggle bit 0 */
	base[off / 4] = test;
	if (base[off / 4] != test) {
		return -1;            /* write dropped: port powered off */
	}
	base[off / 4] = old;
	return 0;
}

/* ---------- STC CLO busy-wait ---------- */

/*
 * The System Timer's CLO (MMIO+0x3004) is the low 32 bits of the
 * 1 MHz microsecond counter; identical on gen4 and gen5.
 */
uint32_t dsi1_micros(void) {
	volatile uint32_t* clo = _block(DSI1_STC_CLO_OFFSET);
	return clo ? *clo : 0;
}

uint32_t bcm283x_dsi1_millis(void) {
	return dsi1_micros() / 1000U;
}

void bcm283x_dsi1_udelay(uint32_t us) {
	volatile uint32_t* clo = _block(DSI1_STC_CLO_OFFSET);

	if (clo == 0) {
		/*
		 * Fallback before MMIO is mapped: coarse cycle-counting
		 * loop.  Only rough, but nothing here needs it early.
		 */
		while (us > 0) {
			volatile uint32_t n = 300;
			while (n) {
				n--;
			}
			us--;
		}
		return;
	}
	uint32_t start = *clo;
	while ((uint32_t)(*clo - start) < us) {
		/* spin */
	}
}

void bcm283x_dsi1_mdelay(uint32_t ms) {
	if (ms == 0) {
		return;
	}
	usleep(ms * 1000U);
}

/* ---------- VC firmware power domains ---------- */

/*
 * VC property mailbox tags (raspberrypi-firmware.h).  SET_DOMAIN_STATE
 * is the "new" power interface that covers DSI1; the old SET_POWER_STATE
 * interface only knows USB/V3D-style device ids.
 */
#define TAG_GET_DOMAIN_STATE  0x00030030U
#define TAG_SET_DOMAIN_STATE  0x00038030U

#define MAILBOX_VC_ALIAS_NONCACHED  0x40000000U
#define MAILBOX_VC_ALIAS_COHERENT   0xC0000000U
#define MAILBOX_RESPONSE_SUCCESS    0x80000000U

/*
 * Instrumented property-channel transaction.  The VC property channel
 * has no cross-process arbitration in this OS (cpud polls it from a
 * separate process), so a concurrent client can pop and drop our
 * response while we wait.  Count what happens so one boot photo can
 * tell contention (foreign>0) apart from a silent firmware (timeout>0,
 * foreign=0) or a rejected buffer (ok<calls, timeout=0).
 */
/*
 * A firmware property reply lands in single-digit milliseconds;
 * ~100ms of MMIO polling is a generous budget.  The old 33M-loop
 * figure turned every silent call into a multi-second spin, and a
 * dom-probe burst of them read as a full boot hang.
 */
#define MBOX_WAIT_LOOPS  0x40000u

static uint32_t _mb_calls, _mb_ok, _mb_to, _mb_foreign;
static uint32_t _mb_lastphy, _mb_lastb1;

static int _mbox_call(uint32_t* buf, uint32_t alias) {
	uint32_t phy = dma_phy_addr(0, (ewokos_addr_t)(uintptr_t)buf);
	mail_message_t msg;
	uint32_t loop;
	int done = 0;

	memset(&msg, 0, sizeof(msg));
	msg.data = (phy + alias) >> 4;
	msg.channel = PROPERTY_CHANNEL;

	_mb_calls++;
	_mb_lastphy = phy;

	loop = MBOX_WAIT_LOOPS;
	while (MAIL0_STATUS->full) {
		if (--loop == 0) {
			_mb_to++;
			_mb_lastb1 = buf[1];
			return -1;
		}
	}
	bcm283x_mailbox_send(&msg);

	loop = MBOX_WAIT_LOOPS;
	while (loop > 0) {
		uint32_t raw;
		loop--;
		if (MAIL0_STATUS->empty)
			continue;
		/*
		 * Raw FIFO pop.  Do NOT use bcm283x_mailbox_read()
		 * here: it carries its own 33M-loop wait plus a
		 * do-while channel loop, which nested inside this
		 * loop turns a miss into an effective hang.
		 */
		raw = *((volatile uint32_t*)(MAILBOX_BASE + 0x00));
		if (raw != ((msg.data << 4) | (msg.channel & 0xfu))) {
			_mb_foreign++;
			continue;
		}
		done = 1;
		break;
	}

	_mb_lastb1 = buf[1];
	if (!done) {
		_mb_to++;
		return -1;
	}
	if ((buf[1] & MAILBOX_RESPONSE_SUCCESS) == 0)
		return -1;
	_mb_ok++;
	return 0;
}

void bcm283x_dsi1_mbox_stats(void) {
	printf("dsi: mbox calls=%u ok=%u timeout=%u foreign=%u phy=%08x b1=%08x\n",
			(unsigned)_mb_calls, (unsigned)_mb_ok, (unsigned)_mb_to,
			(unsigned)_mb_foreign, (unsigned)_mb_lastphy,
			(unsigned)_mb_lastb1);
}

/*
 * One-tag property transaction with the same VC bus-alias fallback the
 * rest of this library uses (framebuffer.c): try the non-cached alias
 * first, retry with the coherent alias if the firmware stays silent.
 * in_len is the request payload length (GET: 4 = id in, state out;
 * SET: 8 = id + state in); a request code that consumes the whole
 * value buffer leaves no room for the reply and gets the buffer
 * rejected by the firmware.
 */
static int _property_call(uint32_t tag, uint32_t in_len,
		uint32_t* val0, uint32_t* val1) {
	uint32_t size = 8 * 4;
	uint32_t* buf = (uint32_t*)(uintptr_t)dma_alloc(0, size);
	int ret = -1;
	int attempt;

	if (buf == NULL) {
		return -1;
	}

	for (attempt = 0; attempt < 2 && ret != 0; attempt++) {
		uint32_t alias = (attempt == 0) ?
				MAILBOX_VC_ALIAS_NONCACHED : MAILBOX_VC_ALIAS_COHERENT;
		buf[0] = size;
		buf[1] = 0;              /* process request */
		buf[2] = tag;
		buf[3] = 8;              /* value buffer size */
		buf[4] = in_len;         /* request: input length */
		buf[5] = *val0;
		buf[6] = *val1;
		buf[7] = 0;              /* end tag */
		ret = _mbox_call(buf, alias);
	}

	if (ret == 0) {
		*val0 = buf[5];
		*val1 = buf[6];
	}
	dma_free(0, (ewokos_addr_t)(uintptr_t)buf);
	return ret;
}

int bcm283x_dsi1_power_domain_get(uint32_t domain) {
	uint32_t d = domain;
	uint32_t state = 0xffffffffU;

	if (_property_call(TAG_GET_DOMAIN_STATE, 4, &d, &state) != 0) {
		return -1;
	}
	return (int)(state & 1U);
}

int bcm283x_dsi1_power_domain_set(uint32_t domain, int on) {
	uint32_t d;
	uint32_t state;

	/* Request the state change. */
	d = domain;
	state = on ? 1U : 0U;
	if (_property_call(TAG_SET_DOMAIN_STATE, 8, &d, &state) != 0) {
		return -1;
	}

	/*
	 * Read the state back: old firmware silently skips unknown tags,
	 * so a successful mailbox roundtrip alone doesn't prove the
	 * domain moved.
	 */
	d = domain;
	state = 0xffffffffU;
	if (_property_call(TAG_GET_DOMAIN_STATE, 4, &d, &state) != 0) {
		return -1;
	}
	if (on && (state & 1U) == 0U) {
		return -1;
	}
	return 0;
}
