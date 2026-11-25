/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QCOM8GEN3_WCN_DP_H
#define QCOM8GEN3_WCN_DP_H
#include <stddef.h>
#include <stdint.h>
struct wcn_rx_security {
    uint64_t pn;
    unsigned valid,encrypted,group,key,tid,amsdu,first,last,sequence,number;
    uint8_t transmitter[6];
};
int wcn_dp_init(void);
int wcn_dp_ready(void);
/* A programmed ring is not a working ring: the firmware acknowledges the
 * RXDMA/REO configuration and then never writes a completion into it, which
 * leaves the control plane healthy (WMI management frames) with no data
 * frames at all. Only an observed completion proves RX is alive. */
int wcn_dp_rx_alive(void);
unsigned wcn_dp_rx_frames(void);
unsigned wcn_dp_completions(void);
unsigned wcn_dp_tx_errors(void);
unsigned wcn_dp_tx_packets(void);
/* "reo=.. rx_hp=.. refill=.." snapshot proving whether RXDMA advances. */
void wcn_dp_hw_state(char *out, unsigned size);
void wcn_dp_beat(char *out, size_t size);
/* "rxfill=<buffers the firmware wrote> rings=[..hp..]" - separates "frames
 * arrived, completion path lost them" from "no frame was ever received". */
void wcn_dp_rx_probe(char *out, unsigned size);
void wcn_dp_mac2_filter_req(int on);
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
