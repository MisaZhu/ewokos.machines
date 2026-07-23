#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <ewoksys/dma.h>
#include <ewoksys/mmio.h>
#include <ewoksys/vfs.h>

#include "dma_chain.h"

/*
 * ARM-side DMA controller registers.
 * NOTE: _sysinfo.sys_dma.v_base is the DMA *memory* region, NOT the
 * controller. The BCM283x DMA channel registers live in MMIO space.
 * Channel 5 matches the proven raspix/system/soundd configuration.
 */
#define DMA_CHANNEL      5U
#define DMA_CTRL_BASE    (_mmio_base + 0x007000U + (DMA_CHANNEL * 0x100U))
#define DMA_ENABLE_REG   (_mmio_base + 0x007FF0U)
#define DMA_ENABLE_BIT   (1U << DMA_CHANNEL)
#define DMA_CS_ACTIVE    0x1U
#define DMA_CS_END       (1U << 1)
#define DMA_CS_RESET     (1U << 31)
#define DMA_CS_PRIORITY  (0x8U << 16)
#define DMA_CS_PANIC     (0x8U << 20)

/* dma_buf_t.cb.flag states */
#define DMA_BUF_EMPTY    0U
#define DMA_BUF_FILLING  1U
#define DMA_BUF_QUEUED   2U

/* TI: SRC_INC | DEST_DREQ | PERMAP=2 (PCM TX) - same as raspix soundd. */
#define DMA_CB_TI        ((1U << 8) | (1U << 6) | (2U << 16))

static dma_buf_t* dma_buf[DMA_BUF_CNT];
static dma_buf_t* _chain; /* head of queued chain (hardware visible) */
static dma_buf_t* _tail;  /* tail of queued chain */
static dma_buf_t* _fill;  /* buffer currently being filled (not queued yet) */
static dma_buf_t* _started_tail; /* tail of the chain the hardware was launched with */

static inline void dma_reg_write(uint32_t off, uint32_t v) {
	*(volatile uint32_t*)(uintptr_t)(DMA_CTRL_BASE + off) = v;
}

static inline uint32_t dma_reg_read(uint32_t off) {
	return *(volatile uint32_t*)(uintptr_t)(DMA_CTRL_BASE + off);
}

static dma_buf_t* dma_buf_alloc(void) {
	for (int i = 0; i < DMA_BUF_CNT; i++) {
		if (dma_buf[i]->cb.flag == DMA_BUF_EMPTY) {
			dma_buf[i]->cb.flag = DMA_BUF_FILLING;
			dma_buf[i]->cb.next = NULL;
			dma_buf[i]->cb.txfr_len = 0;
			dma_buf[i]->cb.nextconbk = 0;
			return dma_buf[i];
		}
	}
	return NULL;
}

static void dma_buf_free(dma_buf_t* buf) {
	if (buf == NULL) {
		return;
	}
	buf->cb.flag = DMA_BUF_EMPTY;
	buf->cb.next = NULL;
	buf->cb.txfr_len = 0;
	buf->cb.nextconbk = 0;
}

static bool dma_chain_contains(uint32_t cb_ad) {
	dma_buf_t* c = _chain;

	while (c != NULL) {
		if (c->cb.cb_ad == cb_ad) {
			return true;
		}
		c = c->cb.next;
	}
	return false;
}

/* Append the current fill buffer to the queued chain (data is stable then). */
static void dma_chain_seal_fill(void) {
	if (_fill == NULL || _fill->cb.txfr_len == 0) {
		return;
	}
	_fill->cb.flag = DMA_BUF_QUEUED;
	_fill->cb.next = NULL;
	if (_tail != NULL) {
		_tail->cb.next = _fill;
		_tail->cb.nextconbk = _fill->cb.cb_ad;
	}
	else {
		_chain = _fill;
	}
	_tail = _fill;
	_fill = NULL;
}

static void dma_chain_hw_start(void) {
	if (_chain == NULL || _chain->cb.txfr_len == 0) {
		return;
	}
	/* Same start sequence as raspix soundd: reset, enable, load CB, go. */
	dma_reg_write(DMA_CS, DMA_CS_RESET);
	(void)dma_reg_read(DMA_CS);
	*(volatile uint32_t*)(uintptr_t)DMA_ENABLE_REG |= DMA_ENABLE_BIT;
	dma_reg_write(DMA_CONBLK_AD, _chain->cb.cb_ad);
	dma_reg_write(DMA_CS, DMA_CS_ACTIVE | DMA_CS_PRIORITY | DMA_CS_PANIC);
	_started_tail = _tail;
}

void dma_chain_init(void) {
	uint8_t* buf = (uint8_t*)(uintptr_t)dma_alloc(0, DMA_BUF_SIZE * DMA_BUF_CNT);

	_chain = NULL;
	_tail = NULL;
	_fill = NULL;
	_started_tail = NULL;

	/* enable this DMA channel, then reset it */
	*(volatile uint32_t*)(uintptr_t)DMA_ENABLE_REG |= DMA_ENABLE_BIT;
	dma_reg_write(DMA_CS, DMA_CS_RESET);
	(void)dma_reg_read(DMA_CS);
	dma_reg_write(DMA_CONBLK_AD, 0);

	for (int i = 0; i < DMA_BUF_CNT; i++) {
		dma_buf[i] = (dma_buf_t*)(buf + i * DMA_BUF_SIZE);
		memset(dma_buf[i], 0, sizeof(dma_cb_t));
		dma_buf[i]->cb.ti = DMA_CB_TI;
		dma_buf[i]->cb.cb_ad = dma_phy_addr(0, (ewokos_addr_t)(uintptr_t)dma_buf[i]) | 0xC0000000;
		dma_buf[i]->cb.source_ad = dma_buf[i]->cb.cb_ad + sizeof(dma_cb_t);
		dma_buf[i]->cb.dest_ad = 0x7E203004; /* PCM FIFO_A bus address */
		dma_buf[i]->cb.txfr_len = 0;
		dma_buf[i]->cb.stride = 0;
		dma_buf[i]->cb.nextconbk = 0;
		dma_buf[i]->cb.debug = 0;
		dma_buf[i]->cb.flag = DMA_BUF_EMPTY;
		dma_buf[i]->cb.next = NULL;
	}

	_fill = dma_buf_alloc();
}

void dma_chain_reset(void) {
	dma_reg_write(DMA_CS, DMA_CS_RESET);
	(void)dma_reg_read(DMA_CS);
	dma_reg_write(DMA_CONBLK_AD, 0);
	*(volatile uint32_t*)(uintptr_t)DMA_ENABLE_REG |= DMA_ENABLE_BIT;

	for (int i = 0; i < DMA_BUF_CNT; i++) {
		dma_buf_free(dma_buf[i]);
	}
	_chain = NULL;
	_tail = NULL;
	_started_tail = NULL;
	_fill = dma_buf_alloc();
}

int dma_chain_push(const uint8_t* buf, int size) {
	int ret = 0;

	while (size > 0) {
		int space;
		int len;

		if (_fill == NULL) {
			_fill = dma_buf_alloc();
			if (_fill == NULL) {
				break;
			}
		}

		space = (int)DMA_DATA_SIZE - (int)_fill->cb.txfr_len;
		len = space < size ? space : size;
		memcpy(_fill->data + _fill->cb.txfr_len, buf, len);
		_fill->cb.txfr_len += (uint32_t)len;
		buf += len;
		size -= len;
		ret += len;

		if (_fill->cb.txfr_len >= DMA_DATA_SIZE) {
			dma_chain_seal_fill();
		}
	}
	return ret;
}

void dma_chain_flush(void) {
	uint32_t cs = dma_reg_read(DMA_CS);

	if ((cs & DMA_CS_ACTIVE) != 0) {
		/* DMA running: free queued buffers fully behind the current CB. */
		uint32_t cb_ad = dma_reg_read(DMA_CONBLK_AD);
		bool passed_started_tail = false;

		if (cb_ad != 0 && dma_chain_contains(cb_ad)) {
			while (_chain != NULL && _chain->cb.cb_ad != cb_ad) {
				dma_buf_t* done = _chain;
				if (done == _started_tail) {
					passed_started_tail = true;
				}
				_chain = done->cb.next;
				dma_buf_free(done);
			}
			if (passed_started_tail) {
				_started_tail = _chain;
			}
			if (_chain == NULL) {
				_tail = NULL;
			}
		}
		return;
	}

	/*
	 * DMA idle. On this silicon CONBLK_AD reads 0 at chain end, so the
	 * END flag is the completion signal: when set, the launched chain
	 * (head.._started_tail) has been fully consumed and can be freed.
	 * Buffers appended after the launch stay queued for the next start.
	 */
	if ((cs & DMA_CS_END) != 0 && _started_tail != NULL) {
		while (_chain != NULL) {
			dma_buf_t* done = _chain;
			bool last = (done == _started_tail);
			_chain = done->cb.next;
			dma_buf_free(done);
			if (last) {
				break;
			}
		}
		if (_chain == NULL) {
			_tail = NULL;
		}
		_started_tail = NULL;
	}

	/*
	 * Seal a partially filled tail buffer before (re)starting DMA so the
	 * controller never latches a CB whose txfr_len is still growing.
	 */
	dma_chain_seal_fill();
	if (_chain != NULL && _chain->cb.txfr_len > 0) {
		dma_chain_hw_start();
	}
}

uint32_t dma_chain_avail_bytes(void) {
	uint32_t used = 0;
	uint32_t capacity = DMA_DATA_SIZE * DMA_BUF_CNT;

	for (int i = 0; i < DMA_BUF_CNT; i++) {
		if (dma_buf[i]->cb.flag != DMA_BUF_EMPTY) {
			used += dma_buf[i]->cb.txfr_len;
		}
	}

	if (used >= capacity) {
		return 0;
	}
	return capacity - used;
}
