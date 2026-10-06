/*
 * mlme.c - STA association state machine and WPA2-PSK (CCMP) 4-way
 * handshake, condensed from Linux iwlwifi mvm/phy-ctxt.c, mac-ctxt.c,
 * binding.c, sta.c, mld-key.c plus the wpa_supplicant SME/WPA logic.
 *
 * connect() runs synchronously in the dev.cmd worker thread; frames
 * received meanwhile drive d->state forward via the rx handlers.
 */
#include "../iwlm.h"
#include "../utils/log.h"
#include "../utils/aes.h"
#include "../utils/pbkdf2.h"

#include "api/commands.h"
#include "api/phy-ctxt.h"
#include "api/mac.h"
#include "api/binding.h"
#include "api/context.h"
#include "api/sta.h"
#include "api/txq.h"
#include "api/datapath.h"

#include <stdio.h>
#include <unistd.h>

#define AP_STA_ID   0           /* station slot for the AP */

/* 802.11 frame control bits used here */
#define FC_MGMT_AUTH        0x00b0
#define FC_MGMT_ASSOC_REQ   0x0000
#define FC_MGMT_ASSOC_RESP  0x0010
#define FC_MGMT_DEAUTH      0x00c0
#define FC_MGMT_DISASSOC    0x00a0
#define FC_DATA_TODS        0x0108  /* data, ToDS */

#define ETH_P_EAPOL 0x888e

/* EAPOL-Key key_info bits (host order) */
#define KI_VER_MSK   0x0007
#define KI_PAIRWISE  0x0008
#define KI_INSTALL   0x0040
#define KI_ACK       0x0080
#define KI_MIC       0x0100
#define KI_SECURE    0x0200
#define KI_ENCRYPTED 0x1000

/* ---------------- helpers ---------------- */

static u8 ap_band_phy(const struct iwlm_ap_info *ap)
{
    return ap->band ? PHY_BAND_5 : PHY_BAND_24;
}

/* 5 GHz lives on LMAC 1 only when the fw has a second (CDB) lmac */
static u8 ap_lmac(struct iwlm_dev *d, const struct iwlm_ap_info *ap)
{
    if (ap->band && iwlm_fw_has_capa(d, IWL_UCODE_TLV_CAPA_CDB_SUPPORT))
        return 1;
    return 0;
}

static u32 rx_chain_info(struct iwlm_dev *d)
{
    /* diversity: 2 chains static + dynamic on the valid rx antennas */
    return (u32)iwlm_valid_rx_ant(d) << PHY_RX_CHAIN_VALID_POS |
           2 << PHY_RX_CHAIN_CNT_POS | 2 << PHY_RX_CHAIN_MIMO_CNT_POS;
}

/* ---------------- phy context ---------------- */

static int phy_ctxt_apply(struct iwlm_dev *d, u32 action)
{
    struct iwlm_ap_info *ap = &d->cur_ap;
    u16 id = d->phy_ctxt_id[ap->band];
    struct iwl_phy_context_cmd cmd;
    int ret;

    memset(&cmd, 0, sizeof(cmd));
    cmd.id_and_color = cpu_to_le32(FW_CMD_ID_AND_COLOR(id, 0));
    cmd.action = cpu_to_le32(action);
    cmd.lmac_id = cpu_to_le32(ap_lmac(d, ap));
    cmd.ci.channel = cpu_to_le32(ap->channel);
    cmd.ci.band = ap_band_phy(ap);
    cmd.ci.width = IWL_PHY_CHANNEL_MODE20;
    cmd.ci.ctrl_pos = 0;
    if (iwlm_fw_cmd_ver(&d->fw, DATA_PATH_GROUP, RLC_CONFIG_CMD) < 2)
        cmd.rxchain_info = cpu_to_le32(rx_chain_info(d));

    ret = iwlm_trans_send_cmd_pdu(d, LEGACY_GROUP, PHY_CONTEXT_CMD,
                                  &cmd, sizeof(cmd));
    if (ret || action == FW_CTXT_ACTION_REMOVE)
        return ret;

    /* RLC lives in a separate command on modern firmware */
    if (iwlm_fw_cmd_ver(&d->fw, DATA_PATH_GROUP, RLC_CONFIG_CMD) >= 2) {
        struct iwl_rlc_config_cmd rlc;

        memset(&rlc, 0, sizeof(rlc));
        rlc.phy_id = cpu_to_le32(id);
        rlc.rlc.rx_chain_info = cpu_to_le32(rx_chain_info(d));
        /* Linux wraps this one with iwl_cmd_id(): header version 2 */
        ret = iwlm_trans_send_cmd(d, DATA_PATH_GROUP, RLC_CONFIG_CMD, 2,
                                  &rlc, sizeof(rlc), NULL, NULL,
                                  IWLM_HCMD_TIMEOUT_MS);
    }
    return ret;
}

/* ---------------- mac context ---------------- */

static int mac_ctx_apply(struct iwlm_dev *d, u32 action, bool assoc)
{
    struct iwlm_ap_info *ap = &d->cur_ap;
    struct iwl_mac_ctx_cmd cmd;
    static const u8 fifo_gen2[4] = { IWL_GEN2_EDCA_TX_FIFO_BK,
                                     IWL_GEN2_EDCA_TX_FIFO_BE,
                                     IWL_GEN2_EDCA_TX_FIFO_VI,
                                     IWL_GEN2_EDCA_TX_FIFO_VO };
    static const u8 fifo_bz[4] = { IWL_BZ_EDCA_TX_FIFO_BK,
                                   IWL_BZ_EDCA_TX_FIFO_BE,
                                   IWL_BZ_EDCA_TX_FIFO_VI,
                                   IWL_BZ_EDCA_TX_FIFO_VO };
    const u8 *fifo = d->cfg->family == IWLM_FAM_BZ ? fifo_bz : fifo_gen2;
    int i;

    memset(&cmd, 0, sizeof(cmd));
    cmd.id_and_color = cpu_to_le32(FW_CMD_ID_AND_COLOR(d->mac_id, 0));
    cmd.action = cpu_to_le32(action);
    cmd.mac_type = cpu_to_le32(FW_MAC_TYPE_BSS_STA);
    cmd.tsf_id = 0;                     /* TSF_ID_A */
    memcpy(cmd.node_addr, d->mac, 6);
    memcpy(cmd.bssid_addr, ap->bssid, 6);

    /* ack/basic rates: 2.4G cck 1/2/5.5/11 + ofdm 6; 5G ofdm 6/12/24 */
    if (ap->band) {
        cmd.cck_rates = 0;
        cmd.ofdm_rates = cpu_to_le32(0x15);
    } else {
        cmd.cck_rates = cpu_to_le32(0xf);
        cmd.ofdm_rates = cpu_to_le32(0x1);
    }

    cmd.filter_flags = cpu_to_le32(MAC_FILTER_ACCEPT_GRP |
                                   (assoc ? 0 : MAC_FILTER_IN_BEACON));
    cmd.qos_flags = 0;      /* legacy: no edca update, no tgn */

    /* legacy qos: flat cw/aifs, per-AC fifo (ucode ac order BK,BE,VI,VO) */
    for (i = 0; i < 4; i++) {
        cmd.ac[i].cw_min = cpu_to_le16(15);
        cmd.ac[i].cw_max = cpu_to_le16(1023);
        cmd.ac[i].aifsn = 2;
        cmd.ac[i].edca_txop = 0;
        cmd.ac[i].fifos_mask = BIT(fifo[i]);
    }

    cmd.sta.is_assoc = cpu_to_le32(assoc ? 1 : 0);
    cmd.sta.bi = cpu_to_le32(ap->bi ? ap->bi : 100);
    cmd.sta.dtim_interval =
        cpu_to_le32((u32)(ap->bi ? ap->bi : 100) *
                    (ap->dtim_period ? ap->dtim_period : 1));
    cmd.sta.listen_interval = cpu_to_le32(10);
    cmd.sta.assoc_id = cpu_to_le32(d->aid);

    return iwlm_trans_send_cmd_pdu(d, LEGACY_GROUP, MAC_CONTEXT_CMD,
                                   &cmd, sizeof(cmd));
}

/* ---------------- binding ---------------- */

static int binding_apply(struct iwlm_dev *d, u32 action)
{
    struct iwlm_ap_info *ap = &d->cur_ap;
    u16 phy_id = d->phy_ctxt_id[ap->band];
    struct iwl_binding_cmd cmd;
    __le32 status = 0;
    u32 rsp_len = sizeof(status);
    u32 len;
    int ret;

    memset(&cmd, 0, sizeof(cmd));
    cmd.id_and_color = cpu_to_le32(FW_CMD_ID_AND_COLOR(0, 0));
    cmd.action = cpu_to_le32(action);
    cmd.macs[0] = cpu_to_le32(FW_CMD_ID_AND_COLOR(d->mac_id, 0));
    cmd.macs[1] = cpu_to_le32(FW_CTXT_INVALID);
    cmd.macs[2] = cpu_to_le32(FW_CTXT_INVALID);
    cmd.phy = cpu_to_le32(FW_CMD_ID_AND_COLOR(phy_id, 0));

    if (iwlm_fw_has_capa(d, IWL_UCODE_TLV_CAPA_BINDING_CDB_SUPPORT)) {
        cmd.lmac_id = cpu_to_le32(ap_lmac(d, ap));
        len = sizeof(cmd);
    } else {
        len = IWL_BINDING_CMD_SIZE_V1;
    }

    ret = iwlm_trans_send_cmd(d, LEGACY_GROUP, BINDING_CONTEXT_CMD, 0,
                              &cmd, len, &status, &rsp_len,
                              IWLM_HCMD_TIMEOUT_MS);
    if (ret)
        return ret;
    if (rsp_len < sizeof(status) || le32_to_cpu(status) != 0)
        return -2;
    return 0;
}

/* ---------------- AP station ---------------- */

static int ap_sta_add(struct iwlm_dev *d)
{
    struct iwl_mvm_add_sta_cmd cmd;
    __le32 status = 0;
    u32 rsp_len = sizeof(status);
    int ret;

    memset(&cmd, 0, sizeof(cmd));
    cmd.add_modify = 0;                 /* STA_MODE_ADD */
    cmd.sta_id = AP_STA_ID;
    cmd.mac_id_n_color = cpu_to_le32(FW_CMD_ID_AND_COLOR(d->mac_id, 0));
    memcpy(cmd.addr, d->cur_ap.bssid, 6);
    cmd.tid_disable_tx = cpu_to_le16(0xffff);
    cmd.station_flags_msk = cpu_to_le32(STA_FLG_FAT_EN_MSK |
                                        STA_FLG_MIMO_EN_MSK |
                                        STA_FLG_RTS_MIMO_PROT);
    cmd.station_flags = cpu_to_le32(STA_FLG_MIMO_EN_SISO);
    if (iwlm_fw_has_api(d, IWL_UCODE_TLV_API_STA_TYPE))
        cmd.station_type = IWL_STA_LINK;
    cmd.tfd_queue_msk = 0;              /* new tx api: set via SCD cfg */

    ret = iwlm_trans_send_cmd(d, LEGACY_GROUP, ADD_STA, 0, &cmd,
                              sizeof(cmd), &status, &rsp_len,
                              IWLM_HCMD_TIMEOUT_MS);
    if (ret)
        return ret;
    if (rsp_len < sizeof(status) ||
        (le32_to_cpu(status) & IWL_ADD_STA_STATUS_MASK) != ADD_STA_SUCCESS)
        return -2;
    d->ap_sta_id = AP_STA_ID;
    return 0;
}

static int ap_sta_remove(struct iwlm_dev *d)
{
    struct iwl_mvm_rm_sta_cmd cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.sta_id = AP_STA_ID;
    return iwlm_trans_send_cmd_pdu(d, LEGACY_GROUP, REMOVE_STA,
                                   &cmd, sizeof(cmd));
}

/* ---------------- key install (SEC_KEY_CMD, grp 5 cmd 0x18) ---------------- */

static int sec_key_add(struct iwlm_dev *d, u8 key_id, u32 flags,
                       const u8 *key)
{
    struct iwl_sec_key_cmd cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.action = cpu_to_le32(FW_CTXT_ACTION_ADD);
    cmd.u.add.sta_mask = cpu_to_le32(BIT(d->ap_sta_id));
    cmd.u.add.key_id = cpu_to_le32(key_id);
    cmd.u.add.key_flags = cpu_to_le32(flags);
    memcpy(cmd.u.add.key, key, 16);

    return iwlm_trans_send_cmd_pdu(d, DATA_PATH_GROUP, SEC_KEY_CMD,
                                   &cmd, sizeof(cmd));
}

/* ---------------- 802.11 frame tx ---------------- */

static u16 mgmt_seq;

static int tx_mgmt(struct iwlm_dev *d, u16 fc, const u8 *da,
                   const u8 *body, u32 bodylen)
{
    u8 hdr[24];
    u8 *p = hdr;

    put_unaligned_le16(fc, p);
    put_unaligned_le16(0, p + 2);           /* duration */
    memcpy(p + 4, da, 6);                   /* addr1 */
    memcpy(p + 10, d->mac, 6);              /* addr2 */
    memcpy(p + 16, d->cur_ap.bssid, 6);     /* addr3 */
    put_unaligned_le16((mgmt_seq++ & 0xfff) << 4, p + 22);
    return iwlm_trans_tx_frame(d, hdr, 24, body, bodylen, false);
}

static int send_auth(struct iwlm_dev *d)
{
    u8 body[6];

    put_unaligned_le16(0, body);            /* open system */
    put_unaligned_le16(1, body + 2);        /* transaction seq */
    put_unaligned_le16(0, body + 4);        /* status */
    return tx_mgmt(d, FC_MGMT_AUTH, d->cur_ap.bssid, body, sizeof(body));
}

/* legacy rate IEs: 2.4G 1-11 + ofdm in ext; 5G all ofdm */
static const u8 rates_24[8] = { 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };
static const u8 rates_ext[4] = { 0x30, 0x48, 0x60, 0x6c };
static const u8 rates_5[8] = { 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c };

static int send_assoc(struct iwlm_dev *d)
{
    struct iwlm_ap_info *ap = &d->cur_ap;
    u8 body[128];
    u8 *p = body;
    u16 capab = 0x0001 | (ap->capab & 0x0420); /* ESS + short pre/slot */

    put_unaligned_le16(capab, p);
    put_unaligned_le16(10, p + 2);          /* listen interval */
    p += 4;

    *p++ = 0;                               /* SSID */
    *p++ = ap->ssid_len;
    memcpy(p, ap->ssid, ap->ssid_len);
    p += ap->ssid_len;

    if (ap->band) {
        *p++ = 1;
        *p++ = 8;
        memcpy(p, rates_5, 8);
        p += 8;
    } else {
        *p++ = 1;
        *p++ = 8;
        memcpy(p, rates_24, 8);
        p += 8;
        *p++ = 50;                          /* extended rates */
        *p++ = 4;
        memcpy(p, rates_ext, 4);
        p += 4;
    }

    if (ap->has_rsn && ap->rsn_ie_len) {
        memcpy(p, ap->rsn_ie, ap->rsn_ie_len);
        p += ap->rsn_ie_len;
    }

    return tx_mgmt(d, FC_MGMT_ASSOC_REQ, ap->bssid, body, p - body);
}

/* ---------------- state wait ---------------- */

static int wait_state(struct iwlm_dev *d, enum iwlm_state want, int timeout_ms)
{
    int waited;

    for (waited = 0; waited < timeout_ms; waited += 10) {
        iwlm_trans_rx_poll(d);
        if (d->state == want || d->state == IWLM_STATE_CONNECTED)
            return 0;
        if (d->state == IWLM_STATE_FW_LOADED)
            return -1;      /* deauth/disassoc kicked us out */
        usleep(10000);
    }
    return -2;
}

/* ---------------- eapol / WPA2 ---------------- */

static u64 xrstate;

static u32 xrand(void)
{
    u64 x;

    __asm__ volatile("rdtsc" : "=A"(x));
    xrstate ^= x + 0x9e3779b97f4a7c15ULL;
    xrstate ^= xrstate << 13;
    xrstate ^= xrstate >> 7;
    xrstate ^= xrstate << 17;
    return (u32)xrstate;
}

static void prf_sha1(const u8 *key, u32 key_len, const char *label,
                     const u8 *data, u32 data_len, u8 *out, u32 out_len)
{
    u8 buf[160];
    u8 digest[20];
    u32 i, pos = 0, llen = strlen(label);

    for (i = 0; pos < out_len; i++) {
        u32 n;

        memcpy(buf, label, llen);
        buf[llen] = 0;
        memcpy(buf + llen + 1, data, data_len);
        buf[llen + 1 + data_len] = (u8)i;
        sha1_hmac(key, key_len, buf, llen + 1 + data_len + 1, digest);
        n = out_len - pos > 20 ? 20 : out_len - pos;
        memcpy(out + pos, digest, n);
        pos += n;
    }
}

static void derive_ptk(struct iwlm_dev *d)
{
    u8 data[76];
    const u8 *aa = d->cur_ap.bssid, *sa = d->mac;
    const u8 *an = d->anonce, *sn = d->snonce;
    int i;

    for (i = 0; i < 6; i++) {
        data[i] = aa[i] < sa[i] ? aa[i] : sa[i];          /* min mac */
        data[6 + i] = aa[i] < sa[i] ? sa[i] : aa[i];      /* max mac */
    }
    for (i = 0; i < 32; i++) {
        data[12 + i] = an[i] < sn[i] ? an[i] : sn[i];
        data[44 + i] = an[i] < sn[i] ? sn[i] : an[i];
    }
    prf_sha1(d->pmk, 32, "Pairwise key expansion", data, sizeof(data),
             d->ptk, 64);
    d->have_ptk = true;
}

/* eapol frame: eapol hdr(4) + key desc(95+) */
static int eapol_tx(struct iwlm_dev *d, const u8 *eapol, u32 len)
{
    u8 hdr[24];
    u8 snap[8] = { 0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x88, 0x8e };
    u8 body[256];
    u8 *p = hdr;

    if (len + 8 > sizeof(body))
        return -1;
    put_unaligned_le16(FC_DATA_TODS, p);
    put_unaligned_le16(0, p + 2);
    memcpy(p + 4, d->cur_ap.bssid, 6);      /* addr1 = ra = bssid */
    memcpy(p + 10, d->mac, 6);              /* addr2 = ta */
    memcpy(p + 16, d->cur_ap.bssid, 6);     /* addr3 = da */
    put_unaligned_le16((mgmt_seq++ & 0xfff) << 4, p + 22);

    memcpy(body, snap, 8);
    memcpy(body + 8, eapol, len);
    return iwlm_trans_tx_frame(d, hdr, 24, body, len + 8, false);
}

/*
 * Build a response key frame (msg2/msg4/group-msg2): eapol header +
 * descriptor, mic computed over the whole frame with KCK.
 */
static int eapol_send_resp(struct iwlm_dev *d, u16 ki, const u8 *replay,
                           const u8 *nonce, const u8 *kdata, u16 kdlen)
{
    u8 f[192];
    u8 *p = f;
    u32 len = 95 + kdlen;

    *p++ = 2;                               /* eapol version */
    *p++ = 3;                               /* type: key */
    put_unaligned_le16((u16)((len >> 8) | (len << 8)), p);
    p += 2;
    *p++ = 2;                               /* desc: RSN key */
    put_unaligned_le16((u16)((ki >> 8) | (ki << 8)), p);
    p += 2;
    put_unaligned_le16(16 << 8, p);         /* key length, BE */
    p += 2;
    memcpy(p, replay, 8);
    p += 8;
    memcpy(p, nonce, 32);
    p += 32;
    memset(p, 0, 16 + 8 + 8);               /* iv, rsc, id */
    p += 32;
    memset(p, 0, 16);                       /* mic, filled below */
    p += 16;
    *p++ = (u8)(kdlen >> 8);
    *p++ = (u8)kdlen;
    if (kdlen) {
        memcpy(p, kdata, kdlen);
        p += kdlen;
    }

    /* MIC = HMAC-SHA1(KCK, frame)[0:16] */
    {
        u8 digest[20];
        sha1_hmac(d->ptk, 16, f, p - f, digest);
        memcpy(f + 81, digest, 16);
    }
    return eapol_tx(d, f, p - f);
}

/* RFC3394-unwrap the msg3 key data and pull the GTK KDE out */
static int eapol_extract_gtk(struct iwlm_dev *d, const u8 *kdata, u32 kdlen,
                             u8 *gtk_id)
{
    u8 plain[128];
    u32 i;

    if (kdlen < 16 || kdlen - 8 > sizeof(plain) || (kdlen & 7))
        return -1;
    if (aes_key_unwrap(d->ptk + 16, kdata, kdlen, plain))
        return -2;

    for (i = 0; i + 2 <= kdlen - 8;) {
        u8 id = plain[i], len = plain[i + 1];

        if (i + 2 + len > kdlen - 8)
            break;
        /* GTK KDE: dd 16 00 0f ac 01 [keyid/rsv][rsv][gtk 16] */
        if (id == 0xdd && len >= 22 && plain[i + 2] == 0x00 &&
            plain[i + 3] == 0x0f && plain[i + 4] == 0xac &&
            plain[i + 5] == 0x01) {
            *gtk_id = plain[i + 6] & 3;
            memcpy(d->gtk, plain + i + 8, 16);
            d->have_gtk = true;
            return 0;
        }
        i += 2 + len;
    }
    return -3;
}

static bool eapol_mic_ok(struct iwlm_dev *d, const u8 *f, u32 len)
{
    u8 tmp[256];
    u8 digest[20];

    if (len > sizeof(tmp))
        return false;
    memcpy(tmp, f, len);
    memset(tmp + 81, 0, 16);                /* zero the mic field */
    sha1_hmac(d->ptk, 16, tmp, len, digest);
    return !memcmp(digest, f + 81, 16);
}

void iwlm_mlme_rx_eapol(struct iwlm_dev *d, const u8 *payload, u32 len)
{
    const u8 *f = payload;      /* starts at the eapol version byte */
    u16 ki, kdlen;
    const u8 *nonce, *replay, *kdata;
    u8 gtk_id;
    u8 zero_nonce[32] = { 0 };

    if (len < 99 || f[1] != 3 || f[4] != 2)
        return;
    ki = ((u16)f[5] << 8) | f[6];
    if ((ki & KI_VER_MSK) != 2)
        return;                 /* CCMP/AES descriptor version only */
    replay = f + 9;
    nonce = f + 17;
    kdlen = ((u16)f[97] << 8) | f[98];
    kdata = f + 99;
    if (99 + kdlen > len)
        return;

    if ((ki & KI_PAIRWISE) && (ki & KI_ACK) && !(ki & KI_MIC)) {
        /* msg1: store anonce, derive ptk, answer msg2 */
        memcpy(d->anonce, nonce, 32);
        memcpy(d->replay_ctr, replay, 8);
        {
            int i;
            u32 *s = (u32 *)d->snonce;
            for (i = 0; i < 8; i++)
                s[i] = xrand();
        }
        derive_ptk(d);
        d->eapol_msg = 3;
        eapol_send_resp(d, 0x010a, replay, d->snonce,
                        d->cur_ap.rsn_ie, d->cur_ap.rsn_ie_len);
        return;
    }

    if ((ki & KI_PAIRWISE) && (ki & KI_MIC) && (ki & KI_ACK)) {
        /* msg3: verify mic, unwrap gtk, ack, install keys, connected */
        if (d->eapol_msg != 3 || !d->have_ptk)
            return;
        if (!(ki & KI_SECURE) || !(ki & KI_ENCRYPTED))
            return;
        if (!eapol_mic_ok(d, f, 99 + kdlen)) {
            klog("iwlm: eapol msg3 mic mismatch\n");
            return;
        }
        if (eapol_extract_gtk(d, kdata, kdlen, &gtk_id)) {
            klog("iwlm: no gtk in msg3\n");
            return;
        }
        eapol_send_resp(d, 0x030a, replay, zero_nonce, NULL, 0);
        if (sec_key_add(d, 0, IWL_SEC_KEY_FLAG_CIPHER_CCMP, d->ptk + 32)) {
            klog("iwlm: ptk install failed\n");
            return;
        }
        d->pairwise_key_set = true;
        if (sec_key_add(d, gtk_id, IWL_SEC_KEY_FLAG_CIPHER_CCMP |
                                   IWL_SEC_KEY_FLAG_MCAST_KEY, d->gtk)) {
            klog("iwlm: gtk install failed\n");
            return;
        }
        d->eapol_msg = 0;
        d->state = IWLM_STATE_CONNECTED;
        klog("iwlm: connected\n");
        return;
    }

    if (!(ki & KI_PAIRWISE) && (ki & KI_MIC) && (ki & KI_ACK)) {
        /* group rekey (group msg1): unwrap new gtk, ack, reinstall */
        if (!d->have_ptk || !(ki & KI_SECURE) || !(ki & KI_ENCRYPTED))
            return;
        if (!eapol_mic_ok(d, f, 99 + kdlen))
            return;
        if (eapol_extract_gtk(d, kdata, kdlen, &gtk_id))
            return;
        eapol_send_resp(d, 0x0302, replay, zero_nonce, NULL, 0);
        sec_key_add(d, gtk_id, IWL_SEC_KEY_FLAG_CIPHER_CCMP |
                               IWL_SEC_KEY_FLAG_MCAST_KEY, d->gtk);
        return;
    }
}

/* ---------------- mgmt rx ---------------- */

void iwlm_mlme_rx_mgmt(struct iwlm_dev *d, const u8 *frame, u32 len)
{
    u16 fc, st;

    if (len < 24)
        return;
    fc = get_unaligned_le16(frame);

    switch (fc & 0x00fc) {
    case FC_MGMT_AUTH:
        if (d->state != IWLM_STATE_AUTHENTICATING || len < 30)
            return;
        if (memcmp(frame + 16, d->cur_ap.bssid, 6))
            return;
        st = get_unaligned_le16(frame + 24 + 4);
        if (st == 0)
            d->state = IWLM_STATE_ASSOCIATING;
        break;
    case FC_MGMT_ASSOC_RESP:
        if (d->state != IWLM_STATE_ASSOCIATING || len < 30)
            return;
        if (memcmp(frame + 16, d->cur_ap.bssid, 6))
            return;
        st = get_unaligned_le16(frame + 24 + 2);
        if (st == 0) {
            d->aid = get_unaligned_le16(frame + 24 + 4) & 0x3fff;
            d->state = IWLM_STATE_EAPOL;
        }
        break;
    case FC_MGMT_DEAUTH:
    case FC_MGMT_DISASSOC:
        if (!memcmp(frame + 16, d->cur_ap.bssid, 6) &&
            d->state >= IWLM_STATE_AUTHENTICATING) {
            klog("iwlm: disconnected by AP\n");
            d->state = IWLM_STATE_FW_LOADED;
        }
        break;
    default:
        break;
    }
}

/* ---------------- connect / disconnect ---------------- */

static struct iwlm_ap_info *find_ap(struct iwlm_dev *d, const char *ssid)
{
    struct iwlm_ap_info *best = NULL;
    int i;

    for (i = 0; i < d->n_aps; i++) {
        struct iwlm_ap_info *ap = &d->aps[i];

        if (!ap->ssid_len || strcmp((const char *)ap->ssid, ssid))
            continue;
        /* WPA2-PSK/CCMP only */
        if (!ap->has_rsn || ap->rsn_akm != 2 || ap->rsn_pairwise != 4)
            continue;
        if (!best || ap->rssi > best->rssi)
            best = ap;
    }
    return best;
}

/* 64 hex chars = raw PMK (network.json "pmk" field), else passphrase */
static bool hex_to_pmk(const char *s, u8 *pmk)
{
    int i;

    if (strlen(s) != 64)
        return false;
    for (i = 0; i < 32; i++) {
        int hi, lo;
        char c0 = s[2 * i], c1 = s[2 * i + 1];

        hi = c0 >= '0' && c0 <= '9' ? c0 - '0' :
             c0 >= 'a' && c0 <= 'f' ? c0 - 'a' + 10 :
             c0 >= 'A' && c0 <= 'F' ? c0 - 'A' + 10 : -1;
        lo = c1 >= '0' && c1 <= '9' ? c1 - '0' :
             c1 >= 'a' && c1 <= 'f' ? c1 - 'a' + 10 :
             c1 >= 'A' && c1 <= 'F' ? c1 - 'A' + 10 : -1;
        if (hi < 0 || lo < 0)
            return false;
        pmk[i] = (u8)(hi << 4 | lo);
    }
    return true;
}

int iwlm_mvm_connect(struct iwlm_dev *d, const char *ssid, const char *pass)
{
    struct iwlm_ap_info *ap;
    int ret;

    if (d->state < IWLM_STATE_FW_LOADED)
        return -1;
    ap = find_ap(d, ssid);
    if (!ap) {
        klog("iwlm: %s not found (or not WPA2-PSK/CCMP)\n", ssid);
        return -2;
    }
    d->cur_ap = *ap;
    klog("iwlm: connecting to %s ch %d rssi %d\n",
         ap->ssid, ap->channel, ap->rssi);

    /* PMK first: the 4096-round PBKDF2 takes a while on this cpu */
    if (!hex_to_pmk(pass, d->pmk))
        PKCS5_PBKDF2_HMAC((const u8 *)pass, strlen(pass),
                          ap->ssid, ap->ssid_len, 4096, 32, d->pmk);
    d->have_ptk = d->have_gtk = d->pairwise_key_set = false;
    d->eapol_msg = 0;

    /* phy context on the AP channel (id per band), then mac + binding */
    d->phy_ctxt_id[ap->band] = 0;
    d->mac_id = 0;
    ret = phy_ctxt_apply(d, FW_CTXT_ACTION_ADD);
    if (ret) {
        klog("iwlm: phy ctxt failed %d\n", ret);
        return -3;
    }
    ret = mac_ctx_apply(d, FW_CTXT_ACTION_ADD, false);
    if (ret) {
        klog("iwlm: mac ctxt failed %d\n", ret);
        return -4;
    }
    ret = binding_apply(d, FW_CTXT_ACTION_ADD);
    if (ret) {
        klog("iwlm: binding failed %d\n", ret);
        return -5;
    }

    /* the AP station must exist before any frame is sent through it */
    ret = ap_sta_add(d);
    if (ret) {
        klog("iwlm: add ap sta failed %d\n", ret);
        return -6;
    }
    ret = iwlm_trans_txq_alloc(d, BIT(AP_STA_ID), 0, 256);
    if (ret < 0) {
        klog("iwlm: data queue alloc failed %d\n", ret);
        return -7;
    }

    d->state = IWLM_STATE_AUTHENTICATING;
    if (send_auth(d))
        goto fail;
    if (wait_state(d, IWLM_STATE_ASSOCIATING, 2000))
        goto fail;

    if (send_assoc(d))
        goto fail;
    if (wait_state(d, IWLM_STATE_EAPOL, 4000))
        goto fail;

    /* tell the fw we are associated (dtim, aid) then run the 4-way */
    ret = mac_ctx_apply(d, FW_CTXT_ACTION_MODIFY, true);
    if (ret)
        goto fail;

    if (wait_state(d, IWLM_STATE_CONNECTED, 10000)) {
        klog("iwlm: 4-way handshake timed out\n");
        goto fail;
    }
    return 0;

fail:
    /* tear the contexts down so a retry starts from a clean slate */
    d->state = IWLM_STATE_FW_LOADED;
    ap_sta_remove(d);
    binding_apply(d, FW_CTXT_ACTION_REMOVE);
    mac_ctx_apply(d, FW_CTXT_ACTION_REMOVE, false);
    phy_ctxt_apply(d, FW_CTXT_ACTION_REMOVE);
    return -8;
}

void iwlm_mvm_disconnect(struct iwlm_dev *d)
{
    if (d->state < IWLM_STATE_AUTHENTICATING)
        return;
    d->state = IWLM_STATE_FW_LOADED;
    ap_sta_remove(d);
    binding_apply(d, FW_CTXT_ACTION_REMOVE);
    mac_ctx_apply(d, FW_CTXT_ACTION_REMOVE, false);
    phy_ctxt_apply(d, FW_CTXT_ACTION_REMOVE);
    d->pairwise_key_set = d->have_ptk = d->have_gtk = false;
}
