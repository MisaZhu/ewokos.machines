/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef Q8B_WCN_QMI_H
#define Q8B_WCN_QMI_H
#include <stddef.h>
#include <stdint.h>
void wcn_qmi_reset(void);
void wcn_qmi_release(void); /* Only after the endpoint DMA engine is stopped. */
void wcn_qmi_begin(void);
void wcn_qmi_receive(const void *packet, unsigned bytes);
void wcn_qmi_poll(void);
/* Last accepted handshake step or successful request, including local file IO. */
uint64_t wcn_qmi_progress_ms(void);
void wcn_qmi_status(char *out, size_t size);
#endif
