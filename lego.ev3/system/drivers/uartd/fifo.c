#include <stdio.h>
#include <stdlib.h>
#include "fifo.h"

fifo_t *fifo_new(int size){
	fifo_t *f = malloc(size + sizeof(fifo_t));
	f->tsize = size;
	f->wptr = 0;
	f->rptr = 0;
	return f;
}

void fifo_free(fifo_t* f){
	free(f);	
}

int fifo_is_empty(fifo_t* f){
	return f->dsize == 0;
}

int fifo_is_full(fifo_t* f){
	return f->dsize >= f->tsize;
}

void fifo_push(fifo_t* f, char c){
	f->data[f->wptr % f->tsize] = c;
	f->dsize++;
	f->wptr++;
}

char fifo_pop(fifo_t* f){
	char c = f->data[f->rptr % f->tsize];
	f->dsize--;
	f->rptr++;
	return c;
}

void fifo_push_unsafe(fifo_t* f, char c){
	if(f->wptr > f->tsize && f->rptr > f->tsize){
		f->wptr -= f->tsize;
		f->rptr -= f->tsize;
	}

	f->data[f->wptr % f->tsize] = c;
	f->wptr++;
	f->dsize = f->wptr - f->rptr;
}

char fifo_pop_unsafe(fifo_t* f){
	if(f->wptr > f->tsize && f->rptr > f->tsize){
		f->wptr -= f->tsize;
		f->rptr -= f->tsize;
	}
	
	char c = f->data[f->rptr % f->tsize];
	f->rptr++;
	f->dsize = f->wptr - f->rptr;
	return c;
}

