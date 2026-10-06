/*
 * data.c - RX MPDU demux and the 802.3 <-> 802.11 data path.
 *
 * RX: REPLY_RX_MPDU_CMD packets carry iwl_rx_mpdu_desc (64B, v3 on
 * AX210+) followed by the 802.11 frame. Management frames go to the
 * scanner (while scanning) or the MLME, data frames are de-encapsulated
 * to ethernet and queued for netd; eapol is diverted to the handshake.
 *
 * TX: netd hands us raw ethernet frames (batched [u16 len][frame] by
 * main.c); we wrap them in a ToDS data header + LLC/SNAP and let the
 * firmware do CCMP once the pairwise key is installed.
 */
#include "../iwlm.h"
#include "../utils/log.h"
#include "../utils/qbuf.h"

#include "api/rx.h"

#include <stdio.h>

#define RX_DESC_SIZE 64                 /* offsetofend(desc, v3) */
#define RXQ_FRAMES   64
#define RXQ_FRAME_SZ 1600

/* 802.11 fc bits */
#define FC_TYPE_MGMT     0x0000
#define FC_TYPE_DATA     0x0008
#define FC_SUBTYPE_QOS   0x0080
#define FC_PROTECTED     0x0040
#define FC_TODS          0x0100
#define FC_FROMDS        0x0200

#define ETH_P_EAPOL 0x888e

static queue_buffer_t *rxq;
static u16 data_seq;

int iwlm_data_init(struct iwlm_dev *d)
{
    (void)d;
    rxq = queue_buffer_alloc(RXQ_FRAMES, RXQ_FRAME_SZ);
    return rxq ? 0 : -1;
}

int iwlm_data_rx_pending(void)
{
    return rxq ? queue_buffer_check(rxq) : 0;
}

int iwlm_data_rx_pop(u8 *buf, int size)
{
    if (!rxq)
        return -1;
    return queue_buffer_pop(rxq, buf, size);
}

/* ---------------- RX ---------------- */

static void rx_data_frame(struct iwlm_dev *d, const u8 *f, u32 len)
{
    u16 fc = get_unaligned_le16(f);
    u32 hdrlen = 24;
    u32 snap;
    u16 ethertype;
    u8 eth[1600];

    if (d->state < IWLM_STATE_EAPOL)
        return;
    /* from the AP only: FromDS, addr1 = us */
    if (!(fc & FC_FROMDS) || (fc & FC_TODS))
        return;
    if (memcmp(f + 4, d->mac, 6) && (f[4] & 1) != 1)
        return;                         /* not for us, not mcast/bcast */
    if (fc & FC_SUBTYPE_QOS)
        hdrlen += 2;
    if (len < hdrlen)
        return;

    /* CCMP: 8-byte IV after the header, 8-byte MIC at the end */
    if (fc & FC_PROTECTED) {
        hdrlen += 8;
        if (len < hdrlen + 8)
            return;
        len -= 8;
    }

    snap = hdrlen;
    if (len < snap + 8)
        return;
    if (f[snap] != 0xaa || f[snap + 1] != 0xaa || f[snap + 2] != 0x03)
        return;                         /* not LLC/SNAP (A-MSDU etc.) */
    ethertype = ((u16)f[snap + 6] << 8) | f[snap + 7];

    if (ethertype == ETH_P_EAPOL) {
        iwlm_mlme_rx_eapol(d, f + snap + 8, len - snap - 8);
        return;
    }

    if (d->state != IWLM_STATE_CONNECTED)
        return;

    /* rebuild ethernet: dst = addr1, src = addr3, payload after snap */
    if (14 + len - snap - 8 > sizeof(eth))
        return;
    memcpy(eth, f + 4, 6);              /* dst */
    memcpy(eth + 6, f + 16, 6);         /* src = addr3 */
    eth[12] = (u8)(ethertype >> 8);
    eth[13] = (u8)ethertype;
    memcpy(eth + 14, f + snap + 8, len - snap - 8);
    if (rxq)
        queue_buffer_push(rxq, eth, 14 + len - snap - 8);
}

void iwlm_mvm_rx_mpdu(struct iwlm_dev *d, const u8 *data, u32 len)
{
    const struct iwl_rx_mpdu_desc *desc = (const void *)data;
    u32 mpdu_len;
    u32 status;
    s8 rssi = 0;
    const u8 *f;
    u16 fc;

    if (len < RX_DESC_SIZE + 24)
        return;
    mpdu_len = le16_to_cpu(desc->mpdu_len);
    status = le32_to_cpu(desc->status);
    if (!(status & IWL_RX_MPDU_STATUS_CRC_OK) ||
        !(status & IWL_RX_MPDU_STATUS_OVERRUN_OK))
        return;
    if (mpdu_len < 24 || RX_DESC_SIZE + mpdu_len > len)
        return;
    if (desc->mac_flags2 & IWL_RX_MPDU_MFLG2_PAD)
        mpdu_len &= ~3u;                /* padding is not part of the frame */

    rssi = -(s8)(desc->v3.energy_a > desc->v3.energy_b ?
                 desc->v3.energy_a : desc->v3.energy_b);
    f = data + RX_DESC_SIZE;
    fc = get_unaligned_le16(f);

    if ((fc & 0x000c) == FC_TYPE_MGMT) {
        if (d->state == IWLM_STATE_SCANNING)
            iwlm_scan_rx_frame(d, f, mpdu_len, rssi);
        else
            iwlm_mlme_rx_mgmt(d, f, mpdu_len);
        return;
    }
    if ((fc & 0x000c) == FC_TYPE_DATA)
        rx_data_frame(d, f, mpdu_len);
}

/* ---------------- TX ---------------- */

int iwlm_data_tx(struct iwlm_dev *d, const void *eth, u32 len)
{
    u8 hdr[24];
    u8 body[1600];
    const u8 *e = eth;
    u32 plen;

    if (d->state != IWLM_STATE_CONNECTED || len < 14)
        return -1;
    plen = len - 14;
    if (8 + plen > sizeof(body))
        return -2;

    put_unaligned_le16(FC_TYPE_DATA | FC_TODS, hdr);
    put_unaligned_le16(0, hdr + 2);
    memcpy(hdr + 4, d->cur_ap.bssid, 6);    /* addr1 = bssid */
    memcpy(hdr + 10, d->mac, 6);            /* addr2 = ta */
    memcpy(hdr + 16, e, 6);                 /* addr3 = eth dst */
    put_unaligned_le16((data_seq++ & 0xfff) << 4, hdr + 22);

    body[0] = 0xaa;                         /* LLC/SNAP */
    body[1] = 0xaa;
    body[2] = 0x03;
    body[3] = body[4] = body[5] = 0;
    body[6] = e[12];
    body[7] = e[13];
    memcpy(body + 8, e + 14, plen);

    /* fw applies CCMP once the pairwise key is in */
    return iwlm_trans_tx_frame(d, hdr, 24, body, 8 + plen,
                               d->pairwise_key_set) == 0 ? (int)len : -3;
}
