/*
 * GA10B (Orin Nano GPU) compute device, as seen by the machine bsp layer.
 * One device per process; ga10b_dev_open() runs the full secure bring-up
 * (ACR, PMU RTOS, FECS/GPCCS, golden context) and is idempotent.
 */
#ifndef GA10B_DEV_H
#define GA10B_DEV_H

#include <stdint.h>

typedef struct ga10b_module ga10b_module_t;

/* fw_dir NULL selects the default firmware directory.  0 on success. */
int ga10b_dev_open(const char* fw_dir);
int ga10b_dev_ready(void);

/* GPU VA of a page-aligned, physically contiguous buffer (0 on failure) */
uint64_t ga10b_dev_map(uint64_t phy, uint32_t size);
/* driver-owned GPU-visible buffer */
int ga10b_dev_alloc(uint32_t size, void** va, uint64_t* gpu_va);
/* The buffers above are cached by the GPU: hand them to the GPU after the
 * CPU wrote launch inputs, and back before the CPU reads launch outputs */
int ga10b_dev_sync_to_gpu(void);
int ga10b_dev_sync_to_cpu(void);

/* nvcc sm_87 cubin: load once, look kernels up by name, launch
 * synchronously.  args[i] points at kernel parameter i. */
ga10b_module_t* ga10b_dev_module(const void* cubin, uint32_t size);
int ga10b_module_kernel(const ga10b_module_t* m, const char* name);
int ga10b_dev_launch(ga10b_module_t* m, int kernel, const uint32_t grid[3],
                     const uint32_t block[3], void* const* args);

#endif
