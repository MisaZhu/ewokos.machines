#ifndef BSP_AHCI_H
#define BSP_AHCI_H

#include <stdint.h>

/*
 * x86 用户态 AHCI (SATA) HCD — PCI 探测(class 0106) + ABAR 映射 + 轮询块 I/O。
 * 初始化成功后用 bsp_ahci_* 接口供 atafsd 等块服务使用。
 */

int32_t bsp_ahci_init(void);
/* 注册为通用 sd 层后端 (ATA 探测失败时的回退路径) */
int32_t bsp_ahci_register_sd(void);
int32_t bsp_ahci_read(uint64_t start_lba, void *buf, uint32_t count);
int32_t bsp_ahci_write(uint64_t start_lba, const void *buf, uint32_t count);
int32_t bsp_ahci_flush(void);
uint64_t bsp_ahci_get_block_count(void);
uint32_t bsp_ahci_get_block_size(void);

#endif
