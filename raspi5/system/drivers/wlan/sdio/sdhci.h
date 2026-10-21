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

/*
 * Event-driven CARD_INT path.
 *
 * sdhci_card_irq_arm() routes the sdio2 GIC SPI to a user-space handler
 * (same sys_interrupt_setup contract bsp_bt.c and v3d_g2d.c use). The
 * handler masks the CARD_INT signal, acks the latch, bumps an internal
 * sequence counter and fires a directed proc_wakeup_by() at the parked
 * worker. Masking in the handler is required by the SDHCI spec: the
 * controller re-latches CARD_INT while DAT1 stays low, so acking without
 * masking would storm the SPI line.
 *
 * sdhci_card_irq_wait() parks the caller on the delivery token for up to
 * slice_us, rearms the signal, and returns true if a delivery landed.
 * Every wait that elapses silently while INT_STATUS.CARD_INT is set
 * counts a miss; SDHCI_CARD_IRQ_MISS_MAX consecutive misses permanently
 * disable the IRQ path (wrong INTID / routing broken) and the caller
 * falls back to plain usleep polling. sdhci_card_irq_is_on() reports the
 * current state so the worker can pick the sleep primitive accordingly.
 */
void sdhci_card_irq_arm(void);
void sdhci_card_irq_off(const char* why);
bool sdhci_card_irq_is_on(void);
bool sdhci_card_irq_wait(uint32_t slice_us);

/*
 * Wake a worker parked in sdhci_card_irq_wait() immediately, without waiting
 * for the slice timeout or a genuine card interrupt. The park was added to
 * cut RX first-packet latency (DAT1 asserting fires the directed wake the
 * moment inbound data lands); TX had no equivalent, so a lone outbound frame
 * enqueued while the worker was idle-parked only reached the air on the next
 * slice timeout or when the peer transmitted. This gives TX the same
 * edge-driven wake. Best-effort: a no-op when the IRQ path is off or nobody
 * is parked.
 */
void sdhci_card_irq_kick(void);
#endif