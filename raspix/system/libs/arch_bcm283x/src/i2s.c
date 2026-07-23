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

static void clock_init(void) {
	/*
	 * BCLK = PCM_CLK (no extra divider in the PCM block).
	 * Target: 48000Hz * 2ch * 16bit = 1.536MHz.
	 * Pi4/CM4 (mmio @0xfe000000): XOSC=54MHz   -> div = 35.15625 = 0x23.280
	 * Pi3/Zero (mmio @0x3f000000): XOSC=19.2MHz -> div = 12.5     = 0x0C.800
	 */
	sys_info_t sysinfo;
	uint32_t divi = 0x0C;
	uint32_t divf = 0x800;

	sys_get_sys_info(&sysinfo);
	if (sysinfo.mmio.phy_base == 0xfe000000u) {
		divi = 0x23;
		divf = 0x280;
	}

	write32(CM_BASE + CM_I2SCTL, CM_PASSWORD | CM_SRC_OSCILLATOR);
	proc_usleep(10);
	write32(CM_BASE + CM_I2SDIV, CM_PASSWORD | (divi << 12) | divf);
	proc_usleep(10);
	write32(CM_BASE + CM_I2SCTL, CM_PASSWORD | CM_SRC_OSCILLATOR | CM_ENABLE);
	proc_usleep(10);
}

void pcm_init(void) {
	uint32_t nModeA;

	clock_init();
	gpio_init();

	write32(ARM_PCM_CS_A, 1 << 4);
	proc_usleep(10);
	write32(ARM_PCM_CS_A, 0);
	proc_usleep(10);
	write32(ARM_PCM_CS_A, read32(ARM_PCM_CS_A) | CS_A_TXCLR | CS_A_RXCLR);
	proc_usleep(10);

	write32(ARM_PCM_TXC_A, TXC_A_CH1WEX |
			TXC_A_CH1EN |
			(1 << TXC_A_CH1POS__SHIFT) |
			(0 << TXC_A_CH1WID__SHIFT) |
			TXC_A_CH2WEX |
			TXC_A_CH2EN |
			((CHANLEN + 1) << TXC_A_CH2POS__SHIFT) |
			(0 << TXC_A_CH2WID__SHIFT));

	write32(ARM_PCM_RXC_A, RXC_A_CH1WEX |
			RXC_A_CH1EN |
			(1 << RXC_A_CH1POS__SHIFT) |
			(0 << RXC_A_CH1WID__SHIFT) |
			RXC_A_CH2WEX |
			RXC_A_CH2EN |
			((CHANLEN + 1) << RXC_A_CH2POS__SHIFT) |
			(0 << RXC_A_CH2WID__SHIFT));

	nModeA = MODE_A_CLKI |
			(1 << 24) |
			MODE_A_FSI |
			((CHANS * CHANLEN - 1) << MODE_A_FLEN__SHIFT) |
			(CHANLEN << MODE_A_FSLEN__SHIFT);
	write32(ARM_PCM_MODE_A, nModeA);

	write32(ARM_PCM_CS_A, read32(ARM_PCM_CS_A) | (0x2 << CS_A_TXTHR__SHIFT));
	write32(ARM_PCM_DREQ_A, (0x10 << DREQ_A_TX__SHIFT) | 0x10);

	write32(ARM_PCM_CS_A, read32(ARM_PCM_CS_A) | CS_A_STBY);
	proc_usleep(50);

	write32(ARM_PCM_CS_A, read32(ARM_PCM_CS_A) | CS_A_EN);
	proc_usleep(10);

	write32(ARM_PCM_CS_A, read32(ARM_PCM_CS_A) | CS_A_DMAEN);
	write32(ARM_PCM_CS_A, read32(ARM_PCM_CS_A) | CS_A_TXON | CS_A_RXON | CS_A_RXSEX);
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
	(void)buf;
	return size;
}
