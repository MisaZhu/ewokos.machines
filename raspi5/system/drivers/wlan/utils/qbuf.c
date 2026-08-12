#include <stdlib.h>
#include <types.h>
#include <string.h>
#include <pthread.h>

#include "qbuf.h"
#include "log.h"




queue_buffer_t *queue_buffer_alloc(int qsize, int bsize){
	int i;
	queue_buffer_t *qbuf = malloc(sizeof(queue_buffer_t));
	if(qbuf){
		qbuf->bsize = bsize;
		qbuf->push_idx = 0;
		qbuf->pop_idx = 0;
		qbuf->bufs = malloc(sizeof(buf_t) * qsize);
		if(!qbuf->bufs){
			free(qbuf);
			return NULL;
		}
		for(i = 0; i < qsize; i++){
			qbuf->bufs[i].size = 0;
			qbuf->bufs[i].data = malloc(bsize);
			if(qbuf->bufs[i].data == NULL){
				/*
				 * Fail loudly instead of silently truncating qsize:
				 * a partially-allocated queue looks healthy but
				 * overflows constantly under load, which is far
				 * harder to diagnose than an init-time ENOMEM.
				 */
				brcm_log("qbuf alloc failed at %d/%d (bsize=%d)\n",
						i, qsize, bsize);
				while(i > 0)
					free(qbuf->bufs[--i].data);
				free(qbuf->bufs);
				free(qbuf);
				return NULL;
			}
		}
		qbuf->qsize = qsize;
		pthread_mutex_init(&qbuf->lock, NULL);
	}
	return qbuf;
}

void queue_buffer_free(queue_buffer_t *qbuf){
	int i;

	if(!qbuf)
		return;
	if(qbuf->bufs){
		for(i = 0; i < qbuf->qsize; i++){
			free(qbuf->bufs[i].data);
		}
		free(qbuf->bufs);
	}
	pthread_mutex_destroy(&qbuf->lock);
	free(qbuf);
}

int queue_buffer_push(queue_buffer_t *qbuf, uint8_t* buf, int size){
	buf_t *dst = NULL;
	int ret = 0;

	/*
	 * Reject empty and oversize payloads outright. A size<=0 push used
	 * to enqueue a zero-length entry while returning 0 (mistaken for
	 * queue-full by callers); size>bsize used to silently truncate the
	 * frame, delivering corrupt data to the consumer.
	 */
	if(!qbuf || !buf || size <= 0 || size > qbuf->bsize)
		return 0;

	pthread_mutex_lock(&qbuf->lock);
	if(qbuf->push_idx - qbuf->pop_idx < qbuf->qsize){
		int idx = qbuf->push_idx % qbuf->qsize;
		dst = &qbuf->bufs[idx];
	}
	
	if(dst){
		memcpy(dst->data, buf, size);
		dst->size = size;
		qbuf->push_idx++;
		ret = size;
	}
	pthread_mutex_unlock(&qbuf->lock);
	return ret;
}

int queue_buffer_pop(queue_buffer_t *qbuf, uint8_t* buf, int size){
	buf_t *src = NULL;
	int ret = 0;

	pthread_mutex_lock(&qbuf->lock);
	if(qbuf->push_idx > qbuf->pop_idx){
		int idx = qbuf->pop_idx % qbuf->qsize;
 		src = &qbuf->bufs[idx];
	}

	if(src){
		size = min(src->size, size);
		memcpy(buf, src->data, size);
		qbuf->pop_idx++;
		if(qbuf->push_idx >= qbuf->qsize && qbuf->pop_idx >= qbuf->qsize){
			qbuf->push_idx -= qbuf->qsize;
			qbuf->pop_idx -= qbuf->qsize;
		}
		ret = size;
	}
	pthread_mutex_unlock(&qbuf->lock);
	return ret;
}

int queue_buffer_check(queue_buffer_t *qbuf){
	int depth;

	pthread_mutex_lock(&qbuf->lock);
	depth = qbuf->push_idx - qbuf->pop_idx;
	pthread_mutex_unlock(&qbuf->lock);
	return depth;
}

void queue_buffer_reset(queue_buffer_t *qbuf){
	pthread_mutex_lock(&qbuf->lock);
	qbuf->pop_idx = qbuf->push_idx;
	pthread_mutex_unlock(&qbuf->lock);
}
