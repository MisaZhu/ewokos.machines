/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef Q8B_WCN_WMI_H
#define Q8B_WCN_WMI_H
#include <stddef.h>
#include <stdint.h>
struct wcn_he_caps {
    unsigned present;
    uint8_t mac[6],phy[11],mcs[12],ppe[25];
};
struct wcn_bss {
    uint8_t bssid[6];
    char ssid[33];
    unsigned mhz, privacy;
    int rssi;
    unsigned capability, beacon_interval, rsn_len, rates_len;
    uint8_t rsn[258], rates[32];
    unsigned ht, vht, wmm;
    uint8_t ht_cap[26], ht_op[22], vht_cap[12], vht_op[5], wmm_ac[16];
    struct wcn_he_caps he;
    unsigned he_operation;
    uint8_t he_op[6];
};
int wcn_wmi_find_bss(const char *ssid, struct wcn_bss *bss);
int wcn_wmi_channel_power(unsigned mhz);
int wcn_wmi_channel_width_power(unsigned mhz,unsigned center,unsigned width);
unsigned wcn_wmi_chains(void);
unsigned wcn_wmi_vht_caps(void);
unsigned wcn_wmi_nss160(void);
const struct wcn_he_caps *wcn_wmi_he_caps(void);
int wcn_wmi_parse_he(const uint8_t *ies,unsigned bytes,struct wcn_he_caps *cap,uint8_t operation[6]);
unsigned wcn_wmi_bss_160_center(const struct wcn_bss *bss);
int wcn_wmi_station_width_power(const struct wcn_bss *bss,unsigned center,unsigned width);
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
