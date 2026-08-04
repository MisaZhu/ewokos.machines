#ifndef BCM2712_BOARD_H
#define BCM2712_BOARD_H

#include <stdint.h>

enum {
	PI5_UNKNOWN = 0,
	PI5_2G,
	PI5_4G,
	PI5_8G,
	PI5_16G,
	PI5_CM5,
	PI5_PI500
};

/* detect the board through the VPU mailbox (revision property) */
uint32_t bcm2712_board(void);

/* ARM memory size in bytes reported by the firmware, 0 on failure */
uint32_t bcm2712_mem_size(void);

/* UART reference clock in Hz reported by the firmware, 0 on failure */
uint32_t bcm2712_uart_clock(void);

#endif
