/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef Q8B_WCN_WMI_H
#define Q8B_WCN_WMI_H
#include <stddef.h>
void wcn_wmi_reset(void);
void wcn_wmi_receive(const void *data, unsigned bytes);
void wcn_wmi_poll(void);
void wcn_wmi_status(char *out, size_t size);
int wcn_wmi_scan(void);
int wcn_wmi_scan_cancel(void);
void wcn_wmi_scan_results(char *out, size_t size);
int wcn_wmi_ready(void);
#endif
