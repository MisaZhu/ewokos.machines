#ifndef __SDHCI_H__
#define __SDHCI_H__

#include <stdint.h>
#include <stdbool.h>
#include <types.h>
#include "mmc.h"

/* host capability flags cached from SDHCI_CAPABILITIES(_1) */
#define MMC_CAP_UHS_DDR50   (1u << 0)

void sdhci_init(void);
void sdhci_set_base_clock(uint32_t hz);
int sdhci_set_ios(struct mmc *mmc);
int  sdhci_send_command(struct mmc_cmd *cmd, struct mmc_data *data);
void sdhci_enable_irq(int enable);
/*
 * MMIO-only probe of the card interrupt line: no SDIO command is issued and
 * no bus turnaround is paid. See the comment on the implementation.
 */
bool sdhci_card_irq_pending(void);
#endif