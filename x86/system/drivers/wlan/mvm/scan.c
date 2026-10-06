/*
 * scan.c - UMAC scan: SCAN_CFG (reduced) sent at mvm_up, SCAN_REQ_UMAC
 * (v14+ "v17" layout) for one-shot scans and collection of scan results
 * from received beacons / probe responses.
 * Condensed from Linux iwlwifi mvm/scan.c (v6.11).
 */
#include "../iwlm.h"
#include "../utils/log.h"
#include "../iwl/api/commands.h"
#include "../iwl/api/scan.h"
#include "../iwl/api/phy-ctxt.h"

#include <string.h>

/* dwell/timing values from Linux mvm/scan.c */
#define SCAN_DWELL_ACTIVE       10
#define SCAN_DWELL_PASSIVE      110
#define ADWELL_LB_N_APS         2
#define ADWELL_HB_N_APS         8
#define ADWELL_N_APS_SOCIAL     10
#define ADWELL_MAX_BUDGET_FULL  300
#define ADWELL_OV_GO_FRIENDLY   10
#define ADWELL_OV_SOCIAL        2

/* our channel set: 2.4G 1-13 + 5G non-DFS UNII-1/UNII-3 */
static const u8 chans_24[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
static const u8 chans_5[]  = { 36, 40, 44, 48, 149, 153, 157, 161, 165 };

/* ---------------- SCAN_CFG (reduced) ---------------- */

int iwlm_scan_config_send(struct iwlm_dev *d)
{
    struct iwl_scan_config cfg;

    if (!iwlm_fw_has_api(d, IWL_UCODE_TLV_API_REDUCED_SCAN_CONFIG)) {
        klog("iwlm: no REDUCED_SCAN_CONFIG api, SCAN_CFG skipped\n");
        return 0;
    }

    memset(&cfg, 0, sizeof(cfg));
    if (!iwlm_has_new_station_api(d))
        cfg.bcast_sta_id = d->aux_sta_id;
    else if (iwlm_fw_cmd_ver(&d->fw, 0, SCAN_CFG_CMD) < 5)
        cfg.bcast_sta_id = 0xff;
    cfg.tx_chains = cpu_to_le32(iwlm_valid_tx_ant(d));
    cfg.rx_chains = cpu_to_le32(iwlm_valid_rx_ant(d));

    return iwlm_trans_send_cmd_pdu(d, 0, SCAN_CFG_CMD, &cfg, sizeof(cfg));
}

/* ---------------- probe request template ---------------- */

static const u8 rates_24[] = { 1, 8, { 0x82, 0x84, 0x8b, 0x96,
                                       0x0c, 0x12, 0x18, 0x24 } };
static const u8 rates_5[]  = { 1, 8, { 0x8c, 0x12, 0x98, 0x24,
                                       0xb0, 0x48, 0x60, 0x6c } };
static const u8 rates_ext[] = { 50, 4, { 0x30, 0x48, 0x60, 0x6c } };

static void build_probe_req(struct iwlm_dev *d, struct iwl_scan_probe_req *preq)
{
    u8 *buf = preq->buf, *pos;

    memset(preq, 0, sizeof(*preq));

    /* 802.11 header: probe request, broadcast, wildcard SSID IE */
    put_unaligned_le16(0x0040, buf);            /* fc: probe req */
    put_unaligned_le16(0, buf + 2);             /* duration */
    memset(buf + 4, 0xff, 6);                   /* da */
    memcpy(buf + 10, d->mac, 6);                /* sa */
    memset(buf + 16, 0xff, 6);                  /* bssid */
    put_unaligned_le16(0, buf + 22);            /* seq */
    buf[24] = 0;                                /* SSID ie, wildcard */
    buf[25] = 0;
    preq->mac_header.offset = 0;
    preq->mac_header.len = cpu_to_le16(26);

    pos = buf + 26;
    memcpy(pos, rates_24, sizeof(rates_24));
    preq->band_data[0].offset = cpu_to_le16(pos - buf);
    preq->band_data[0].len = cpu_to_le16(sizeof(rates_24));
    pos += sizeof(rates_24);

    memcpy(pos, rates_5, sizeof(rates_5));
    preq->band_data[1].offset = cpu_to_le16(pos - buf);
    preq->band_data[1].len = cpu_to_le16(sizeof(rates_5));
    pos += sizeof(rates_5);

    preq->band_data[2].offset = cpu_to_le16(pos - buf);
    preq->band_data[2].len = 0;

    memcpy(pos, rates_ext, sizeof(rates_ext));
    preq->common_data.offset = cpu_to_le16(pos - buf);
    preq->common_data.len = cpu_to_le16(sizeof(rates_ext));
}

/* ---------------- SCAN_REQ_UMAC (v14+) ---------------- */

int iwlm_mvm_scan_start(struct iwlm_dev *d)
{
    static struct iwl_scan_req_umac_v17 req;    /* ~1.9KB, single worker */
    struct iwl_scan_general_params_v11 *gp = &req.scan_params.general_params;
    struct iwl_scan_channel_params_v7 *cp = &req.scan_params.channel_params;
    struct iwl_scan_periodic_parms_v1 *pp = &req.scan_params.periodic_params;
    struct iwl_scan_probe_params_v4 *pb = &req.scan_params.probe_params;
    u8 ver = iwlm_fw_cmd_ver(&d->fw, 0, SCAN_REQ_UMAC);
    u32 i, n = 0;
    int ret;

    if (ver < 14) {
        klog("iwlm: scan: fw cmd ver %u unsupported (need >= 14)\n", ver);
        return -1;
    }

    d->n_aps = 0;
    d->scan_done = false;

    memset(&req, 0, sizeof(req));
    req.uid = 0;                                /* single regular scan */
    req.ooc_priority = cpu_to_le32(IWL_SCAN_PRIORITY_EXT_6);

    /* general params (v11 layout, dwell_v11, unassociated timing) */
    gp->flags = cpu_to_le16(IWL_UMAC_SCAN_GEN_FLAGS_V2_MATCH |
                            IWL_UMAC_SCAN_GEN_FLAGS_V2_ADAPTIVE_DWELL);
    gp->scan_start_mac_or_link_id = 0;
    gp->active_dwell[0] = SCAN_DWELL_ACTIVE;
    gp->active_dwell[1] = SCAN_DWELL_ACTIVE;
    gp->passive_dwell[0] = SCAN_DWELL_PASSIVE;
    gp->passive_dwell[1] = SCAN_DWELL_PASSIVE;
    gp->adwell_default_2g = ADWELL_LB_N_APS;
    gp->adwell_default_5g = ADWELL_HB_N_APS;
    gp->adwell_default_social_chn = ADWELL_N_APS_SOCIAL;
    gp->adwell_max_budget = cpu_to_le16(ADWELL_MAX_BUDGET_FULL);
    gp->scan_priority = cpu_to_le32(IWL_SCAN_PRIORITY_EXT_6);
    /* max_out_of_time/suspend_time stay 0 (UNASSOC), flags2 = 0 */

    /* one-shot scan: single plan, single iteration */
    pp->schedule[0].interval = 0;
    pp->schedule[0].iter_count = 1;
    pp->delay = 0;

    /* channel params (v7): v17 stores the band in flags bits 30-31 */
    cp->flags = IWL_SCAN_CHANNEL_FLAG_EBS |
                IWL_SCAN_CHANNEL_FLAG_EBS_ACCURATE |
                IWL_SCAN_CHANNEL_FLAG_CACHE_ADD |
                IWL_SCAN_CHANNEL_FLAG_ENABLE_CHAN_ORDER;
    cp->n_aps_override[0] = ADWELL_OV_GO_FRIENDLY;
    cp->n_aps_override[1] = ADWELL_OV_SOCIAL;
    for (i = 0; i < sizeof(chans_24); i++, n++) {
        struct iwl_scan_channel_cfg_umac *c = &cp->channel_config[n];
        c->flags = cpu_to_le32(PHY_BAND_24 << IWL_CHAN_CFG_FLAGS_BAND_POS);
        c->v2.channel_num = chans_24[i];
        c->v2.iter_count = 1;
        c->v2.iter_interval = 0;
    }
    for (i = 0; i < sizeof(chans_5); i++, n++) {
        struct iwl_scan_channel_cfg_umac *c = &cp->channel_config[n];
        c->flags = cpu_to_le32(PHY_BAND_5 << IWL_CHAN_CFG_FLAGS_BAND_POS);
        c->v2.channel_num = chans_5[i];
        c->v2.iter_count = 1;
        c->v2.iter_interval = 0;
    }
    cp->count = n;

    /* probe params (v4): wildcard, no directed ssids */
    build_probe_req(d, &pb->preq);

    ret = iwlm_trans_send_cmd_pdu(d, 0, SCAN_REQ_UMAC, &req, sizeof(req));
    if (ret) {
        klog("iwlm: SCAN_REQ_UMAC failed (%d)\n", ret);
        return ret;
    }
    d->state = IWLM_STATE_SCANNING;
    return 0;
}

/* ---------------- scan results ---------------- */

static void parse_rsn(struct iwlm_ap_info *ap, const u8 *ie, u32 len)
{
    const u8 *p, *end;
    u16 n;

    if (len < 8)
        return;
    p = ie + 2;                         /* skip version */
    end = ie + len;
    if (p + 4 > end)
        return;
    p += 4;                             /* group cipher */
    if (p + 2 > end)
        return;
    n = get_unaligned_le16(p); p += 2;  /* pairwise count */
    if (p + 4 * n > end || n == 0)
        return;
    ap->has_rsn = true;
    ap->rsn_pairwise = p[3];            /* first pairwise cipher type */
    p += 4 * n;
    if (p + 2 > end)
        return;
    n = get_unaligned_le16(p); p += 2;  /* akm count */
    if (p + 4 * n > end || n == 0)
        return;
    ap->rsn_akm = p[3];                 /* first akm type (2 = PSK) */
}

void iwlm_scan_rx_frame(struct iwlm_dev *d, const u8 *frame, u32 len, s8 rssi)
{
    struct iwlm_ap_info *ap;
    const u8 *bssid, *ie, *end;
    u16 fc;
    int i;

    if (len < 36)
        return;
    fc = get_unaligned_le16(frame);
    if ((fc & 0x000c) != 0)                     /* must be mgmt */
        return;
    if ((fc & 0x00f0) != 0x0080 &&              /* beacon */
        (fc & 0x00f0) != 0x0050)                /* probe response */
        return;

    bssid = frame + 16;                         /* addr3 */
    if (bssid[0] & 1)                           /* skip multicast bssid */
        return;

    ap = NULL;
    for (i = 0; i < d->n_aps; i++) {
        if (!memcmp(d->aps[i].bssid, bssid, 6)) {
            ap = &d->aps[i];
            break;
        }
    }
    if (!ap) {
        if (d->n_aps >= IWLM_MAX_APS)
            return;
        ap = &d->aps[d->n_aps++];
        memset(ap, 0, sizeof(*ap));
        memcpy(ap->bssid, bssid, 6);
    }
    if (rssi > ap->rssi || ap->rssi == 0)
        ap->rssi = rssi;

    ap->capab = get_unaligned_le16(frame + 34); /* fixed params capab */
    ap->bi = get_unaligned_le16(frame + 32);    /* beacon interval */

    ie = frame + 36;                            /* after fixed params */
    end = frame + len;
    while (ie + 2 <= end && ie + 2 + ie[1] <= end) {
        u8 id = ie[0], ielen = ie[1];
        switch (id) {
        case 0:                                 /* SSID */
            if (ielen <= 32) {
                memcpy(ap->ssid, ie + 2, ielen);
                ap->ssid[ielen] = 0;
                ap->ssid_len = ielen;
            }
            break;
        case 3:                                 /* DS params: channel */
            if (ielen >= 1)
                ap->channel = ie[2];
            break;
        case 5:                                 /* TIM: dtim period */
            if (ielen >= 2)
                ap->dtim_period = ie[3];
            break;
        case 48:                                /* RSN */
            if (2 + ielen <= sizeof(ap->rsn_ie)) {
                memcpy(ap->rsn_ie, ie, 2 + ielen);
                ap->rsn_ie_len = 2 + ielen;
            }
            parse_rsn(ap, ie + 2, ielen);
            break;
        case 61:                                /* HT op: channel fallback */
            if (!ap->channel && ielen >= 1)
                ap->channel = ie[2];
            break;
        }
        ie += 2 + ielen;
    }
    ap->band = ap->channel > 14 ? 1 : 0;
}
