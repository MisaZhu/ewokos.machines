/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef Q8B_WCN_STA_H
#define Q8B_WCN_STA_H
#include <stdint.h>
#include <stddef.h>
struct wcn_rx_security;
struct wcn_bss;
int wcn_sta_associate(const char *ssid);
int wcn_sta_busy(void);
int wcn_sta_failed(void);
int wcn_sta_resume(void);
/* Scan-then-associate-with-WPA using a passphrase or 64-hex PMK. */
int wcn_sta_join(const char *ssid,const char *credential);
int wcn_sta_bss_supported(const struct wcn_bss *bss);
const struct wcn_bss *wcn_sta_bss(void);
const char *wcn_sta_ssid(void);
const char *wcn_sta_error(void);
int wcn_sta_eapol_send(const uint8_t *packet,unsigned bytes);
int wcn_sta_install_key(unsigned index,unsigned group,const uint8_t key[16],const uint8_t *rsc);
int wcn_sta_authorize(void);
int wcn_sta_link_ready(void);
int wcn_sta_network_send(const uint8_t *frame,unsigned bytes);
void wcn_sta_poll(void);
void wcn_sta_data(const uint8_t *frame,unsigned bytes,const struct wcn_rx_security *security);
void wcn_sta_release(void); /* Only after the endpoint DMA engine is stopped. */
void wcn_sta_status(char *out,size_t size);
void wcn_sta_event(unsigned event,unsigned tag,const uint8_t *data,unsigned bytes);
void wcn_sta_reorder_complete(unsigned cookie,unsigned status);
void wcn_sta_peer(unsigned vdev,const uint8_t *mac,unsigned peer,unsigned hw_peer);
void wcn_sta_management(const uint8_t *frame,unsigned bytes,unsigned mhz);
#endif
