/*
 * mvm.c - mvm op-mode core: RX packet dispatcher, MAC address, the
 * post-alive init command flow (iwl_mvm_up) and the auxiliary station.
 *
 * Condensed from Linux iwlwifi mvm/fw.c, mvm/sta.c, mvm/scan.c and
 * iwl-nvm-parse.c. Only the commands needed for a single WPA2 STA
 * interface are issued; calibration/phy-db is skipped because our
 * targets ship unified ucode.
 */
#include "../iwlm.h"
#include "../utils/log.h"

#include "api/commands.h"
#include "api/alive.h"
#include "api/config.h"
#include "api/coex.h"
#include "api/nvm-reg.h"
#include "api/power.h"
#include "api/sta.h"
#include "api/mac.h"
#include "api/context.h"
#include "api/datapath.h"

#include <stdio.h>

#define WIDE_ID(grp, cmd) (((u16)(grp) << 8) | (cmd))

/* ---------------- MAC address (CSR strap/otp, iwl_set_hw_address_from_csr) */

#define CSR_MAC_ADDR_BASE   0x380   /* cfg->mac_addr_from_csr (22000.c) */
#define CSR_MAC_ADDR0_OTP   (CSR_MAC_ADDR_BASE + 0x00)
#define CSR_MAC_ADDR1_OTP   (CSR_MAC_ADDR_BASE + 0x04)
#define CSR_MAC_ADDR0_STRAP (CSR_MAC_ADDR_BASE + 0x08)
#define CSR_MAC_ADDR1_STRAP (CSR_MAC_ADDR_BASE + 0x0c)

static void flip_hw_address(u32 mac_addr0, u32 mac_addr1, u8 *dest)
{
    const u8 *hw = (const u8 *)&mac_addr0;    /* LE bytes of the reg */

    dest[0] = hw[3];
    dest[1] = hw[2];
    dest[2] = hw[1];
    dest[3] = hw[0];
    hw = (const u8 *)&mac_addr1;
    dest[4] = hw[1];
    dest[5] = hw[0];
}

static bool mac_valid(const u8 *m)
{
    static const u8 zero[6] = { 0 };
    return !(m[0] & 1) && memcmp(m, zero, 6);
}

static void read_mac_addr(struct iwlm_dev *d)
{
    flip_hw_address(iwlm_read32(d, CSR_MAC_ADDR0_STRAP),
                    iwlm_read32(d, CSR_MAC_ADDR1_STRAP), d->mac);
    if (mac_valid(d->mac))
        return;

    /* OEM didn't fuse a strap address: fall back to OTP */
    flip_hw_address(iwlm_read32(d, CSR_MAC_ADDR0_OTP),
                    iwlm_read32(d, CSR_MAC_ADDR1_OTP), d->mac);
}

/* ---------------- tx response (reclaim data queue entries) -------------- */

/*
 * TX_CMD response, new TX api layout (iwl_mvm_tx_resp):
 * frame_count u8 @0 ... tx_queue le16 @36, agg_tx_status le32[] @40,
 * followed by the SCD ssn le32.
 */
static void handle_tx_resp(struct iwlm_dev *d, const struct iwl_rx_packet *pkt,
                           u32 len)
{
    const u8 *p = pkt->data;
    u8 frame_count;
    u16 qid, ssn;

    if (len < 4 + 44)
        return;
    frame_count = p[0];
    if (len < 4 + 44 + 4 * frame_count)
        return;
    qid = get_unaligned_le16(p + 36);
    ssn = get_unaligned_le32(p + 40 + 4 * frame_count) & 0xffff;
    iwlm_trans_tx_reclaim(d, qid, ssn);
}

/* ---------------- alive notification ---------------- */

/*
 * UCODE_ALIVE_NTFY v5/v6 carry the platform sku_id used to match the
 * .pnvm payloads: {status,flags} + lmac_data[2] (48B each) + umac_data
 * (16B) + sku_id (12B). v6 appends the imr info after the sku.
 */
#define ALIVE_SKU_OFS (4 + 2 * 48 + 16)

static void handle_alive(struct iwlm_dev *d, const struct iwl_rx_packet *pkt,
                         u32 len)
{
    struct iwlm_trans *t = &d->trans;
    const u8 *p = pkt->data;

    if (len >= ALIVE_SKU_OFS + 12) {
        t->sku_id[0] = get_unaligned_le32(p + ALIVE_SKU_OFS);
        t->sku_id[1] = get_unaligned_le32(p + ALIVE_SKU_OFS + 4);
        t->sku_id[2] = get_unaligned_le32(p + ALIVE_SKU_OFS + 8);
        klog("iwlm: alive, sku %08x:%08x:%08x\n",
             t->sku_id[0], t->sku_id[1], t->sku_id[2]);
    } else {
        klog("iwlm: alive (no sku)\n");
    }
    t->alive = 1;
}

/* ---------------- RX dispatcher ---------------- */

void iwlm_mvm_rx_pkt(struct iwlm_dev *d, const struct iwl_rx_packet *pkt)
{
    struct iwlm_trans *t = &d->trans;
    u32 len = iwl_rx_packet_len(pkt);
    u8 grp = pkt->hdr.group_id;
    u8 cmd = pkt->hdr.cmd;
    u16 seq = le16_to_cpu(pkt->hdr.sequence);

    /* synchronous command response matching (single outstanding cmd) */
    if (!t->wait_rsp.done && t->wait_grp != 0xff &&
        grp == t->wait_grp && cmd == t->wait_cmd &&
        !(seq & SEQ_RX_FRAME) &&
        ((seq & SEQ_QUEUE_MASK) >> SEQ_QUEUE_SHIFT) == t->cmdq.id) {
        u32 plen = len > 4 ? len - 4 : 0;

        if (t->wait_rsp.buf) {
            if (plen > t->wait_rsp.len)
                plen = t->wait_rsp.len;
            memcpy(t->wait_rsp.buf, pkt->data, plen);
            t->wait_rsp.len = plen;
        }
        t->wait_rsp.status = 0;
        t->wait_rsp.done = 1;
        return;
    }

    switch (WIDE_ID(grp, cmd)) {
    case WIDE_ID(LEGACY_GROUP, UCODE_ALIVE_NTFY):
        handle_alive(d, pkt, len);
        break;
    case WIDE_ID(REGULATORY_AND_NVM_GROUP, PNVM_INIT_COMPLETE_NTFY):
        t->pnvm_done = 1;
        break;
    case WIDE_ID(LEGACY_GROUP, TX_CMD):
        handle_tx_resp(d, pkt, len);
        break;
    case WIDE_ID(LEGACY_GROUP, SCAN_COMPLETE_UMAC):
        d->scan_done = true;
        break;
    case WIDE_ID(LEGACY_GROUP, REPLY_RX_MPDU_CMD):
        if (len > 4)
            iwlm_mvm_rx_mpdu(d, pkt->data, len - 4);
        break;
    default:
        /* scan iteration complete, statistics, beacons, ... : not needed */
        break;
    }
}

/* ---------------- auxiliary station (old station api only) -------------- */

int iwlm_mvm_add_aux_sta(struct iwlm_dev *d)
{
    struct iwl_mvm_add_sta_cmd cmd;
    __le32 rsp_status;
    u32 rsp_len = sizeof(rsp_status);
    int ret;

    /* sta_id 0 is reserved for the AP; aux gets the first free one */
    d->aux_sta_id = 1;

    memset(&cmd, 0, sizeof(cmd));
    cmd.sta_id = d->aux_sta_id;
    /*
     * Old station api: aux station uses a mac id (MAC_INDEX_AUX), encoded
     * like any other station. (The raw-lmac-id form is for the new api,
     * which never adds an aux sta at all.)
     */
    cmd.mac_id_n_color = cpu_to_le32(FW_CMD_ID_AND_COLOR(MAC_INDEX_AUX, 0));
    if (iwlm_fw_has_api(d, IWL_UCODE_TLV_API_STA_TYPE))
        cmd.station_type = IWL_STA_AUX_ACTIVITY;
    cmd.tid_disable_tx = cpu_to_le16(0xffff);

    ret = iwlm_trans_send_cmd(d, LEGACY_GROUP, ADD_STA, 0, &cmd, sizeof(cmd),
                              &rsp_status, &rsp_len, IWLM_HCMD_TIMEOUT_MS);
    if (ret)
        return -1;
    if (rsp_len < sizeof(rsp_status) ||
        (le32_to_cpu(rsp_status) & IWL_ADD_STA_STATUS_MASK) != ADD_STA_SUCCESS)
        return -2;

    /* aux tx queue: tid IWL_MAX_TID_COUNT (8) = tid-agnostic */
    ret = iwlm_trans_txq_alloc_aux(d, BIT(d->aux_sta_id), 8, 32);
    if (ret < 0)
        return -3;
    return 0;
}

/* ---------------- post-alive init flow (iwl_mvm_up) --------------------- */

int iwlm_mvm_up(struct iwlm_dev *d)
{
    struct iwl_tx_ant_cfg_cmd ant;
    struct iwl_bt_coex_cmd bt;
    struct iwl_device_power_cmd pw;
    int ret;

    read_mac_addr(d);
    if (!mac_valid(d->mac)) {
        klog("iwlm: no valid MAC address\n");
        return -1;
    }
    klog("iwlm: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
         d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5]);

    /*
     * pnvm handshake: the alive notification carried the sku_id, now
     * hand the platform NVM payloads to the fw. Mandatory on AX210+
     * hardware before any other init command.
     */
    ret = iwlm_fw_pnvm_handshake(d);
    if (ret) {
        klog("iwlm: pnvm handshake failed %d\n", ret);
        return ret;
    }

    /* TX_ANT_CONFIGURATION_CMD: valid tx antennas before calibrations */
    ant.valid = cpu_to_le32(iwlm_valid_tx_ant(d));
    ret = iwlm_trans_send_cmd_pdu(d, LEGACY_GROUP, TX_ANT_CONFIGURATION_CMD,
                                  &ant, sizeof(ant));
    if (ret) {
        klog("iwlm: TX_ANT_CFG failed %d\n", ret);
        return ret;
    }

    /* BT coexistence: normal mode, high band retention (iwlwifi default) */
    bt.mode = cpu_to_le32(BT_COEX_NW);
    bt.enabled_modules = cpu_to_le32(BT_COEX_HIGH_BAND_RET);
    ret = iwlm_trans_send_cmd_pdu(d, LEGACY_GROUP, BT_CONFIG,
                                  &bt, sizeof(bt));
    if (ret) {
        klog("iwlm: BT_CONFIG failed %d\n", ret);
        return ret;
    }

    /* DQA enable (all modern firmware has it) */
    if (iwlm_fw_has_capa(d, IWL_UCODE_TLV_CAPA_DQA_SUPPORT)) {
        struct {
            __le32 cmd_queue;
        } __packed dqa = { 0 };

        ret = iwlm_trans_send_cmd_pdu(d, DATA_PATH_GROUP, DQA_ENABLE_CMD,
                                      &dqa, sizeof(dqa));
        if (ret) {
            klog("iwlm: DQA enable failed %d\n", ret);
            return ret;
        }
    }

    /* auxiliary station for scanning (not needed with new station api) */
    if (!iwlm_has_new_station_api(d)) {
        ret = iwlm_mvm_add_aux_sta(d);
        if (ret) {
            klog("iwlm: add aux sta failed %d\n", ret);
            return ret;
        }
    }

    /* device power: CAM (always awake) - flags 0 */
    memset(&pw, 0, sizeof(pw));
    ret = iwlm_trans_send_cmd_pdu(d, LEGACY_GROUP, POWER_TABLE_CMD,
                                  &pw, sizeof(pw));
    if (ret) {
        klog("iwlm: POWER_TABLE failed %d\n", ret);
        return ret;
    }

    /* scan configuration */
    if (iwlm_fw_has_capa(d, IWL_UCODE_TLV_CAPA_UMAC_SCAN)) {
        ret = iwlm_scan_config_send(d);
        if (ret) {
            klog("iwlm: SCAN_CFG failed %d\n", ret);
            return ret;
        }
    }

    d->state = IWLM_STATE_FW_LOADED;
    return 0;
}
