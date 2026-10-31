/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef Q8B_WCN_WPA_H
#define Q8B_WCN_WPA_H
#include <stdint.h>
#include <stddef.h>
int wcn_wpa_init(const char *ssid,const char *password,const uint8_t seed[32],
                 const uint8_t mac[6],const uint8_t bssid[6],
                 const uint8_t *rsn,unsigned rsn_len,const uint8_t selected_rsn[22]);
void wcn_wpa_receive(const uint8_t *packet,unsigned bytes);
void wcn_wpa_poll(void);
void wcn_wpa_tx_complete(unsigned cookie,unsigned success);
void wcn_wpa_key_complete(unsigned index,unsigned group,unsigned status);
void wcn_wpa_reset(void);
void wcn_wpa_forget(void);
const char *wcn_wpa_profile_ssid(void);
int wcn_wpa_restart(const uint8_t mac[6],const uint8_t bssid[6],const uint8_t *rsn,unsigned rsn_len,const uint8_t selected[22]);
int wcn_wpa_ready(void);
int wcn_wpa_failed(void);
int wcn_wpa_established(void);
int wcn_wpa_accept_rx(unsigned group,unsigned key,unsigned tid,uint64_t pn);
int wcn_wpa_accept_msdu(unsigned group,unsigned key,unsigned tid,uint64_t pn,unsigned amsdu,unsigned first,unsigned last,unsigned sequence,unsigned number);
void wcn_wpa_status(char *out,size_t size);
#endif
