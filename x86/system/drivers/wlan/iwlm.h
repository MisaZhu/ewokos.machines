/*
 * iwlm.h - master state for the EwokOS Intel WLAN driver (BE202, AX211/AX411)
 *
 * Condensed port of Linux iwlwifi (pcie gen3 transport + mvm op-mode)
 * to a polled userspace driver. All DMA memory comes from EwokOS dma_alloc()
 * (uncached, contiguous); all hardware progress is observed by polling
 * shared-memory write pointers.
 */
#ifndef __IWLM_H__
#define __IWLM_H__

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ewoksys/dma.h>

#include <linux/types.h>
#include <linux/ieee80211.h>

#include "iwl/cfg.h"
#include "iwl/regs.h"

/* ---------------- firmware image ---------------- */

#define IWLM_FW_MAX_SEC   96

struct iwlm_fw_sec {
    u32 offset;                 /* SRAM dest addr (or separator marker) */
    const u8 *data;             /* points into the loaded file buffer */
    u32 size;
};

/* PNVM (platform NVM): separate .pnvm file, SKU/hw matched, DMAed to the
 * device after the alive notification (which carries the sku_id). */
#define IWLM_PNVM_MAX_CHUNKS 8

struct iwlm_fw {
    u8 *file;                   /* whole .ucode file */
    u32 file_size;
    char ver_str[64];           /* human readable */
    u32 api_ver;                /* from file name */

    struct iwlm_fw_sec sec[IWLM_FW_MAX_SEC];
    int n_sec;
    bool dual_cpus;             /* TLV NUM_OF_CPU == 2 */
    u32 phy_config;             /* TLV PHY_SKU */
    u32 n_scan_channels;
    u32 num_stations;

    u32 capa[8];                /* TLV ENABLED_CAPABILITIES bitset (<=256) */
    u32 api[8];                 /* TLV API_CHANGES_SET bitset */

    const u8 *iml;              /* IML section (ROM boot for BZ) */
    u32 iml_size;

    /* .pnvm file (loaded alongside, parsed after alive) */
    u8 *pnvm;
    u32 pnvm_size;
    const u8 *pnvm_chunk[IWLM_PNVM_MAX_CHUNKS]; /* into pnvm buffer */
    u32 pnvm_chunk_len[IWLM_PNVM_MAX_CHUNKS];
    int pnvm_n_chunks;
    u32 pnvm_version;

    /* TLV CMD_VERSIONS: (group << 8 | cmd) -> cmd version, 0 = legacy */
    u8 cmd_ver[64 * 256 / 8];   /* bitmap "known" */
    u8 cmd_vers[64 * 256];      /* version per (group<<8|cmd) 64KB — fine */

    /* fw sections split for ctxt_info dram map */
    int lmac_cnt, umac_cnt, paging_cnt;
};

static inline u8 iwlm_fw_cmd_ver(const struct iwlm_fw *fw, u8 group, u8 cmd)
{
    u32 id;

    /* fw/img.c: legacy-group ids are looked up under LONG_GROUP */
    if (group == 0)
        group = 1;
    id = ((u32)group << 8) | cmd;
    return fw->cmd_ver[id >> 3] & BIT(id & 7) ? fw->cmd_vers[id] : 0;
}

/* ---------------- DMA regions ---------------- */

struct iwlm_dma {
    ewokos_addr_t va;
    ewokos_addr_t pa;
    u32 size;
};

/* ---------------- transport ---------------- */

#define IWLM_RX_POOL     512            /* one RB per free RBD entry */
#define IWLM_CMDQ_SIZE   32
#define IWLM_CMDQ_ID     0

struct iwlm_rxq {
    struct iwlm_dma free_td;            /* 512 x iwl_rx_transfer_desc */
    struct iwlm_dma used_cd;            /* 512 x completion desc (SO:32B BZ:4B) */
    struct iwlm_dma stts;               /* page, le16 write index at [0] */
    struct iwlm_dma bufs;               /* IWLM_RX_POOL x 4K */
    u16 read;                           /* host read index into used ring */
    u16 write;                          /* host write index into free ring */
    u8  rb_used[IWLM_RX_POOL / 8];      /* which pool slots are on the NIC */
};

struct iwlm_txq {
    struct iwlm_dma tfd;                /* size x 256B */
    struct iwlm_dma bc;                 /* byte counts (u16 per entry) + pad */
    struct iwlm_dma bufs;               /* per-entry command/frame storage */
    u16 write_ptr;
    u16 read_ptr;
    u16 size;                           /* power of two */
    u16 id;
    bool used;
};

/* host command tracking (sync send: we poll for the response) */
#define IWLM_HCMD_TIMEOUT_MS 2000

struct iwlm_hcmd_rsp {
    volatile int done;                  /* set when matching resp rx'ed */
    int status;                         /* 0 ok / -EIO / -ETIMEDOUT */
    u8 *buf;                            /* response payload (copied) */
    u32 len;
};

struct iwlm_trans {
    /* context info gen3 self-load structures */
    struct iwlm_dma ctxt_info;          /* 4K: struct iwl_context_info_gen3 */
    struct iwlm_dma prph_scratch;       /* 4K: struct iwl_prph_scratch */
    struct iwlm_dma prph_info;          /* 4K page: tr_tail/cr_tail idx arrays */
    struct iwlm_dma iml_dma;            /* IML image copy (BZ only) */

    struct iwlm_rxq rxq;
    struct iwlm_txq cmdq;               /* queue 0: host commands */
    struct iwlm_txq dataq;              /* dynamic queue from SCD cfg */
    struct iwlm_txq auxq;               /* aux sta queue (old sta api) */

    u16 seq_num;                        /* next command sequence */
    u32 alive;                          /* alive notification received */
    u32 init_alive;                     /* INIT vs regular alive */
    u32 sku_id[3];                      /* from alive ntf v5+ (pnvm match) */
    volatile u32 pnvm_done;             /* PNVM_INIT_COMPLETE_NTFY seen */

    /* pnvm payloads handed to the fw (fragmented: one DMA per chunk plus
     * the descriptor address array) */
    struct iwlm_dma pnvm_desc;
    struct iwlm_dma pnvm_data[IWLM_PNVM_MAX_CHUNKS];

    struct iwlm_hcmd_rsp wait_rsp;      /* single outstanding sync cmd */
    u8 wait_grp, wait_cmd;
};

/* ---------------- mvm / connection state ---------------- */

enum iwlm_state {
    IWLM_STATE_DOWN = 0,
    IWLM_STATE_FW_LOADED,               /* alive + init commands done */
    IWLM_STATE_SCANNING,
    IWLM_STATE_AUTHENTICATING,
    IWLM_STATE_ASSOCIATING,
    IWLM_STATE_EAPOL,                   /* 4-way handshake in flight */
    IWLM_STATE_CONNECTED,
};

struct iwlm_ap_info {
    u8 bssid[6];
    u8 channel;
    u8 band;                            /* 0 = 2.4G, 1 = 5G */
    s8 rssi;
    u16 capab;
    u16 bi;                             /* beacon interval (TU) */
    u8 dtim_period;
    u8 ssid[33];
    u8 ssid_len;
    u8 rsn_akm;                         /* 0=open 2=psk */
    u8 rsn_pairwise;                    /* cipher suite (4=ccmp) */
    bool has_rsn;
    u8 rsn_ie[66];                      /* raw RSN IE incl. id/len */
    u8 rsn_ie_len;
};

#define IWLM_MAX_APS 32

struct iwlm_dev {
    const struct iwlm_dev_cfg *cfg;
    u8 bus, slot, fn;
    volatile u8 *mmio;
    u32 mmio_size;
    u32 hw_rev;
    u32 hw_rf_id;

    struct iwlm_fw fw;
    struct iwlm_trans trans;

    /* mvm-ish state */
    enum iwlm_state state;
    u8 mac[6];

    /* phy contexts: [0] = 2.4G, [1] = 5G */
    u16 phy_ctxt_id[2];
    u8  phy_ctxt_channel[2];

    /* active connection */
    struct iwlm_ap_info cur_ap;
    u8  assoc_ies[512];
    u32 assoc_ies_len;
    u16 aid;
    u8  ap_sta_id;
    u8  aux_sta_id;
    u8  mac_id;                         /* bss mac context id */
    bool pairwise_key_set;

    /* scan results */
    struct iwlm_ap_info aps[IWLM_MAX_APS];
    int n_aps;
    bool scan_done;

    /* WPA */
    u8 psk_pass[65];                    /* from network.json */
    u8 pmk[32];
    u8 ptk[64];
    u8 gtk[16];
    u8 anonce[32], snonce[32];
    u8 replay_ctr[8];
    int eapol_msg;                      /* expected handshake step */
    bool have_ptk, have_gtk;

    /* rx ethernet frames for netd (filled by data path) */
    void *rx_priv;

    /* cmd worker (async dev.cmd) */
    int pending_cmd;
    char cmd_ssid[33];
    char cmd_pass[65];
    int cmd_result;
};

extern struct iwlm_dev *iwlm;

/* ---------------- CSR/prph access (pcie.c) ---------------- */

int  iwlm_pcie_probe(struct iwlm_dev *d);
void iwlm_write32(struct iwlm_dev *d, u32 ofs, u32 val);
u32  iwlm_read32(struct iwlm_dev *d, u32 ofs);
void iwlm_set_bit(struct iwlm_dev *d, u32 ofs, u32 bits);
void iwlm_clear_bit(struct iwlm_dev *d, u32 ofs, u32 bits);
int  iwlm_poll_bit(struct iwlm_dev *d, u32 ofs, u32 bits, u32 expected, int ms);
void iwlm_write_prph(struct iwlm_dev *d, u32 ofs, u32 val);
u32  iwlm_read_prph(struct iwlm_dev *d, u32 ofs);
void iwlm_write_prph64(struct iwlm_dev *d, u32 ofs, u64 val);
int  iwlm_sw_reset(struct iwlm_dev *d);
int  iwlm_prepare_card_hw(struct iwlm_dev *d);
int  iwlm_apm_init(struct iwlm_dev *d);
u32  iwlm_grab_nic_access(struct iwlm_dev *d);  /* 0 ok */
void iwlm_release_nic_access(struct iwlm_dev *d);

/* ---------------- firmware file (fwfile.c) ---------------- */

int  iwlm_fw_load(struct iwlm_dev *d, const char *path);
int  iwlm_fw_find_and_load(struct iwlm_dev *d, const char *dir);
int  iwlm_fw_pnvm_handshake(struct iwlm_dev *d); /* parse+load+kick+wait */
void iwlm_fw_free(struct iwlm_dev *d);
#define IWL_UCODE_TLV_CAPA_UMAC_SCAN   2
#define IWL_UCODE_TLV_CAPA_DQA_SUPPORT 12
#define IWL_UCODE_TLV_CAPA_FRAGMENTED_PNVM_IMG 32
#define IWL_UCODE_TLV_CAPA_BINDING_CDB_SUPPORT 39
#define IWL_UCODE_TLV_CAPA_CDB_SUPPORT 40
#define IWL_UCODE_TLV_CAPA_MLD_API_SUPPORT 110

static inline bool iwlm_fw_has_capa(const struct iwlm_dev *d, int capa)
{
    if (capa < 0 || capa >= 256)
        return false;
    return d->fw.capa[capa >> 5] & BIT(capa & 31);
}

static inline bool iwlm_fw_has_api(const struct iwlm_dev *d, int api)
{
    if (api < 0 || api >= 256)
        return false;
    return d->fw.api[api >> 5] & BIT(api & 31);
}

/* fw API change bits (fw/file.h enum iwl_ucode_tlv_api) */
#define IWL_UCODE_TLV_API_STA_TYPE             30
#define IWL_UCODE_TLV_API_ADAPTIVE_DWELL       32
#define IWL_UCODE_TLV_API_ADAPTIVE_DWELL_V2    42
#define IWL_UCODE_TLV_API_REDUCED_SCAN_CONFIG  56
#define IWL_UCODE_TLV_API_ADWELL_HB_DEF_N_AP   57
#define IWL_UCODE_TLV_API_SCAN_EXT_CHAN_VER    58

/* antennas from the PHY_SKU tlv */
static inline u8 iwlm_valid_tx_ant(const struct iwlm_dev *d)
{
    return (d->fw.phy_config >> 16) & 0xf;
}

static inline u8 iwlm_valid_rx_ant(const struct iwlm_dev *d)
{
    return (d->fw.phy_config >> 20) & 0xf;
}

/* mvm.h iwl_mvm_has_new_station_api(): MLD capa || ADD_STA cmd ver >= 12 */
static inline bool iwlm_has_new_station_api(const struct iwlm_dev *d)
{
    return iwlm_fw_has_capa(d, IWL_UCODE_TLV_CAPA_MLD_API_SUPPORT) ||
           iwlm_fw_cmd_ver(&d->fw, 0, 0x18) >= 12;
}

/* ---------------- transport (trans.c) ---------------- */

int  iwlm_trans_start_fw(struct iwlm_dev *d);   /* boot + wait alive */
int  iwlm_trans_load_pnvm(struct iwlm_dev *d);  /* DMA payloads + scratch */
void iwlm_trans_stop(struct iwlm_dev *d);
int  iwlm_trans_send_cmd(struct iwlm_dev *d, u8 grp, u8 cmd, u8 ver,
                         const void *payload, u32 payload_len,
                         void *rsp_buf, u32 *rsp_len, int timeout_ms);
int  iwlm_trans_send_cmd_pdu(struct iwlm_dev *d, u8 grp, u8 cmd,
                             const void *payload, u32 payload_len);
int  iwlm_trans_txq_alloc(struct iwlm_dev *d, u32 sta_mask, u8 tid, u16 size);
int  iwlm_trans_txq_alloc_aux(struct iwlm_dev *d, u32 sta_mask, u8 tid,
                              u16 size);
void iwlm_trans_rx_poll(struct iwlm_dev *d);    /* reap RX ring */
int  iwlm_trans_tx_frame(struct iwlm_dev *d, const void *hdr, u32 hdrlen,
                         const void *body, u32 bodylen, bool encrypted);
void iwlm_trans_tx_reclaim(struct iwlm_dev *d, u16 qid, u16 ssn);
void iwlm_trans_tx_reap(struct iwlm_dev *d);

/* rx notification handler (mvm/mvm.c), called by the transport */
void iwlm_mvm_rx_pkt(struct iwlm_dev *d, const struct iwl_rx_packet *pkt);

/* ---------------- mvm (mvm/) ---------------- */

int  iwlm_mvm_up(struct iwlm_dev *d);           /* post-alive init cmds */
int  iwlm_mvm_add_aux_sta(struct iwlm_dev *d);

/* scan (mvm/scan.c) */
int  iwlm_scan_config_send(struct iwlm_dev *d); /* SCAN_CFG at mvm_up */
int  iwlm_mvm_scan_start(struct iwlm_dev *d);
void iwlm_scan_rx_frame(struct iwlm_dev *d, const u8 *frame, u32 len, s8 rssi);

/* mlme (mvm/mlme.c) */
int  iwlm_mvm_connect(struct iwlm_dev *d, const char *ssid, const char *pass);
void iwlm_mvm_disconnect(struct iwlm_dev *d);
void iwlm_mlme_rx_mgmt(struct iwlm_dev *d, const u8 *frame, u32 len);
void iwlm_mlme_rx_eapol(struct iwlm_dev *d, const u8 *payload, u32 len);

/* data path (mvm/data.c) */
int  iwlm_data_init(struct iwlm_dev *d);
void iwlm_mvm_rx_mpdu(struct iwlm_dev *d, const u8 *data, u32 len);
int  iwlm_data_tx(struct iwlm_dev *d, const void *eth, u32 len);
int  iwlm_data_rx_pending(void);
int  iwlm_data_rx_pop(u8 *buf, int size);

#endif
