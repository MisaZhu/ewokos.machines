#include <unistd.h>
#include <stdlib.h>
#include <types.h>

#include "skb.h"
#include "log.h"

#define SKB_POOL_COUNT 8
#define SKB_POOL_MAX_SIZE 16384
#define SKB_POOL_TOTAL (SKB_POOL_MAX_SIZE + SKB_MAX_EXTEND * 2)

static struct sk_buff skb_pool_meta[SKB_POOL_COUNT];
static uint8_t skb_pool_mem[SKB_POOL_COUNT][SKB_POOL_TOTAL];
static uint8_t skb_pool_used[SKB_POOL_COUNT];

struct sk_buff* skb_alloc(uint32_t size){
    if (size <= SKB_POOL_MAX_SIZE) {
        for (int i = 0; i < SKB_POOL_COUNT; i++) {
            if (skb_pool_used[i])
                continue;
            skb_pool_used[i] = 1;
            memset(&skb_pool_meta[i], 0, sizeof(struct sk_buff));
            memset(skb_pool_mem[i], 0, size + SKB_MAX_EXTEND * 2);
            skb_pool_meta[i].total = size + SKB_MAX_EXTEND * 2;
            skb_pool_meta[i].mem = skb_pool_mem[i];
            skb_pool_meta[i].data = skb_pool_meta[i].mem + SKB_MAX_EXTEND;
            skb_pool_meta[i].pooled = 1;
            return &skb_pool_meta[i];
        }
    }

    struct sk_buff *skb = malloc(sizeof(struct sk_buff));
    if(!skb)
        return NULL;

    skb->len = 0;
    skb->total =  size + SKB_MAX_EXTEND * 2;
    skb->mem = calloc(1, skb->total);
    if(!skb->mem){
        free(skb);
        return NULL;
    }
    skb->data = skb->mem + SKB_MAX_EXTEND;
    skb->pooled = 0;
    return skb;
}

void *skb_put(struct sk_buff* skb, uint32_t size){
    if(!skb)
        return NULL;
    void* ret = skb->data;
    skb->len += size;
    if(skb->data + skb->len > skb->mem + skb->total){
        brcm_log("reach skb buffer max extend:%d \n", SKB_MAX_EXTEND);
    }
    return ret;
}

void *skb_push(struct sk_buff* skb, uint32_t size){
    if(!skb)
        return NULL;
    void* ret = skb->data;
    skb->len += size;
    skb->data -= size;
    if(skb->data < skb->mem){
        brcm_log("reach skb buffer min extend:%d \n", SKB_MAX_EXTEND);
    }
    return ret;
}

void *skb_pull(struct sk_buff* skb, uint32_t size){
    if(!skb)
        return NULL;
    void* ret = skb->data;
    skb->len -= size;
    skb->data += size;
    if(skb->data > skb->mem + skb->total){
        brcm_log("reach skb buffer max extend:%d \n", SKB_MAX_EXTEND);
    }
    return ret;
}

void skb_reserve(struct sk_buff* skb, uint32_t size){
    if(!skb)
        return;
    skb->data += size;
    if(skb->data > skb->mem + skb->total){
        brcm_log("reach skb buffer max extend:%d \n", SKB_MAX_EXTEND);
    }
}

void *skb_trim(struct sk_buff* skb, uint32_t size){
    if(!skb && size >= skb->len)
        return NULL;
    skb->len = size;
    return skb->data;
}

void skb_free(struct sk_buff* skb){
    if(!skb)
        return;
    if (skb->pooled) {
        for (int i = 0; i < SKB_POOL_COUNT; i++) {
            if (skb == &skb_pool_meta[i]) {
                skb_pool_used[i] = 0;
                return;
            }
        }
        return;
    }
    if(skb->mem)
        free(skb->mem);
    free(skb);
}
