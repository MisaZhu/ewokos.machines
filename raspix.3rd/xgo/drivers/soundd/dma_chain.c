#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ewoksys/dma.h>
#include <ewoksys/vfs.h>

#include "dma_chain.h"
 
#define write32(p, v)   (*((uint32_t*)(p)) = ((uint32_t)(v)))
#define read32(p)       (*((uint32_t*)(p)))

static dma_buf_t* dma_buf[DMA_BUF_CNT];
static dma_buf_t *_chain;

static dma_buf_t *dma_buf_alloc(int size){

	if(size > DMA_BUF_SIZE){
		return NULL;
	}

	for(int i = 0; i < DMA_BUF_CNT; i++){
		if(dma_buf[i]->cb.flag == 0){
			dma_buf[i]->cb.flag = 1;
			dma_buf[i]->cb.next = NULL;
			dma_buf[i]->cb.txfr_len = 0;
			dma_buf[i]->cb.nextconbk = 0;
			return  dma_buf[i];
		}
	}

	return NULL;
}

static void dma_buf_free(dma_buf_t *buf){
	if(buf == NULL)
		return;
	buf->cb.flag = 0;
}

static int dma_chain_size(dma_buf_t *chain){
	int ret = 0;

	while(chain != NULL){
		ret++;
		chain = chain->cb.next;
	}
	return ret;
}

void dma_chain_init(void){
    uint8_t *buf =  (uint8_t*)dma_map(DMA_BUF_SIZE*DMA_BUF_CNT);
	
	write32(DMA_BASE + DMA_CS, 0x1 << 31);

    for(int i = 0; i < DMA_BUF_CNT; i++){
        dma_buf[i] = (dma_buf_t*)(buf + i * DMA_BUF_SIZE);
		memset(dma_buf[i], 0 , sizeof(dma_cb_t));
        dma_buf[i]->cb.ti = (1 << 26) | (1 << 3) | (1 << 8) | (2 << 16) | ( 1 << 6);
        dma_buf[i]->cb.cb_ad = dma_phy_addr((uint32_t)dma_buf[i]) | 0xC0000000;
        dma_buf[i]->cb.source_ad = dma_buf[i]->cb.cb_ad + sizeof(dma_cb_t);
        dma_buf[i]->cb.dest_ad = 0x7E203004;
        dma_buf[i]->cb.txfr_len = 0;
        dma_buf[i]->cb.stride = 0;
        dma_buf[i]->cb.nextconbk = 0;
        dma_buf[i]->cb.debug = 0;
		dma_buf[i]->cb.flag = 0;
		dma_buf[i]->cb.next = 0;
    }

	_chain = dma_buf_alloc(DMA_DATA_SIZE);
}

int dma_chain_push(const uint8_t *buf, int size){

	dma_buf_t *c = _chain;
	int ret = 0;
	while(c != NULL){
		if(c->cb.txfr_len < DMA_DATA_SIZE){
			int len = DMA_DATA_SIZE - c->cb.txfr_len;
			len = (len > size) ? size : len;
			memcpy(c->data + c->cb.txfr_len,  buf, len);
			c->cb.txfr_len += len;
			size -= len;
			buf += len;
			ret += len;
			if(size <= 0)
				return ret;
		}
		if(c->cb.next == NULL){
			dma_buf_t *n =  dma_buf_alloc(DMA_DATA_SIZE);
			if(n){
				c->cb.next = n;
				c->cb.nextconbk = n->cb.cb_ad;
			}
		}
		c = c->cb.next;
	}
	return ret;
}

void dma_chain_flush(void){
	if((read32(DMA_BASE + DMA_CS) & 0x1) == 0){
		if(dma_chain_size(_chain) > DMA_BUF_TH){
			write32(DMA_BASE + DMA_CONBLK_AD, _chain->cb.cb_ad);
			write32(DMA_BASE + DMA_CS,  0x1|(0x8<<20)|(0x8<<16));
		}
	}else{
        uint32_t cb_ad = read32(DMA_BASE + DMA_CONBLK_AD);
        while(_chain != NULL){
            if(_chain->cb.cb_ad == cb_ad)
                break;
            dma_buf_free(_chain);
			_chain = _chain->cb.next;
		}
	}

	if(_chain == NULL){
		_chain = dma_buf_alloc(DMA_DATA_SIZE);
	}
}
