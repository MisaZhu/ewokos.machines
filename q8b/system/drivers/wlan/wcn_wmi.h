/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef Q8B_WCN_WMI_H
#define Q8B_WCN_WMI_H
#include <stddef.h>
#include <stdint.h>
struct wcn_bss {
    uint8_t bssid[6];
    char ssid[33];
    unsigned mhz, privacy;
    int rssi;
    unsigned capability, beacon_interval, rsn_len, rates_len;
    uint8_t rsn[258], rates[32];
    unsigned ht, vht, wmm;
    uint8_t ht_cap[26], ht_op[22], vht_cap[12], vht_op[5], wmm_ac[16];
};
int wcn_wmi_find_bss(const char *ssid, struct wcn_bss *bss);
int wcn_wmi_channel_power(unsigned mhz);
int wcn_wmi_channel_width_power(unsigned mhz,unsigned center,unsigned width);
unsigned wcn_wmi_chains(void);
int wcn_wmi_scan_finished(void);
void wcn_wmi_mac(uint8_t mac[6]);
void wcn_wmi_reset(void);
void wcn_wmi_receive(const void *data, unsigned bytes);
void wcn_wmi_poll(void);
void wcn_wmi_status(char *out, size_t size);
int wcn_wmi_scan(void);
int wcn_wmi_country(const char *alpha2);
int wcn_wmi_scan_cancel(void);
void wcn_wmi_scan_results(char *out, size_t size);
int wcn_wmi_ready(void);
int wcn_wmi_station_available(void);
#endif
