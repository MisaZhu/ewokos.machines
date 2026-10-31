/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef Q8B_WCN_NET_H
#define Q8B_WCN_NET_H
#include <stddef.h>
#include <stdint.h>
void wcn_net_receive(const uint8_t *frame,unsigned bytes);
void wcn_net_reset(void);
int wcn_net_pending(void);
int wcn_net_writable(void);
int wcn_net_read(void *buf,unsigned bytes);
int wcn_net_read_batch(void *buf,unsigned bytes);
unsigned wcn_net_activity(void);
int wcn_net_write(const void *buf,unsigned bytes);
void wcn_net_status(char *out,size_t size);
#endif
