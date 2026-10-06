/*
 * Minimal 802.11 definitions required by the iwlwifi firmware API headers
 * and by the MLME/data paths of this driver. Field layout matches
 * include/linux/ieee80211.h on little-endian machines.
 */
#ifndef __IWLW_COMPAT_IEEE80211_H__
#define __IWLW_COMPAT_IEEE80211_H__

#include <linux/types.h>

/* 802.11 frame control bits (little-endian bitfields, as Linux does) */
#define IEEE80211_FCTL_VERS            0x0003
#define IEEE80211_FCTL_FTYPE           0x000c
#define IEEE80211_FCTL_STYPE           0x00f0
#define IEEE80211_FCTL_TODS            0x0100
#define IEEE80211_FCTL_FROMDS          0x0200
#define IEEE80211_FCTL_MOREFRAGS       0x0400
#define IEEE80211_FCTL_RETRY           0x0800
#define IEEE80211_FCTL_PM              0x1000
#define IEEE80211_FCTL_MOREDATA        0x2000
#define IEEE80211_FCTL_PROTECTED       0x4000
#define IEEE80211_FCTL_ORDER           0x8000

#define IEEE80211_FTYPE_MGMT           0x0000
#define IEEE80211_FTYPE_CTL            0x0004
#define IEEE80211_FTYPE_DATA           0x0008

#define IEEE80211_STYPE_ASSOC_REQ      0x0000
#define IEEE80211_STYPE_ASSOC_RESP     0x0010
#define IEEE80211_STYPE_REASSOC_REQ    0x0020
#define IEEE80211_STYPE_REASSOC_RESP   0x0030
#define IEEE80211_STYPE_PROBE_REQ      0x0040
#define IEEE80211_STYPE_PROBE_RESP     0x0050
#define IEEE80211_STYPE_BEACON         0x0080
#define IEEE80211_STYPE_ATIM           0x0090
#define IEEE80211_STYPE_DISASSOC       0x00a0
#define IEEE80211_STYPE_AUTH           0x00b0
#define IEEE80211_STYPE_DEAUTH         0x00c0
#define IEEE80211_STYPE_ACTION         0x00d0

#define IEEE80211_STYPE_BACK_REQ       0x0080
#define IEEE80211_STYPE_BACK           0x0090

#define IEEE80211_STYPE_DATA           0x0000
#define IEEE80211_STYPE_QOS_DATA       0x0080
#define IEEE80211_STYPE_NULLFUNC       0x0040
#define IEEE80211_STYPE_QOS_NULLFUNC   0x00c0

#define IEEE80211_SCTL_FRAG            0x000F
#define IEEE80211_SCTL_SEQ             0xFFF0

struct ieee80211_hdr {
    __le16 frame_control;
    __le16 duration_id;
    u8 addr1[6];
    u8 addr2[6];
    u8 addr3[6];
    __le16 seq_ctrl;
} __packed;

struct ieee80211_hdr_3addr {
    __le16 frame_control;
    __le16 duration_id;
    u8 addr1[6];
    u8 addr2[6];
    u8 addr3[6];
    __le16 seq_ctrl;
} __packed;

struct ieee80211_qos_hdr {
    __le16 frame_control;
    __le16 duration_id;
    u8 addr1[6];
    u8 addr2[6];
    u8 addr3[6];
    __le16 seq_ctrl;
    __le16 qos_ctrl;
} __packed;

static inline int ieee80211_is_mgmt(__le16 fc)  { return (fc & IEEE80211_FCTL_FTYPE) == IEEE80211_FTYPE_MGMT; }
static inline int ieee80211_is_ctl(__le16 fc)   { return (fc & IEEE80211_FCTL_FTYPE) == IEEE80211_FTYPE_CTL; }
static inline int ieee80211_is_data(__le16 fc)  { return (fc & IEEE80211_FCTL_FTYPE) == IEEE80211_FTYPE_DATA; }
static inline int ieee80211_is_data_qos(__le16 fc) {
    return (fc & (IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_STYPE)) ==
           (IEEE80211_FTYPE_DATA | IEEE80211_STYPE_QOS_DATA);
}
static inline int ieee80211_is_auth(__le16 fc) {
    return (fc & (IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_STYPE)) ==
           (IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_AUTH);
}
static inline int ieee80211_is_deauth(__le16 fc) {
    return (fc & (IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_STYPE)) ==
           (IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_DEAUTH);
}
static inline int ieee80211_is_disassoc(__le16 fc) {
    return (fc & (IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_STYPE)) ==
           (IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_DISASSOC);
}
static inline int ieee80211_is_assoc_resp(__le16 fc) {
    return (fc & (IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_STYPE)) ==
           (IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_ASSOC_RESP);
}
static inline int ieee80211_is_probe_resp(__le16 fc) {
    return (fc & (IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_STYPE)) ==
           (IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_PROBE_RESP);
}
static inline int ieee80211_is_beacon(__le16 fc) {
    return (fc & (IEEE80211_FCTL_FTYPE | IEEE80211_FCTL_STYPE)) ==
           (IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_BEACON);
}
static inline int ieee80211_has_protected(__le16 fc) { return !!(fc & IEEE80211_FCTL_PROTECTED); }
static inline int ieee80211_has_a4(__le16 fc) {
    return (fc & (IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS)) ==
           (IEEE80211_FCTL_TODS | IEEE80211_FCTL_FROMDS);
}

/* management frame fixed fields */
struct ieee80211_mgmt_beacon {
    __le64 timestamp;
    __le16 beacon_int;
    __le16 capab_info;
    u8 variable[];
} __packed;

struct ieee80211_mgmt_auth {
    __le16 auth_alg;
    __le16 auth_transaction;
    __le16 status_code;
    u8 variable[];
} __packed;

struct ieee80211_mgmt_assoc_req {
    __le16 capab_info;
    __le16 listen_interval;
    u8 variable[];
} __packed;

struct ieee80211_mgmt_assoc_resp {
    __le16 capab_info;
    __le16 status_code;
    __le16 aid;
    u8 variable[];
} __packed;

/* information element ids used by the driver */
#define WLAN_EID_SSID            0
#define WLAN_EID_SUPP_RATES      1
#define WLAN_EID_DS_PARAMS       3
#define WLAN_EID_EXT_SUPP_RATES  50
#define WLAN_EID_RSN             48
#define WLAN_EID_VENDOR_SPECIFIC 221

#define IEEE80211_MAX_SSID_LEN   32

/* RSN / WPA2 constants */
#define RSN_AKM_PSK              2

#endif
