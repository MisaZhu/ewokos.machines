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
/*
 * Cached Pi4 (BCM2711) detection. Every Pi4-only branch of the SDIO card-IRQ
 * scheme keys off this so the Pi3 register state and worker cadence stay
 * byte-for-byte the pre-optimisation driver (see the implementation comment).
 */
bool sdhci_is_pi4(void);
/*
 * MMIO-only probe of the card interrupt line: no SDIO command is issued and
 * no bus turnaround is paid. See the comment on the implementation.
 */
bool sdhci_card_irq_pending(void);
/*
 * Async SDIO CARD_INT (DAT1) parking. arm() registers a user-space handler on
 * the WLAN Arasan GIC SPI (Pi4 INTID 158; it returns without arming on Pi3,
 * which this build does not route) and leaves SIGNAL_ENABLE masked; wait()
 * unmasks and parks the worker on the delivery token only while DAT1 is quiet,
 * so the line is never enabled into an asserted state. off() retires the
 * handler and falls back to polling; is_on() reports whether the async path is
 * live. wait() returns true if a delivery woke the park.
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
bool sdhci_host_allows_high_speed(void);
#endif
