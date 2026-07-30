#ifndef __SDIO_H__
#define __SDIO_H__

#include <types.h>

#define R4_18V_PRESENT      (1 << 24)
#define R4_MEMORY_PRESENT   (1 << 27)

#define SDIO_CCCR_CCCR      0x00
#define SDIO_CCCR_SD        0x01
#define SDIO_CCCR_IOEx      0x02
#define SDIO_CCCR_IORx      0x03
#define SDIO_CCCR_IENx      0x04    /* Function/Master Interrupt Enable */
#define SDIO_CCCR_INTx      0x05    /* Function Interrupt Pending */
#define SDIO_CCCR_ABORT     0x06    /* function abort/card reset */
#define SDIO_CCCR_IF        0x07    /* bus interface controls */

#define  SDIO_BUS_WIDTH_MASK    0x03    /* data bus width setting */
#define  SDIO_BUS_WIDTH_1BIT    0x00
#define  SDIO_BUS_WIDTH_4BIT    0x02
#define  SDIO_BUS_CD_DISABLE    0x80    /* disable pull-up on DAT3 (pin 1) */

#define SDIO_CCCR_CAPS      0x08
#define SDIO_CCCR_CIS       0x09    /* common CIS pointer (3 bytes) */
#define SDIO_CCCR_BLKSIZE   0x10
#define SDIO_CCCR_POWER     0x12
#define SDIO_CCCR_SPEED     0x13

#define  SDIO_SPEED_SHS     0x01    /* Supports High-Speed mode */
#define  SDIO_SPEED_BSS_SHIFT   1
#define  SDIO_SPEED_BSS_MASK    (7<<SDIO_SPEED_BSS_SHIFT)
#define  SDIO_SPEED_SDR12   (0<<SDIO_SPEED_BSS_SHIFT)
#define  SDIO_SPEED_SDR25   (1<<SDIO_SPEED_BSS_SHIFT)
#define  SDIO_SPEED_EHS     SDIO_SPEED_SDR25   /* Enable High-Speed */

#define SDIO_FBR_BASE(f)    ((f) * 0x100) /* base of function f's FBRs */
#define SDIO_FBR_STD_IF     0x00
#define SDIO_FBR_STD_IF_EXT 0x01
#define SDIO_FBR_POWER      0x02
#define SDIO_FBR_CIS        0x09    /* CIS pointer (3 bytes) */
#define SDIO_FBR_CSA        0x0C    /* CSA pointer (3 bytes) */
#define SDIO_FBR_BLKSIZE    0x10    /* block size (2 bytes) */

/* CIS tuple codes */
#define CISTPL_NULL         0x00
#define CISTPL_MANFID       0x20
#define CISTPL_FUNCID       0x21
#define CISTPL_FUNCE        0x22
#define CISTPL_END          0xff

int sdio_memcpy_fromio(int func, void *dst, unsigned int addr, int count);
int sdio_memcpy_toio(int func, unsigned int addr, void *src, int count);
uint8_t sdio_readb(int func, unsigned int addr, int *err_ret);
void sdio_writeb(int func, uint8_t b, unsigned int addr, int *err_ret);
int sdio_readsb(int func, void *dst, unsigned int addr, int count);
int sdio_writesb(int func, unsigned int addr, void *src, int count);
uint16_t sdio_readw(int func, unsigned int addr, int *err_ret);
void sdio_writew(int func, uint16_t b, unsigned int addr, int *err_ret);
uint32_t sdio_readl(int func, unsigned int addr, int *err_ret);
void sdio_writel(int func, uint32_t b, unsigned int addr, int *err_ret);

int sdio_reset(void);
int sdio_set_block_size(unsigned int func, unsigned int blksz);
int sdio_enable_func(int func);
int sdio_disable_func(int func);
int sdio_claim_irq(int func);

/* parse CISTPL_MANFID of the common CIS: vendor/device id of the card */
int sdio_get_manfid(uint16_t *vendor, uint16_t *device);

#endif /* __SDIO_H__ */
