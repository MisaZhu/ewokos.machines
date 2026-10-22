#ifndef Q8B_BSP_ETH_H
#define Q8B_BSP_ETH_H

#include <stdint.h>

int bsp_eth_init(void);
int bsp_eth_read_mac(uint8_t mac[6]);
void bsp_eth_poll(void);
int bsp_eth_pending_rx(void);
int bsp_eth_can_write(void);
int bsp_eth_read(void *buf, uint32_t size);
int bsp_eth_write(const void *buf, uint32_t size);
void bsp_eth_stats(char *buf, uint32_t size);

#endif
