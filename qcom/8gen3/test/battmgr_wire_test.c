/*
 * battmgr_wire host test - validates the PMIC GLINK battmgr wire layer
 * (powerd/battmgr_wire.h) against the layouts in Linux pmic_glink.h and
 * qcom_battmgr.c.  Build and run on the host: no hardware, no EwokOS header.
 *
 *   cc -std=c99 -Wall -Wextra -o battmgr_wire_test battmgr_wire_test.c && \
 *   ./battmgr_wire_test
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../system/drivers/powerd/battmgr_wire.h"

static int failures;

static void check(int cond, const char* what) {
    if (!cond) {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

int main(void) {
    uint8_t req[BATTMGR_ENABLE_MSG_SIZE];
    uint8_t reply[BATTMGR_PROPERTY_MSG_SIZE];
    uint32_t prop = 0, value = 0;
    size_t len;

    /* --- property get request --- */
    len = battmgr_encode_property_get(req, BATT_PROP_CAPACITY);
    check(len == 24, "property get length is 24");
    check(battmgr_get_le32(req + 0) == PMIC_GLINK_OWNER_BATTMGR,
            "owner is 0x800a");
    check(PMIC_GLINK_OWNER_BATTMGR == 0x800aU, "owner constant matches Linux");
    check(battmgr_get_le32(req + 4) == PMIC_GLINK_REQ_RESP,
            "type is req/resp");
    check(battmgr_get_le32(req + 8) == BATTMGR_BAT_PROPERTY_GET,
            "opcode is 0x30");
    check(battmgr_get_le32(req + 12) == 0, "battery id is 0");
    check(battmgr_get_le32(req + 16) == BATT_PROP_CAPACITY, "property is capacity");
    check(battmgr_get_le32(req + 20) == 0, "get carries no value");
    check(BATTMGR_BAT_PROPERTY_GET == 0x30U, "opcode constant matches Linux");

    /* --- enable / request-notification --- */
    len = battmgr_encode_enable(req);
    check(len == 28, "enable length is 28");
    check(battmgr_get_le32(req + 4) == PMIC_GLINK_NOTIFY, "enable type is notify");
    check(battmgr_get_le32(req + 8) == BATTMGR_REQUEST_NOTIFICATION,
            "enable opcode is 0x04");
    check(battmgr_get_le32(req + 12) == 0, "enable battery id 0");
    check(battmgr_get_le32(req + 16) == 0, "power_state 0");
    check(battmgr_get_le32(req + 20) == 0, "low_capacity 0");
    check(battmgr_get_le32(req + 24) == 0, "high_capacity 0");

    /* --- property reply decode, capacity is percent * 100 on the wire --- */
    memset(reply, 0, sizeof(reply));
    battmgr_put_le32(reply + 0, PMIC_GLINK_OWNER_BATTMGR);
    battmgr_put_le32(reply + 4, PMIC_GLINK_REQ_RESP);
    battmgr_put_le32(reply + 8, BATTMGR_BAT_PROPERTY_GET);
    battmgr_put_le32(reply + 12, 0);
    battmgr_put_le32(reply + 16, BATT_PROP_CAPACITY);
    battmgr_put_le32(reply + 20, 9800);      /* 98.00 % */
    check(battmgr_decode_property(reply, sizeof(reply), &prop, &value) == 0,
            "decode capacity reply");
    check(prop == BATT_PROP_CAPACITY, "decoded property id");
    check(value == 9800, "decoded raw value");
    check(value / 100 == 98, "raw percent*100 scales to 98%");

    /* --- SOH is already a percentage --- */
    battmgr_put_le32(reply + 16, BATT_PROP_SOH);
    battmgr_put_le32(reply + 20, 91);
    check(battmgr_decode_property(reply, sizeof(reply), &prop, &value) == 0,
            "decode SOH reply");
    check(prop == BATT_PROP_SOH && value == 91, "SOH is a percentage");

    /* --- notification sub-opcode is accepted too --- */
    battmgr_put_le32(reply + 8, BATTMGR_NOTIF_BAT_PROPERTY);
    battmgr_put_le32(reply + 16, BATT_PROP_TEMP);
    battmgr_put_le32(reply + 20, 305);       /* 30.5 C */
    check(battmgr_decode_property(reply, sizeof(reply), &prop, &value) == 0,
            "decode notification");
    check(prop == BATT_PROP_TEMP && value == 305, "temp is deci-degrees");

    /* --- rejections --- */
    check(battmgr_decode_property(reply, 16, &prop, &value) != 0,
            "short buffer rejected");
    battmgr_put_le32(reply + 0, 0x1234);
    check(battmgr_decode_property(reply, sizeof(reply), &prop, &value) != 0,
            "foreign owner rejected");
    battmgr_put_le32(reply + 0, PMIC_GLINK_OWNER_BATTMGR);
    battmgr_put_le32(reply + 8, 0x99);
    check(battmgr_decode_property(reply, sizeof(reply), &prop, &value) != 0,
            "foreign opcode rejected");

    /* --- property ids used by the client, verified against Linux --- */
    check(BATT_PROP_STATUS == 0 && BATT_PROP_HEALTH == 1 && BATT_PROP_CAPACITY == 4,
            "core property ids");
    check(BATT_PROP_SOH == 5 && BATT_PROP_VOLT_NOW == 7 && BATT_PROP_TEMP == 12,
            "measurement property ids");
    check(BATT_PROP_CYCLE_COUNT == 15 && BATT_PROP_CHG_FULL_DESIGN == 16 &&
            BATT_PROP_CHG_FULL == 17, "capacity/counter property ids");

    if (failures == 0)
        printf("battmgr_wire_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
