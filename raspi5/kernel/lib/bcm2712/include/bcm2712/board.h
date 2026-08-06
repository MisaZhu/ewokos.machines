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

/*
 * Query the firmware's existing boot framebuffer via ALLOCATE_BUFFER
 * with 0 request bytes. Returns the ARM physical address (or 0 on
 * failure). *size receives the framebuffer size in bytes.
 */
uint64_t bcm2712_fb_query(uint32_t *size);

#endif
