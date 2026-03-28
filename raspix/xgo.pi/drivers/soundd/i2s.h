#ifndef __BCM_I2S__
#define __BCM_I2S__

#include <ewoksys/mmio.h>

#define ARM_PCM_BASE		(_mmio_base + 0x203000)


#define ARM_PCM_CS_A		(ARM_PCM_BASE + 0x00)
#define ARM_PCM_FIFO_A		(ARM_PCM_BASE + 0x04)
#define ARM_PCM_MODE_A		(ARM_PCM_BASE + 0x08)
#define ARM_PCM_RXC_A		(ARM_PCM_BASE + 0x0C)
#define ARM_PCM_TXC_A		(ARM_PCM_BASE + 0x10)
#define ARM_PCM_DREQ_A		(ARM_PCM_BASE + 0x14)
#define ARM_PCM_INTEN_A		(ARM_PCM_BASE + 0x18)
#define ARM_PCM_INTSTC_A	(ARM_PCM_BASE + 0x1C)
#define ARM_PCM_GRAY		(ARM_PCM_BASE + 0x20)


#define CHANS			2			// 2 I2S stereo channels
#define CHANLEN			16			// width of a channel slot in bits

//
// PCM / I2S registers
//
#define CS_A_STBY		(1 << 25)
#define CS_A_SYNC		(1 << 24)
#define CS_A_RXSEX		(1 << 23)
#define CS_A_TXE		(1 << 21)
#define CS_A_TXD		(1 << 19)
#define CS_A_TXW		(1 << 17)
#define CS_A_TXERR		(1 << 15)
#define CS_A_TXSYNC		(1 << 13)
#define CS_A_DMAEN		(1 << 9)
#define CS_A_TXTHR__SHIFT	5
#define CS_A_RXCLR		(1 << 4)
#define CS_A_TXCLR		(1 << 3)
#define CS_A_TXON		(1 << 2)
#define CS_A_RXON		(1 << 1)
#define CS_A_EN			(1 << 0)

#define MODE_A_CLKI		(1 << 22)
#define MODE_A_CLKM		(1 << 23)
#define MODE_A_FSI		(1 << 20)
#define MODE_A_FSM		(1 << 21)
#define MODE_A_FLEN__SHIFT	10
#define MODE_A_FSLEN__SHIFT	0

#define RXC_A_CH1WEX		(1 << 31)
#define RXC_A_CH1EN		(1 << 30)
#define RXC_A_CH1POS__SHIFT	20
#define RXC_A_CH1WID__SHIFT	16
#define RXC_A_CH2WEX		(1 << 15)
#define RXC_A_CH2EN		(1 << 14)
#define RXC_A_CH2POS__SHIFT	4
#define RXC_A_CH2WID__SHIFT	0

#define TXC_A_CH1WEX		(1 << 31)
#define TXC_A_CH1EN		(1 << 30)
#define TXC_A_CH1POS__SHIFT	20
#define TXC_A_CH1WID__SHIFT	16
#define TXC_A_CH2WEX		(1 << 15)
#define TXC_A_CH2EN		(1 << 14)
#define TXC_A_CH2POS__SHIFT	4
#define TXC_A_CH2WID__SHIFT	0

#define DREQ_A_TX__SHIFT	8
#define DREQ_A_TX__MASK		(0x7F << 8)
#define DREQ_A_RX__SHIFT	0
#define DREQ_A_RX__MASK		(0x7F << 0)

#define CM_BASE   (_mmio_base + 0x101000) // Clock manager base address
#define CM_PASSWORD 0x5A000000  // Clock Control: Password "5A"
#define CM_SRC_OSCILLATOR 0x01   // Clock Control: Clock Source = Oscillator
#define CM_ENABLE 0x10
#define CM_I2SCTL 0x98  // Clock Manager I2S Clock Control 
#define CM_I2SDIV 0x9c  // Clock Manager 12SClock Divisor 
						//
void pcm_init(void);
int pcm_write(uint8_t *buf, int size);
int pcm_read(uint8_t *buf, int size);
//void pcm_test(void);

#endif
