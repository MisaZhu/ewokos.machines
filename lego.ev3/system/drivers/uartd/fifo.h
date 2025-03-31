#ifndef __FIFO_H__
#define __FIFO_H__

#include <stdint.h>

typedef struct{
	uint32_t tsize;
	uint32_t wptr;
	uint32_t rptr;
	uint32_t dsize;
	char data[1];
}fifo_t;


fifo_t *fifo_new(int size);
void fifo_free(fifo_t* f);
void fifo_push(fifo_t* f, char c);
char fifo_pop(fifo_t* f);

int fifo_is_empty_unsafe(fifo_t* f);
int fifo_is_full_unsafe(fifo_t* f);
void fifo_push_unsafe(fifo_t* f, char c);
char fifo_pop_unsafe(fifo_t* f);

#endif
