#ifndef BSP_SD_H
#define BSP_SD_H

int bsp_sd_init(void);
static inline int bsp_sd_flush(void) {
	return 0;
}

#endif
