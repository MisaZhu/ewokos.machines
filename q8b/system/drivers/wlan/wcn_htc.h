/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef Q8B_WCN_HTC_H
#define Q8B_WCN_HTC_H
#include <stddef.h>
void wcn_htc_receive(unsigned pipe, const void *data, unsigned bytes);
void wcn_htc_poll(void);
void wcn_htc_reset(void);
void wcn_htc_status(char *out, size_t size);
int wcn_htc_wmi_send(const void *data, unsigned bytes);
int wcn_htc_htt_send(const void *data, unsigned bytes);
#endif
