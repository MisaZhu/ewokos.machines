/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QCOM8GEN3_WCN_HTT_H
#define QCOM8GEN3_WCN_HTT_H
#include <stddef.h>
void wcn_htt_receive(const void *data, unsigned bytes);
void wcn_htt_poll(void);
void wcn_htt_reset(void);
void wcn_htt_status(char *out, size_t size);
int wcn_htt_ready(void);
#endif
