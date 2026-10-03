#ifndef BSP_NVME_H
#define BSP_NVME_H

#include <stdint.h>

/*
 * x86 用户态 NVMe HCD — PCI 探测(class 0108) + BAR0 映射 + 轮询队列 I/O。
 * API 与 raspi5 bsp_nvme 保持一致, nvmefsd 无需修改即可复用。
 */

int bsp_nvme_init(void);

int32_t bsp_nvme_read(uint64_t start_lba, void *buf, uint32_t count);
int32_t bsp_nvme_write(uint64_t start_lba, const void *buf, uint32_t count);
uint64_t bsp_nvme_get_block_count(void);
uint32_t bsp_nvme_get_block_size(void);

#endif
