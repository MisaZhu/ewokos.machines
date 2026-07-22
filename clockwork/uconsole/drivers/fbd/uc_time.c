#include "uc_time.h"

#include <ewoksys/mmio.h>
#include <unistd.h>

/*
 * BCM2711 System Timer registers live at MMIO+0x3000. CLO (offset 0x04)
 * is the low 32 bits of the 1 MHz microsecond counter.
 */
#define STC_CLO_OFFSET  0x3004U

static volatile uint32_t* _stc_clo = 0;

void uc_time_init(void) {
	if (_stc_clo == 0 && _mmio_base != 0) {
		_stc_clo = (volatile uint32_t*)(uintptr_t)(_mmio_base + STC_CLO_OFFSET);
	}
}

uint32_t uc_micros(void) {
	uc_time_init();
	if (_stc_clo == 0) {
		return 0;
	}
	return *_stc_clo;
}

void uc_udelay(uint32_t us) {
	uc_time_init();
	if (_stc_clo == 0) {
		/*
		 * Fallback: coarse cycle-counting busy-loop. Roughly calibrated
		 * for a 1.5 GHz CM4 A72 (about 1500 cycles per μs); actual timing
		 * depends on the compiler and CPU state, but this is only used
		 * before MMIO is mapped.
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
	uint32_t start = *_stc_clo;
	while ((uint32_t)(*_stc_clo - start) < us) {
		/* spin */
	}
}

void uc_mdelay(uint32_t ms) {
	if (ms == 0) {
		return;
	}
	usleep(ms * 1000U);
}
