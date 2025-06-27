#ifndef __DMA_H__
#define __DMA_H__

#include <ewoksys/mmio.h>

#define DMA_V_BASE				 (_mmio_base + 0x7000)

#define DMA_CS                   0x00
#define DMA_CONBLK_AD            0x04
#define DMA_TI                   0x08
#define DMA_SOURCE_AD            0x0C
#define DMA_DEST_AD              0x10
#define DMA_TXFR_LEN             0x14
#define DMA_STRIDE               0x18
#define DMA_NEXTCONBK            0x1C
#define DMA_DEBUG                0x20

#define DMA_BUF_SIZE	(2048)
#define DMA_BUF_CNT		(32)
#define DMA_BUF_TH		((int)(DMA_BUF_CNT*0.5))

#define DMA_DATA_SIZE	(DMA_BUF_SIZE - sizeof(dma_cb_t))

struct dma_buf;
struct dma_cb;

typedef struct dma_cb{
    uint32_t ti;
    uint32_t source_ad;
    uint32_t dest_ad;
    uint32_t txfr_len;
    uint32_t stride;
    uint32_t nextconbk;
    uint32_t debug;
	uint32_t resv[6];
    uint32_t cb_ad;
	struct dma_buf* next;
	uint32_t flag;
}dma_cb_t;

typedef struct dma_buf{
    dma_cb_t cb;
    uint8_t  data[1];
}dma_buf_t;

void dma_chain_init(void);
int dma_chain_push(const uint8_t *buf, int size);
void dma_chain_flush(void);
#endif


