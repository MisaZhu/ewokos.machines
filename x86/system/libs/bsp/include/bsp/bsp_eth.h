#ifndef BSP_ETH_H
#define BSP_ETH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Polled RTL8139 (QEMU "-device rtl8139") Ethernet back end.
 *
 * Mirrors the role libarch_virt's virtio_net plays on machine.virt: a small
 * hardware-facing library the /dev/eth0 vdevice driver drives. No interrupts
 * are used -- the net driver's loop_step calls bsp_eth_poll() to reap the ring
 * exactly like the uhci host controller driver polls USB.
 */

/* Probe PCI, reset the chip and set up DMA rings. 0 on success, -1 if absent. */
int  bsp_eth_init(void);

/* Copy the 6-byte station address. 0 on success, -1 if not initialized. */
int  bsp_eth_read_mac(uint8_t mac[6]);

/* Drain the RX ring into the software frame queue and reap TX completions. */
void bsp_eth_poll(void);

/* Number of received frames buffered and ready to hand to netd. */
int  bsp_eth_pending_rx(void);

/* Non-zero when a TX descriptor is free to accept another frame. */
int  bsp_eth_can_write(void);

/* Pop one received frame into buf (up to size). Returns bytes, 0 if none. */
int  bsp_eth_read(void *buf, uint32_t size);

/* Queue one raw Ethernet frame for transmission. Returns size, 0 if busy. */
int  bsp_eth_write(const void *buf, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif
