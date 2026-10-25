#ifndef Q8B_QAIRT_DSP_RPC_H
#define Q8B_QAIRT_DSP_RPC_H
#include <stddef.h>
#include <stdint.h>
typedef union {
    struct { void *data; uint32_t size; } buf;
    uint64_t handle;
} remote_arg;
#ifdef __hexagon__
_Static_assert(sizeof(remote_arg) == 8, "Hexagon remote_arg ABI");
#endif
int q8b_model_open(uint64_t *handle);
int q8b_model_close(uint64_t handle);
int q8b_model_invoke(uint64_t handle, uint32_t sc, remote_arg *args);
#endif
