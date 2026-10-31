/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef Q8B_WCN_DP_H
#define Q8B_WCN_DP_H
#include <stddef.h>
#include <stdint.h>
struct wcn_rx_security {
    uint64_t pn;
    unsigned valid,encrypted,group,key,tid,amsdu,first,last,sequence,number;
    uint8_t transmitter[6];
};
int wcn_dp_init(void);
int wcn_dp_ready(void);
int wcn_dp_can_transmit(void);
void wcn_dp_tx_batch_begin(void);
void wcn_dp_tx_batch_end(void);
int wcn_dp_transmit(const void *frame,unsigned bytes,unsigned lmac);
int wcn_dp_reorder(uint64_t queue,unsigned window,unsigned ssn);
void wcn_dp_poll(void);
void wcn_dp_release(void);
void wcn_dp_status(char *out, size_t size);
void wcn_dp_rings(char *out, size_t size);
#endif
