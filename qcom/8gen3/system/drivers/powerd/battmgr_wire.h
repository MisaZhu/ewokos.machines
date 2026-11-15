/*
 * battmgr_wire.h - PMIC GLINK battery-manager (battmgr) wire protocol for
 * the Robo & Kala TW220 (Qualcomm SC8280XP).
 *
 * Real battery capacity on this Windows-on-ARM laptop is owned by the ADSP
 * firmware battery manager, reached over PMIC GLINK - NOT from the PMIC
 * fuel-gauge registers (UEFI leaves the FG measurement engine halted, and
 * OS-side SPMI writes to FG/ADC wedge the bus).  This header is the
 * transport-independent protocol layer; the GLINK transport (ADSP PAS boot
 * + the "PMIC_RTR_ADSP_APPS" rpmsg channel) is a separate bring-up step.
 *
 * Constants and message layouts mirror the upstream Linux drivers:
 *   include/linux/soc/qcom/pmic_glink.h   (owner ids, hdr, req/notify types)
 *   drivers/power/supply/qcom_battmgr.c   (opcodes, property ids, requests)
 *   drivers/soc/qcom/pmic_glink.c         (PMIC_RTR_ADSP_APPS, charger_pd)
 *
 * All multi-byte fields are little-endian on the wire.  The helpers here
 * encode/decode byte buffers so they can be unit-tested on the host without
 * any hardware access.
 */
#ifndef BATTMGR_WIRE_H
#define BATTMGR_WIRE_H

#include <stddef.h>
#include <stdint.h>

/* pmic_glink.h: client owner ids and message types. */
#define PMIC_GLINK_OWNER_BATTMGR   32778U  /* 0x800a */
#define PMIC_GLINK_OWNER_USBC      32779U
#define PMIC_GLINK_REQ_RESP        1U
#define PMIC_GLINK_NOTIFY          2U

/* GLINK channel / service names (pmic_glink.c).  The battery service runs
 * in the ADSP "charger_pd" protection domain, tracked via PDR/servreg. */
#define PMIC_GLINK_CHANNEL_ADSP    "PMIC_RTR_ADSP_APPS"
#define BATTMGR_PDR_SERVICE        "tms/servreg"
#define BATTMGR_PDR_PATH           "msm/adsp/charger_pd"

/* battmgr opcodes (qcom_battmgr.c). */
#define BATTMGR_BAT_STATUS         0x01U
#define BATTMGR_REQUEST_NOTIFICATION 0x04U
#define BATTMGR_NOTIFICATION       0x07U
#define BATTMGR_BAT_INFO           0x09U
#define BATTMGR_BAT_DISCHARGE_TIME 0x0cU
#define BATTMGR_BAT_CHARGE_TIME    0x0dU
#define BATTMGR_BAT_PROPERTY_GET   0x30U
#define BATTMGR_BAT_PROPERTY_SET   0x31U

/* Notification sub-opcodes. */
#define BATTMGR_NOTIF_BAT_PROPERTY 0x30U
#define BATTMGR_NOTIF_USB_PROPERTY 0x32U
#define BATTMGR_NOTIF_WLS_PROPERTY 0x34U
#define BATTMGR_NOTIF_BAT_STATUS   0x80U
#define BATTMGR_NOTIF_BAT_INFO     0x81U
#define BATTMGR_NOTIF_BAT_CHARGING_STATE 0x83U

/* Battery property ids (qcom_battmgr.c).  BATT_CAPACITY is the real SOC. */
#define BATT_PROP_STATUS           0U
#define BATT_PROP_HEALTH           1U
#define BATT_PROP_PRESENT          2U
#define BATT_PROP_CHG_TYPE         3U
#define BATT_PROP_CAPACITY         4U   /* state of charge, percent */
#define BATT_PROP_SOH              5U
#define BATT_PROP_VOLT_OCV         6U
#define BATT_PROP_VOLT_NOW         7U   /* millivolts */
#define BATT_PROP_VOLT_MAX         8U
#define BATT_PROP_CURR_NOW         9U   /* microamps */
#define BATT_PROP_TEMP             12U  /* deci-degrees Celsius */
#define BATT_PROP_TECHNOLOGY       13U
#define BATT_PROP_CYCLE_COUNT      15U

/* On-wire header: three little-endian u32 (owner, type, opcode). */
#define BATTMGR_HDR_SIZE           12U

/* property request/response: hdr + battery + property + value (6 x u32). */
#define BATTMGR_PROPERTY_MSG_SIZE  24U
/* enable/notification request: hdr + battery_id + power_state +
 * low_capacity + high_capacity (7 x u32). */
#define BATTMGR_ENABLE_MSG_SIZE    28U

static inline void battmgr_put_le32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffU);
    p[1] = (uint8_t)((v >> 8) & 0xffU);
    p[2] = (uint8_t)((v >> 16) & 0xffU);
    p[3] = (uint8_t)((v >> 24) & 0xffU);
}

static inline uint32_t battmgr_get_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
            ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Encode a BATTMGR_BAT_PROPERTY_GET request for battery 0 into buf (must
 * hold >= BATTMGR_PROPERTY_MSG_SIZE bytes).  Returns the message length. */
static inline size_t battmgr_encode_property_get(uint8_t* buf,
        uint32_t property) {
    battmgr_put_le32(buf + 0, PMIC_GLINK_OWNER_BATTMGR);
    battmgr_put_le32(buf + 4, PMIC_GLINK_REQ_RESP);
    battmgr_put_le32(buf + 8, BATTMGR_BAT_PROPERTY_GET);
    battmgr_put_le32(buf + 12, 0U);          /* battery id */
    battmgr_put_le32(buf + 16, property);
    battmgr_put_le32(buf + 20, 0U);          /* value (unused on get) */
    return BATTMGR_PROPERTY_MSG_SIZE;
}

/* Encode the enable / request-notification message into buf (must hold
 * >= BATTMGR_ENABLE_MSG_SIZE bytes).  Returns the message length. */
static inline size_t battmgr_encode_enable(uint8_t* buf) {
    battmgr_put_le32(buf + 0, PMIC_GLINK_OWNER_BATTMGR);
    battmgr_put_le32(buf + 4, PMIC_GLINK_NOTIFY);
    battmgr_put_le32(buf + 8, BATTMGR_REQUEST_NOTIFICATION);
    battmgr_put_le32(buf + 12, 0U);          /* battery_id */
    battmgr_put_le32(buf + 16, 0U);          /* power_state */
    battmgr_put_le32(buf + 20, 0U);          /* low_capacity */
    battmgr_put_le32(buf + 24, 0U);          /* high_capacity */
    return BATTMGR_ENABLE_MSG_SIZE;
}

/* Decode a property response/notification.  Returns 0 on success and fills
 * *property and *value; -1 if the buffer is short or not a battmgr property
 * message.  The firmware echoes battery/property and fills value. */
static inline int battmgr_decode_property(const uint8_t* buf, size_t len,
        uint32_t* property, uint32_t* value) {
    if (buf == NULL || len < BATTMGR_PROPERTY_MSG_SIZE)
        return -1;
    if (battmgr_get_le32(buf + 0) != PMIC_GLINK_OWNER_BATTMGR)
        return -1;
    if (battmgr_get_le32(buf + 8) != BATTMGR_BAT_PROPERTY_GET &&
            battmgr_get_le32(buf + 8) != BATTMGR_NOTIF_BAT_PROPERTY)
        return -1;
    if (property != NULL)
        *property = battmgr_get_le32(buf + 16);
    if (value != NULL)
        *value = battmgr_get_le32(buf + 20);
    return 0;
}

#endif