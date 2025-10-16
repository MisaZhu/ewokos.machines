#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <ewoksys/syscall.h>
#include <ewoksys/vfs.h>
#include "arch/bcm283x/gpio.h"
#include "i2s.h"

#define write32(p, v)	(*((uint32_t*)(p)) = ((uint32_t)(v)))
#define read32(p)		(*((uint32_t*)(p)))

//#include "data_raw.h"
//void pcm_test(void){
//	printf("play test audio...\n");
//	int loop = 0;
//		int idx = 0;
//		while(idx < data_raw_len / 4){
//			if( (read32(ARM_PCM_CS_A) & CS_A_TXW) == CS_A_TXW){
//				write32(ARM_PCM_FIFO_A, ((uint32_t*)data_raw)[idx]);
//				idx++;
//			}
//		}
//		printf("loop %d\n", loop++);
//}

void gpio_init(void) {
    // Configure GPIO pins 18, 19, 20, 21 for ALT0 function (I2S)
	bcm283x_gpio_config(18, GPIO_ALTF0);
	bcm283x_gpio_config(19, GPIO_ALTF0);
	bcm283x_gpio_config(20, GPIO_ALTF0);
	bcm283x_gpio_config(21, GPIO_ALTF0);
}

void clock_init(void){
	write32(CM_BASE + CM_I2SCTL, CM_PASSWORD | CM_SRC_OSCILLATOR);
	write32(CM_BASE + CM_I2SDIV, CM_PASSWORD |  (0x23<<12) | 0xA0);
	write32(CM_BASE + CM_I2SCTL, CM_PASSWORD | CM_SRC_OSCILLATOR | CM_ENABLE);
}


void pcm_init(void){

	clock_init();
	gpio_init();

	//reset I2S
	write32 (ARM_PCM_CS_A, 1<<4);
	proc_usleep(10);
	write32 (ARM_PCM_CS_A, 0);
	proc_usleep(10);
	// clearing FIFOs
	write32 (ARM_PCM_CS_A, read32 (ARM_PCM_CS_A) | CS_A_TXCLR | CS_A_RXCLR);
	proc_usleep(10);

	// enable channel 1 and 2
	write32 (ARM_PCM_TXC_A,   TXC_A_CH1WEX
				| TXC_A_CH1EN
				| (1 << TXC_A_CH1POS__SHIFT)
				| (0 << TXC_A_CH1WID__SHIFT)
				| TXC_A_CH2WEX
				| TXC_A_CH2EN
				| ((CHANLEN+1) << TXC_A_CH2POS__SHIFT)
				| (0 << TXC_A_CH2WID__SHIFT));

	write32 (ARM_PCM_RXC_A,   RXC_A_CH1WEX
			| RXC_A_CH1EN
			| (1 << RXC_A_CH1POS__SHIFT)
			| (0 << RXC_A_CH1WID__SHIFT)
			| RXC_A_CH2WEX
			| RXC_A_CH2EN
			| ((CHANLEN+1) << RXC_A_CH2POS__SHIFT)
			| (0 << RXC_A_CH2WID__SHIFT));

	uint32_t nModeA =   MODE_A_CLKI
		 | 1 << 24
	     | MODE_A_FSI
	     | ((CHANS*CHANLEN-1) << MODE_A_FLEN__SHIFT)
	     | (CHANLEN << MODE_A_FSLEN__SHIFT);

	write32 (ARM_PCM_MODE_A, nModeA);

	// set fifo
	write32(ARM_PCM_CS_A, read32(ARM_PCM_CS_A) | (0x2 << CS_A_TXTHR__SHIFT));

	// disable standby
	write32 (ARM_PCM_CS_A, read32 (ARM_PCM_CS_A) | CS_A_STBY);
	proc_usleep(50);

	// enable I2S
	write32 (ARM_PCM_CS_A, read32 (ARM_PCM_CS_A) | CS_A_EN);
	proc_usleep(10);

	write32 (ARM_PCM_CS_A, read32 (ARM_PCM_CS_A) | CS_A_DMAEN);
	write32 (ARM_PCM_CS_A, read32 (ARM_PCM_CS_A) | CS_A_TXON | CS_A_RXON | CS_A_RXSEX);
	
}

int pcm_write(uint8_t* buf, int size){
	int idx = 0;
	uint32_t *p = (uint32_t*)buf;
	size /= 4;
	while(idx < size){
		if( (read32(ARM_PCM_CS_A) & CS_A_TXW) == CS_A_TXW){
			write32(ARM_PCM_FIFO_A, p[idx]);
			idx++;
		}
	}
	return 0;
}

int i2s_read(uint32_t* buf, int size){

	return size;
}
