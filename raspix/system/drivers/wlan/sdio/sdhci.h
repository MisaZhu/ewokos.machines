#ifndef __SDHCI_H__
#define __SDHCI_H__

#include <stdint.h>
#include <types.h>
#include "mmc.h"


void sdhci_init(void);
void sdhci_set_base_clock(uint32_t hz);
uint32_t sdhci_get_max_clock(void);
int sdhci_set_ios(struct mmc *mmc);
int  sdhci_send_command(struct mmc_cmd *cmd, struct mmc_data *data);
void sdhci_enable_irq(int enable);
bool sdhci_host_allows_high_speed(void);
#endif
