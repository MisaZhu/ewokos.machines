#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include <ewoksys/proc.h>
#include <ewoksys/sys.h>
#include <ewoksys/syscall.h>
#include <ewoksys/vfs.h>
#include <sysinfo.h>

#include <arch/bcm283x/gpio.h>
#include <arch/bcm283x/i2s.h>

#define write32(p, v)	(*((uint32_t*)(p)) = ((uint32_t)(v)))
#define read32(p)	(*((uint32_t*)(p)))

static void gpio_init(void) {
	bcm283x_gpio_config(18, GPIO_ALTF0);
	bcm283x_gpio_config(19, GPIO_ALTF0);
	bcm283x_gpio_config(20, GPIO_ALTF0);
	bcm283x_gpio_config(21, GPIO_ALTF0);
}

static void clock_init(uint32_t bclk_hz) {
	sys_info_t sysinfo;
	uint32_t osc_hz = 19200000u;
	uint32_t divi;
	uint32_t divf;
	uint32_t div_x4096;

	sys_get_sys_info(&sysinfo);
	if (sysinfo.mmio.phy_base == 0xfe000000u) {
		osc_hz = 54000000u;
	}
	if (bclk_hz == 0) {
		bclk_hz = 1;
	}
	div_x4096 = (uint32_t)(((uint64_t)osc_hz << 12) / (uint64_t)bclk_hz);
	divi = div_x4096 >> 12;
	divf = div_x4096 & 0xFFFu;

	write32(CM_BASE + CM_I2SCTL, CM_PASSWORD | CM_SRC_OSCILLATOR);
	proc_usleep(10);
	write32(CM_BASE + CM_I2SDIV, CM_PASSWORD | (divi << 12) | divf);
	proc_usleep(10);
	write32(CM_BASE + CM_I2SCTL, CM_PASSWORD | CM_SRC_OSCILLATOR | CM_ENABLE);
	proc_usleep(10);
}

static uint32_t pcm_sample_width_bits(uint32_t sample_bits) {
	if (sample_bits < 16) {
		sample_bits = 16;
	}
	return sample_bits - 16;
}

static void pcm_init_cfg(uint32_t slot_bits, uint32_t sample_bits, bool tx_enable) {
	uint32_t nModeA;
	uint32_t width_bits;

	clock_init(48000u * 2u * slot_bits);
	gpio_init();

	write32(ARM_PCM_CS_A, 1 << 4);
	proc_usleep(10);
	write32(ARM_PCM_CS_A, 0);
	proc_usleep(10);
	write32(ARM_PCM_CS_A, read32(ARM_PCM_CS_A) | CS_A_TXCLR | CS_A_RXCLR);
	proc_usleep(10);

	width_bits = pcm_sample_width_bits(sample_bits);
	write32(ARM_PCM_TXC_A, TXC_A_CH1WEX |
			TXC_A_CH1EN |
			(1 << TXC_A_CH1POS__SHIFT) |
			(width_bits << TXC_A_CH1WID__SHIFT) |
			TXC_A_CH2WEX |
			TXC_A_CH2EN |
			((slot_bits + 1) << TXC_A_CH2POS__SHIFT) |
			(width_bits << TXC_A_CH2WID__SHIFT));

	if (slot_bits <= 16u) {
		/* exact slot-wide channels: width = WID + 8, no WEX */
		uint32_t rx_wid = slot_bits - 8u;
		write32(ARM_PCM_RXC_A, RXC_A_CH1EN |
				(1 << RXC_A_CH1POS__SHIFT) |
				(rx_wid << RXC_A_CH1WID__SHIFT) |
				RXC_A_CH2EN |
				((slot_bits + 1) << RXC_A_CH2POS__SHIFT) |
				(rx_wid << RXC_A_CH2WID__SHIFT));
	}
	else {
		write32(ARM_PCM_RXC_A, RXC_A_CH1WEX |
				RXC_A_CH1EN |
				(1 << RXC_A_CH1POS__SHIFT) |
				(width_bits << RXC_A_CH1WID__SHIFT) |
				RXC_A_CH2WEX |
				RXC_A_CH2EN |
				((slot_bits + 1) << RXC_A_CH2POS__SHIFT) |
				(width_bits << RXC_A_CH2WID__SHIFT));
	}

	nModeA = MODE_A_CLKI |
			(1 << 24) |
			MODE_A_FSI |
			((CHANS * slot_bits - 1) << MODE_A_FLEN__SHIFT) |
			(slot_bits << MODE_A_FSLEN__SHIFT);
	write32(ARM_PCM_MODE_A, nModeA);

	write32(ARM_PCM_CS_A, read32(ARM_PCM_CS_A) | (0x2 << CS_A_TXTHR__SHIFT));
	write32(ARM_PCM_DREQ_A, (0x10 << DREQ_A_TX__SHIFT) | 0x10);

	write32(ARM_PCM_CS_A, read32(ARM_PCM_CS_A) | CS_A_STBY);
	proc_usleep(50);

	write32(ARM_PCM_CS_A, read32(ARM_PCM_CS_A) | CS_A_EN);
	proc_usleep(10);

	write32(ARM_PCM_CS_A, read32(ARM_PCM_CS_A) | CS_A_DMAEN);
	if (tx_enable) {
		write32(ARM_PCM_CS_A, read32(ARM_PCM_CS_A) | CS_A_TXON | CS_A_RXON | CS_A_RXSEX);
	}
	else {
		write32(ARM_PCM_CS_A, read32(ARM_PCM_CS_A) | CS_A_RXON | CS_A_RXSEX);
	}
}

void pcm_init(void) {
	pcm_init_cfg(16u, 16u, true);
}

/* RX-only, 16-bit slots: matches WM8960 codec ADC output (I2S 16-bit). */
void pcm_init_rx16(void) {
	pcm_init_cfg(16u, 16u, false);
}

int pcm_write(uint8_t* buf, int size) {
	int idx = 0;
	uint32_t* p = (uint32_t*)buf;

	size /= 4;
	while (idx < size) {
		if ((read32(ARM_PCM_CS_A) & CS_A_TXW) == CS_A_TXW) {
			write32(ARM_PCM_FIFO_A, p[idx]);
			idx++;
		}
	}
	return 0;
}

int pcm_read(uint8_t* buf, int size) {
	int idx = 0;
	uint32_t* p = (uint32_t*)buf;

	if (buf == NULL || size < (int)sizeof(uint32_t)) {
		return 0;
	}

	size /= (int)sizeof(uint32_t);
	while (idx < size) {
		if ((read32(ARM_PCM_CS_A) & CS_A_RXD) != CS_A_RXD) {
			break;
		}
		p[idx] = read32(ARM_PCM_FIFO_A);
		idx++;
	}
	return idx * (int)sizeof(uint32_t);
}
