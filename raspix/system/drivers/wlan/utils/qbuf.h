#ifndef __QUEUE_T__
#define __QUEUE_T__ 

#include <types.h>
#include <pthread.h>

typedef struct _buf{
	int size;
	uint8_t *data;
}buf_t;

typedef struct _queue_buffer{
    int qsize;
    int bsize;
    buf_t *bufs;
    int push_idx;
    int pop_idx;
    /*
     * Serializes push/pop/check/reset. The queues cross thread
     * boundaries (producer = worker/DPC thread, consumer = vdevice IPC
     * thread), and the unsynchronized index wrap in pop() could clobber
     * a concurrent push_idx increment, losing frames. Leaf lock: never
     * call out to other locking code while holding it.
     */
    pthread_mutex_t lock;
}queue_buffer_t;

queue_buffer_t *queue_buffer_alloc(int qsize, int bsize);
int queue_buffer_push(queue_buffer_t *qbuf, uint8_t* buf, int size);
int queue_buffer_pop(queue_buffer_t *qbuf, uint8_t* buf, int size);
int queue_buffer_check(queue_buffer_t *qbuf);
void queue_buffer_reset(queue_buffer_t *qbuf);
#endif