/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef Q8B_WCN_CE_H
#define Q8B_WCN_CE_H
#include <stddef.h>
int wcn_ce_init(void);
void wcn_ce_poll(void);
void wcn_ce_release(void); /* Controller DMA must already be disabled. */
void wcn_ce_status(char *out, size_t size);
int wcn_ce_send(unsigned pipe, unsigned endpoint, const void *packet, unsigned bytes);
#endif
