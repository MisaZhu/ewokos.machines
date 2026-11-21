#ifndef ORIN_BSP_SDMMC_H
#define ORIN_BSP_SDMMC_H

#include <stdint.h>

/*
 * Tegra234 SDMMC1 (microSD on the p3767 module), standard SDHCI + Tegra
 * vendor registers, polled PIO.  BPMP supplies clock/reset; card detect is
 * main GPIO G7 (active low).  Skips UHS/1.8V tuning -- runs the card at
 * legacy/high-speed 25MHz, which is enough to read/write a rootfs.
 */

int bsp_sdmmc_init(void);
int bsp_sdmmc_present(void);          /* 1 if a card is detected */
uint64_t bsp_sdmmc_capacity(void);    /* card size in 512B sectors, 0 if none */
int bsp_sdmmc_read_blocks(uint32_t sector, void* buf, uint32_t count);
int bsp_sdmmc_write_blocks(uint32_t sector, const void* buf, uint32_t count);

#endif