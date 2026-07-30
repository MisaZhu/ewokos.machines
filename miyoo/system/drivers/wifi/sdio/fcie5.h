/*
 * fcie5.h - user-space SD/SDIO host driver for the SigmaStar SSD202D
 *           FCIE5 IP (wired to the RTL8723CS wifi module).
 *
 * Data transfers run in R2N (CIFD PIO) mode, so no DMA buffer is needed.
 */
#ifndef __FCIE5_H__
#define __FCIE5_H__

#include <types.h>
#include "mmc.h"

/* bring the host IP up (reset + low init clock + 1bit bus) */
int      fcie5_init(void);

/* soft-reset the FCIE IP */
void     fcie5_reset(void);

/* select the closest clock source <= hz, returns the real clock */
uint32_t fcie5_set_clock(uint32_t hz);

/* 1 or 4 data lines */
void     fcie5_set_bus_width(int width);

/*
 * Send one SD command, optionally with a data phase described by "data"
 * (R2N PIO transfer). Response bytes are filled into cmd->response[].
 * Returns 0 or a negative errno.
 */
int      fcie5_send_command(struct mmc_cmd *cmd, struct mmc_data *data);

/* card-interrupt (DAT1) detection */
void     fcie5_enable_sdio_int(int on);
int      fcie5_sdio_int_pending(void);
void     fcie5_clear_sdio_int(void);

#endif /* __FCIE5_H__ */
