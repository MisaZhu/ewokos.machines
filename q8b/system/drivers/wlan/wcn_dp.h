/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef Q8B_WCN_DP_H
#define Q8B_WCN_DP_H
#include <stddef.h>
#include <stdint.h>
struct wcn_rx_security {
    uint64_t pn;
    unsigned valid,encrypted,group,key,tid;
    uint8_t transmitter[6];
};
int wcn_dp_init(void);
int wcn_dp_ready(void);
int wcn_dp_can_transmit(void);
int wcn_dp_transmit(const void *frame,unsigned bytes,unsigned lmac);
void wcn_dp_poll(void);
void wcn_dp_release(void);
void wcn_dp_status(char *out, size_t size);
void wcn_dp_rings(char *out, size_t size);
#endif
