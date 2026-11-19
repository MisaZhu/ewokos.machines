/* PMIC GLINK battery-manager wire format for the SC8280XP ADSP.
 *
 * Reverse-engineered from the working mainline Linux driver on the TW220
 * (drivers/power/supply/qcom_battmgr.c, qcom_battmgr_sc8280xp_callback, and
 * drivers/rpmsg/qcom_glink_native.c).  Two facts matter:
 *
 *  1. battmgr rides the GLINK channel "PMIC_RTR_ADSP_APPS" as a plain rpmsg
 *     channel, but the glink transport does NOT frame payloads with the
 *     16-byte rpmsg_hdr.  __qcom_glink_send() puts the caller's buffer straight
 *     into the GLINK TX_DATA payload and qcom_glink_rx_data() hands the raw
 *     payload to the rpmsg callback (src/dst are RPMSG_ADDR_ANY and never go on
 *     the wire).  So the on-wire bytes are just the PMIC GLINK message itself,
 *     immediately after the GLINK data-frame header.  Adding an rpmsg_hdr here
 *     makes the ADSP read owner=0 and drop the request.
 *
 *  2. On SC8280XP the state of charge is NOT obtained with BAT_PROPERTY_GET
 *     (0x30, that is the sm8350 path).  The driver sends BAT_INFO (0x9) to learn
 *     last_full_capacity, then BAT_STATUS (0x1) for the live capacity, and
 *     computes percent = 100 * capacity / last_full_capacity (both in mWh).
 */
#ifndef Q8B_ADSP_GPR_H
#define Q8B_ADSP_GPR_H

#include <stddef.h>
#include <stdint.h>

#define Q8B_PMIC_GLINK_CHANNEL         "PMIC_RTR_ADSP_APPS"

/* PMIC GLINK message header (struct pmic_glink_hdr). */
#define Q8B_BATTMGR_OWNER              0x0000800aU /* PMIC_GLINK_OWNER_BATTMGR */
#define Q8B_BATTMGR_REQ_RESP           1U
#define Q8B_BATTMGR_NOTIFY             2U

#define Q8B_BATTMGR_BAT_STATUS         0x01U
#define Q8B_BATTMGR_BAT_INFO           0x09U

/* struct qcom_battmgr_update_request: hdr(12) + battery_id(4). */
#define Q8B_BATTMGR_UPDATE_REQ_SIZE    16U
/* BAT_STATUS response: hdr(12) + status(7 * u32 = 28). */
#define Q8B_BATTMGR_STATUS_MSG_SIZE    40U
/* BAT_INFO response: we only need up to last_full_capacity (offset 20 + 4). */
#define Q8B_BATTMGR_INFO_MIN_SIZE      24U

/* status.battery_state bits. */
#define Q8B_BATT_STATE_DISCHARGING     (1U << 0)
#define Q8B_BATT_STATE_CHARGING        (1U << 1)
#define Q8B_BATT_STATE_CRITICAL        (1U << 2)

static inline void q8b_battmgr_put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xffU);
    p[1] = (uint8_t)((v >> 8) & 0xffU);
    p[2] = (uint8_t)((v >> 16) & 0xffU);
    p[3] = (uint8_t)((v >> 24) & 0xffU);
}

static inline uint32_t q8b_battmgr_get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void q8b_battmgr_put_hdr(uint8_t *p, uint32_t type,
        uint32_t opcode) {
    q8b_battmgr_put_le32(p + 0, Q8B_BATTMGR_OWNER);
    q8b_battmgr_put_le32(p + 4, type);
    q8b_battmgr_put_le32(p + 8, opcode);
}

/* Build a BAT_INFO / BAT_STATUS request (hdr + battery_id=0).  pkt must have
 * room for Q8B_BATTMGR_UPDATE_REQ_SIZE bytes. */
static inline size_t q8b_battmgr_build_update_req(uint8_t *pkt,
        uint32_t opcode) {
    q8b_battmgr_put_hdr(pkt, Q8B_BATTMGR_REQ_RESP, opcode);
    q8b_battmgr_put_le32(pkt + 12, 0U); /* battery_id */
    return Q8B_BATTMGR_UPDATE_REQ_SIZE;
}

/* Parse a BAT_INFO response.  info layout after the 12-byte hdr:
 *   power_unit@12, design_capacity@16, last_full_capacity@20 (mWh).
 * Returns 0 and fills the capacities on a matching response, -1 otherwise. */
static inline int q8b_battmgr_parse_info(const uint8_t *pkt, size_t len,
        uint32_t *last_full, uint32_t *design) {
    if (pkt == NULL || len < Q8B_BATTMGR_INFO_MIN_SIZE)
        return -1;
    if (q8b_battmgr_get_le32(pkt + 0) != Q8B_BATTMGR_OWNER)
        return -1;
    if (q8b_battmgr_get_le32(pkt + 8) != Q8B_BATTMGR_BAT_INFO)
        return -1;
    if (design)
        *design = q8b_battmgr_get_le32(pkt + 16);
    if (last_full)
        *last_full = q8b_battmgr_get_le32(pkt + 20);
    return 0;
}

/* Parse a BAT_STATUS response.  status layout after the 12-byte hdr:
 *   battery_state@12, capacity@16 (mWh), rate@20, battery_voltage@24 (mV),
 *   power_state@28, charging_source@32, temperature@36.
 * Returns 0 on a matching response, -1 otherwise.  Any out pointer may be NULL. */
static inline int q8b_battmgr_parse_status(const uint8_t *pkt, size_t len,
        uint32_t *capacity, uint32_t *state, uint32_t *source,
        uint32_t *voltage) {
    if (pkt == NULL || len < Q8B_BATTMGR_STATUS_MSG_SIZE)
        return -1;
    if (q8b_battmgr_get_le32(pkt + 0) != Q8B_BATTMGR_OWNER)
        return -1;
    if (q8b_battmgr_get_le32(pkt + 8) != Q8B_BATTMGR_BAT_STATUS)
        return -1;
    if (state)
        *state = q8b_battmgr_get_le32(pkt + 12);
    if (capacity)
        *capacity = q8b_battmgr_get_le32(pkt + 16);
    if (voltage)
        *voltage = q8b_battmgr_get_le32(pkt + 24);
    if (source)
        *source = q8b_battmgr_get_le32(pkt + 32);
    return 0;
}

#endif