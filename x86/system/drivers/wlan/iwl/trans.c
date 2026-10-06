/*
 * trans.c - gen3 (ctxt-info) transport for AX210/BZ family NICs.
 *
 * Boots the firmware via the self-load DRAM mechanism, owns the host
 * command queue (queue 0, MTR), one dynamic data queue, and the MQ RX
 * ring. Everything is polled: the RX ring is reaped by comparing the
 * device-maintained le16 write index against our read index; command
 * responses are matched by (group, cmd, sequence).
 *
 * Sequences condensed from Linux iwlwifi pcie/trans-gen2.c,
 * pcie/ctxt-info-gen3.c, pcie/tx-gen2.c and pcie/rx.c.
 */
#include "../iwlm.h"
#include "../utils/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

#define PAGE_4K 4096

#define RX_TD_SIZE  sizeof(struct iwl_rx_transfer_desc)  /* 16 */
#define RX_CD_SIZE(fam) ((fam) == IWLM_FAM_BZ ? 4 : 32)

struct iwl_rx_transfer_desc {
    __le16 rbid;
    __le16 reserved[3];
    __le64 addr;
} __packed;

/* command/data queue per-entry buffer */
#define TXQ_ENTRY_BUFSZ 4096

/* ---------------- dma helpers ---------------- */

static int dma_get(struct iwlm_dma *m, u32 size)
{
    m->size = (size + PAGE_4K - 1) & ~(PAGE_4K - 1);
    m->va = dma_alloc(0, m->size);
    if (!m->va)
        return -1;
    m->pa = dma_phy_addr(0, m->va);
    memset((void *)m->va, 0, m->size);
    return 0;
}

static void dma_put(struct iwlm_dma *m)
{
    if (m->va)
        dma_free(0, m->va);
    m->va = m->pa = m->size = 0;
}

/* ---------------- rx queue setup ---------------- */

static int rxq_init(struct iwlm_dev *d)
{
    struct iwlm_rxq *q = &d->trans.rxq;
    u32 cd_size = RX_CD_SIZE(d->cfg->family);
    int i;

    if (!q->free_td.va) {
        if (dma_get(&q->free_td, IWLM_NUM_RBDS * RX_TD_SIZE) ||
            dma_get(&q->used_cd, IWLM_NUM_RBDS * cd_size) ||
            dma_get(&q->stts, PAGE_4K) ||
            dma_get(&q->bufs, IWLM_RX_POOL * IWLM_RX_BUF_SIZE))
            return -1;
    }
    memset((void *)q->free_td.va, 0, IWLM_NUM_RBDS * RX_TD_SIZE);
    memset((void *)q->used_cd.va, 0, IWLM_NUM_RBDS * cd_size);
    memset((void *)q->stts.va, 0, PAGE_4K);
    memset(q->rb_used, 0, sizeof(q->rb_used));
    q->read = 0;
    q->write = 0;

    /* hand every pool buffer to the NIC: rbid is 1-based */
    for (i = 0; i < IWLM_RX_POOL; i++) {
        struct iwl_rx_transfer_desc *td =
            (void *)(q->free_td.va + i * RX_TD_SIZE);
        td->rbid = cpu_to_le16(i + 1);
        td->addr = cpu_to_le64(q->bufs.pa + (u64)i * IWLM_RX_BUF_SIZE);
        q->rb_used[i >> 3] |= BIT(i & 7);
    }
    q->write = IWLM_RX_POOL;  /* wraps to 0 at 512 entries */
    return 0;
}

/* return a used buffer to the free ring */
static void rxq_restock_one(struct iwlm_dev *d, u16 rbid)
{
    struct iwlm_rxq *q = &d->trans.rxq;
    u16 w = q->write & (IWLM_NUM_RBDS - 1);
    struct iwl_rx_transfer_desc *td =
        (void *)(q->free_td.va + w * RX_TD_SIZE);

    td->rbid = cpu_to_le16(rbid);
    td->addr = cpu_to_le64(q->bufs.pa +
                           (u64)(rbid - 1) * IWLM_RX_BUF_SIZE);
    q->rb_used[(rbid - 1) >> 3] |= BIT((rbid - 1) & 7);
    q->write = (q->write + 1) & (IWLM_NUM_RBDS - 1);

    /* shadow write-index register, triggered in multiples of 8 */
    if (!(q->write & 7))
        iwlm_write32(d, RFH_Q_FRBDCB_WIDX_TRG(0), q->write);
}

/* ---------------- tx queue helpers ---------------- */

static int txq_init(struct iwlm_dev *d, struct iwlm_txq *q, u16 id, u16 size)
{
    if (!q->tfd.va) {
        if (dma_get(&q->tfd, (u32)size * sizeof(struct iwl_tfh_tfd)) ||
            dma_get(&q->bc, (u32)size * 2 + 256) ||
            dma_get(&q->bufs, (u32)size * TXQ_ENTRY_BUFSZ))
            return -1;
    }
    memset((void *)q->tfd.va, 0, (u32)size * sizeof(struct iwl_tfh_tfd));
    memset((void *)q->bc.va, 0, (u32)size * 2);
    q->write_ptr = q->read_ptr = 0;
    q->size = size;
    q->id = id;
    q->used = true;
    return 0;
}

static inline u16 txq_index(const struct iwlm_txq *q, u16 ptr)
{
    return ptr & (q->size - 1);
}

/* add a TB to a TFD; returns tb index or -1 */
static int tfd_add_tb(struct iwl_tfh_tfd *tfd, u64 pa, u16 len)
{
    u16 n = le16_to_cpu(tfd->num_tbs) & 0x1f;

    if (n >= IWL_TFH_NUM_TBS)
        return -1;
    put_unaligned_le64(pa, &tfd->tbs[n].addr);
    tfd->tbs[n].tb_len = cpu_to_le16(len);
    tfd->num_tbs = cpu_to_le16(n + 1);
    return n;
}

/* gen2 byte-count table entry (bc table is used on data queues only) */
static void txq_update_bc(struct iwlm_dev *d, struct iwlm_txq *q,
                          u16 idx, u16 byte_cnt, u8 num_tbs)
{
    u32 filled = offsetof(struct iwl_tfh_tfd, tbs) +
                 num_tbs * sizeof(struct iwl_tfh_tb);
    u16 chunks = (u16)((filled + 63) / 64) - 1;
    __le16 *bc = (__le16 *)q->bc.va;

    bc[idx] = cpu_to_le16(byte_cnt | (chunks << 14));
    (void)d;
}

static void txq_doorbell(struct iwlm_dev *d, struct iwlm_txq *q)
{
    iwlm_write32(d, HBUS_TARG_WRPTR, q->write_ptr | ((u32)q->id << 16));
}

/* ---------------- context info gen3 ---------------- */

/* copy one fw section to its own DMA page-set, record 64-bit address */
static int fw_sec_dma(struct iwlm_dev *d, const struct iwlm_fw_sec *s,
                      struct iwlm_dma *m, __le64 *out)
{
    if (dma_get(m, s->size))
        return -1;
    memcpy((void *)m->va, s->data, s->size);
    *out = cpu_to_le64(m->pa);
    return 0;
}

/*
 * Firmware sections must stay alive while the device runs: the ROM reads
 * lmac/umac at boot, the firmware DMAs paging sections on demand.
 */
#define IWLM_MAX_SEC_DMA (2 * IWL_MAX_DRAM_ENTRY)

static struct iwlm_dma sec_dma[IWLM_MAX_SEC_DMA];
static int sec_dma_cnt;

static int ctxt_info_init(struct iwlm_dev *d)
{
    struct iwlm_trans *t = &d->trans;
    struct iwlm_fw *fw = &d->fw;
    struct iwl_context_info_gen3 *ci;
    struct iwl_prph_scratch *ps;
    int i, phase, li = 0, ui = 0, vi = 0;

    if (!t->ctxt_info.va &&
        (dma_get(&t->ctxt_info, PAGE_4K) ||
         dma_get(&t->prph_scratch, PAGE_4K) ||
         dma_get(&t->prph_info, PAGE_4K)))
        return -1;

    ps = (void *)t->prph_scratch.va;
    memset((void *)ps, 0, PAGE_4K);
    ps->ctrl_cfg.version.version = 0;
    ps->ctrl_cfg.version.mac_id = cpu_to_le16((u16)d->hw_rev);
    ps->ctrl_cfg.version.size = cpu_to_le16(sizeof(*ps) / 4);
    ps->ctrl_cfg.control.control_flags =
        cpu_to_le32(IWL_PRPH_SCRATCH_RB_SIZE_4K |
                    IWL_PRPH_SCRATCH_MTR_MODE |
                    IWL_PRPH_MTR_FORMAT_256B);
    ps->ctrl_cfg.rbd_cfg.free_rbd_addr = cpu_to_le64(t->rxq.free_td.pa);

    /* sections: lmac, [sep], umac, [sep], paging */
    phase = 0;
    sec_dma_cnt = 0;
    for (i = 0; i < fw->n_sec; i++) {
        const struct iwlm_fw_sec *s = &fw->sec[i];
        __le64 *slot = NULL;

        if (s->offset == 0xFFFFCCCC) { phase = 1; continue; }
        if (s->offset == 0xAAAABBBB) { phase = 2; continue; }
        if (sec_dma_cnt >= IWLM_MAX_SEC_DMA)
            return -2;

        if (phase == 0 && li < IWL_MAX_DRAM_ENTRY)
            slot = &ps->dram.lmac_img[li++];
        else if (phase == 1 && ui < IWL_MAX_DRAM_ENTRY)
            slot = &ps->dram.umac_img[ui++];
        else if (phase == 2 && vi < IWL_MAX_DRAM_ENTRY)
            slot = &ps->dram.virtual_img[vi++];
        if (!slot)
            return -3;
        if (fw_sec_dma(d, s, &sec_dma[sec_dma_cnt++], slot))
            return -4;
    }

    /* IML: required by the ROM boot flow */
    if (fw->iml && fw->iml_size) {
        if (!t->iml_dma.va && dma_get(&t->iml_dma, fw->iml_size))
            return -5;
        memcpy((void *)t->iml_dma.va, fw->iml, fw->iml_size);
    }

    ci = (void *)t->ctxt_info.va;
    memset((void *)ci, 0, sizeof(*ci));
    ci->prph_info_base_addr = cpu_to_le64(t->prph_info.pa);
    ci->prph_scratch_base_addr = cpu_to_le64(t->prph_scratch.pa);
    ci->prph_scratch_size = cpu_to_le32(sizeof(*ps));
    ci->cr_head_idx_arr_base_addr = cpu_to_le64(t->rxq.stts.pa);
    ci->tr_tail_idx_arr_base_addr =
        cpu_to_le64(t->prph_info.pa + PAGE_4K / 2);
    ci->cr_tail_idx_arr_base_addr =
        cpu_to_le64(t->prph_info.pa + 3 * PAGE_4K / 4);
    ci->mtr_base_addr = cpu_to_le64(t->cmdq.tfd.pa);
    ci->mcr_base_addr = cpu_to_le64(t->rxq.used_cd.pa);
    ci->mtr_size = cpu_to_le16(TFD_QUEUE_CB_SIZE(t->cmdq.size));
    ci->mcr_size = cpu_to_le16(RX_QUEUE_CB_SIZE(IWLM_NUM_RBDS));

    /* kick the ROM self-load */
    iwlm_write32(d, CSR_CTXT_INFO_ADDR, (u32)t->ctxt_info.pa);
    iwlm_write32(d, CSR_CTXT_INFO_ADDR + 4, (u32)(t->ctxt_info.pa >> 32));
    if (t->iml_dma.va) {
        iwlm_write32(d, CSR_IML_DATA_ADDR, (u32)t->iml_dma.pa);
        iwlm_write32(d, CSR_IML_DATA_ADDR + 4, (u32)(t->iml_dma.pa >> 32));
        iwlm_write32(d, CSR_IML_SIZE_ADDR, fw->iml_size);
    }
    iwlm_set_bit(d, CSR_CTXT_INFO_BOOT_CTRL, CSR_AUTO_FUNC_BOOT_ENA);
    return 0;
}

/*
 * PNVM payload handover (ctxt-info-gen3.c load_pnvm/set_pnvm). Runs
 * after the alive notification: the payloads live in host DRAM, their
 * addresses are published through prph_scratch->ctrl_cfg.pnvm_cfg, then
 * the fw is poked with the pnvm doorbell. Both target firmwares set
 * FRAGMENTED_PNVM_IMG: one DMA region per chunk plus a le64 address
 * array; the continuous two-chunk layout is kept as fallback.
 */
#define PNVM_DESC_ENTRIES 64            /* IPC_DRAM_MAP_ENTRY_NUM_MAX */

int iwlm_trans_load_pnvm(struct iwlm_dev *d)
{
    struct iwlm_trans *t = &d->trans;
    struct iwlm_fw *fw = &d->fw;
    struct iwl_prph_scratch *ps = (void *)t->prph_scratch.va;
    u32 total = 0;
    int i;

    if (fw->pnvm_n_chunks <= 0 || fw->pnvm_n_chunks > IWLM_PNVM_MAX_CHUNKS)
        return -1;
    for (i = 0; i < fw->pnvm_n_chunks; i++)
        total += fw->pnvm_chunk_len[i];

    if (iwlm_fw_has_capa(d, IWL_UCODE_TLV_CAPA_FRAGMENTED_PNVM_IMG)) {
        __le64 *desc;

        if (!t->pnvm_desc.va &&
            dma_get(&t->pnvm_desc, PNVM_DESC_ENTRIES * 8))
            return -2;
        desc = (void *)t->pnvm_desc.va;
        memset(desc, 0, PNVM_DESC_ENTRIES * 8);
        for (i = 0; i < fw->pnvm_n_chunks; i++) {
            if (!t->pnvm_data[i].va &&
                dma_get(&t->pnvm_data[i], fw->pnvm_chunk_len[i]))
                return -3;
            memcpy((void *)t->pnvm_data[i].va, fw->pnvm_chunk[i],
                   fw->pnvm_chunk_len[i]);
            desc[i] = cpu_to_le64(t->pnvm_data[i].pa);
        }
        ps->ctrl_cfg.pnvm_cfg.pnvm_base_addr = cpu_to_le64(t->pnvm_desc.pa);
        ps->ctrl_cfg.pnvm_cfg.pnvm_size = cpu_to_le32(total);
    } else {
        /* unfragmented: two payloads concatenated in one region */
        if (fw->pnvm_n_chunks != 2)
            return -4;
        if (!t->pnvm_data[0].va && dma_get(&t->pnvm_data[0], total))
            return -5;
        memcpy((void *)t->pnvm_data[0].va, fw->pnvm_chunk[0],
               fw->pnvm_chunk_len[0]);
        memcpy((void *)(t->pnvm_data[0].va + fw->pnvm_chunk_len[0]),
               fw->pnvm_chunk[1], fw->pnvm_chunk_len[1]);
        ps->ctrl_cfg.pnvm_cfg.pnvm_base_addr =
            cpu_to_le64(t->pnvm_data[0].pa);
        ps->ctrl_cfg.pnvm_cfg.pnvm_size = cpu_to_le32(total);
    }
    klog("iwlm: pnvm ver %08x, %d chunks, %u bytes\n",
         fw->pnvm_version, fw->pnvm_n_chunks, total);
    return 0;
}

/* ---------------- firmware boot ---------------- */

int iwlm_trans_start_fw(struct iwlm_dev *d)
{
    struct iwlm_trans *t = &d->trans;
    int ret, timeout;

    ret = iwlm_prepare_card_hw(d);
    if (ret) {
        klog("iwlm: prepare_card_hw failed\n");
        return ret;
    }

    /* ack everything, keep interrupts disabled (we poll) */
    iwlm_write32(d, CSR_INT, 0xFFFFFFFF);
    iwlm_write32(d, CSR_INT_MASK, 0);

    /* clear rfkill handshake + cmd-blocked bits */
    iwlm_write32(d, CSR_UCODE_DRV_GP1_CLR, CSR_UCODE_SW_BIT_RFKILL);
    iwlm_write32(d, CSR_UCODE_DRV_GP1_CLR, CSR_UCODE_DRV_GP1_BIT_CMD_BLOCKED);
    iwlm_write32(d, CSR_INT, 0xFFFFFFFF);

    /* gen2 nic init */
    ret = iwlm_apm_init(d);
    if (ret) {
        klog("iwlm: apm init failed\n");
        return ret;
    }
    if (rxq_init(d) || txq_init(d, &t->cmdq, IWLM_CMDQ_ID, IWLM_CMDQ_SIZE))
        return -1;

    /* enable shadow regs in HW */
    iwlm_set_bit(d, CSR_MAC_SHADOW_REG_CTRL, 0x800FFFFF);

    ret = ctxt_info_init(d);
    if (ret) {
        klog("iwlm: ctxt info failed %d\n", ret);
        return ret;
    }

    if (d->cfg->family == IWLM_FAM_BZ) {
        iwlm_write32(d, CSR_FUNC_SCRATCH, CSR_FUNC_SCRATCH_INIT_VALUE);
        iwlm_set_bit(d, CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_ROM_START);
    } else {
        /* AX210 family: release the UMAC cpu */
        if (iwlm_grab_nic_access(d))
            return -1;
        iwlm_write32(d, HBUS_TARG_PRPH_WADDR,
                     ((UREG_CPU_INIT_RUN + 0x300000) & 0x00FFFFFF) |
                     (3 << 24));
        iwlm_write32(d, HBUS_TARG_PRPH_WDAT, 1);
        iwlm_release_nic_access(d);
    }

    /* wait for ALIVE (arrives as the first RX packet) */
    t->alive = 0;
    for (timeout = 0; timeout < 3000; timeout++) {
        iwlm_trans_rx_poll(d);
        if (t->alive)
            return 0;
        usleep(1000);
    }
    klog("iwlm: timeout waiting for alive\n");
    return -2;
}

void iwlm_trans_stop(struct iwlm_dev *d)
{
    /* stop DMA, reset chip; DMA memory stays allocated for the next run */
    iwlm_set_bit(d, CSR_RESET, CSR_RESET_REG_FLAG_STOP_MASTER);
    iwlm_poll_bit(d, CSR_RESET, CSR_RESET_REG_FLAG_MASTER_DISABLED,
                  CSR_RESET_REG_FLAG_MASTER_DISABLED, 1000);
    iwlm_sw_reset(d);
    iwlm_prepare_card_hw(d);
}

/* ---------------- host commands ---------------- */

/* build one TFD entry for a host command and ring the doorbell */
static int hcmd_enqueue(struct iwlm_dev *d, u8 grp, u8 cmd, u8 ver,
                        const void *payload, u32 len, u16 *out_seq)
{
    struct iwlm_trans *t = &d->trans;
    struct iwlm_txq *q = &t->cmdq;
    u16 idx = txq_index(q, q->write_ptr);
    struct iwl_tfh_tfd *tfd =
        (void *)(q->tfd.va + idx * sizeof(struct iwl_tfh_tfd));
    u8 *buf = (u8 *)q->bufs.va + (u32)idx * TXQ_ENTRY_BUFSZ;
    u64 buf_pa = q->bufs.pa + (u32)idx * TXQ_ENTRY_BUFSZ;
    u32 total = 8 + len;          /* wide header + payload */
    u16 seq;
    u16 free;

    free = (q->read_ptr - q->write_ptr - 1) & (q->size - 1);
    if (free < 2)
        return -1;

    if (total > TXQ_ENTRY_BUFSZ)
        return -2;

    memset(tfd, 0, sizeof(*tfd));

    /* wide command header */
    buf[0] = cmd;
    buf[1] = grp;
    seq = ((u16)q->id << SEQ_QUEUE_SHIFT) | (idx << SEQ_INDEX_SHIFT);
    put_unaligned_le16(seq, buf + 2);
    put_unaligned_le16(len, buf + 4);
    buf[6] = 0;
    buf[7] = ver;
    if (len)
        memcpy(buf + 8, payload, len);

    /* TB0: first 20 bytes from the 64B-aligned mirror area */
    memcpy(buf + TXQ_ENTRY_BUFSZ - 64, buf, 20 < total ? 20 : total);
    tfd_add_tb(tfd, buf_pa + TXQ_ENTRY_BUFSZ - 64,
               total < 20 ? total : 20);
    if (total > 20)
        tfd_add_tb(tfd, buf_pa + 20, total - 20);

    q->write_ptr = (q->write_ptr + 1) & (q->size - 1);
    txq_doorbell(d, q);
    if (out_seq)
        *out_seq = seq;
    return 0;
}

/* wait for the response to (grp, cmd); response copied to caller buf */
int iwlm_trans_send_cmd(struct iwlm_dev *d, u8 grp, u8 cmd, u8 ver,
                        const void *payload, u32 payload_len,
                        void *rsp_buf, u32 *rsp_len, int timeout_ms)
{
    struct iwlm_trans *t = &d->trans;
    int ret, waited;
    u32 want = rsp_len ? *rsp_len : 0;

    t->wait_rsp.done = 0;
    t->wait_rsp.status = -1;
    t->wait_rsp.buf = rsp_buf;
    t->wait_rsp.len = want;
    t->wait_grp = grp;
    t->wait_cmd = cmd;

    ret = hcmd_enqueue(d, grp, cmd, ver, payload, payload_len, NULL);
    if (ret)
        return -1;

    for (waited = 0; waited < timeout_ms * 10; waited++) {
        iwlm_trans_rx_poll(d);
        if (t->wait_rsp.done)
            return t->wait_rsp.status;
        usleep(100);
    }
    t->wait_grp = 0xFF;   /* stop matching */
    return -2;
}

/*
 * Linux sends commands as a bare WIDE_ID(group, cmd): the version field
 * of the wide header is 0 for everything except the few commands wrapped
 * with iwl_cmd_id() (RLC_CONFIG_CMD = v1). The TLV cmd_versions table is
 * only consulted host-side to pick the payload struct layout; the fw
 * itself versions by payload length. Mirror that: header version 0 here,
 * explicit versions only where Linux has them.
 */
int iwlm_trans_send_cmd_pdu(struct iwlm_dev *d, u8 grp, u8 cmd,
                            const void *payload, u32 payload_len)
{
    return iwlm_trans_send_cmd(d, grp, cmd, 0,
                               payload, payload_len, NULL, NULL,
                               IWLM_HCMD_TIMEOUT_MS);
}

/* ---------------- TX data frames ---------------- */

/*
 * Send one TX_CMD (gen3, version from fw table) on the data queue.
 * hdr = 802.11 header (+qos ctrl), body = llc/snap payload (plaintext;
 * CCMP is applied by the firmware once keys are set).
 *
 * Called from both the vdevice IPC thread (net_write data frames) and
 * the worker thread (mgmt/eapol); the lock serializes the ring update.
 */
static pthread_mutex_t tx_lock = PTHREAD_MUTEX_INITIALIZER;

int iwlm_trans_tx_frame(struct iwlm_dev *d, const void *hdr, u32 hdrlen,
                        const void *body, u32 bodylen, bool encrypted)
{
    struct iwlm_trans *t = &d->trans;
    struct iwlm_txq *q = &t->dataq;
    u16 idx, seq, free;
    struct iwl_tfh_tfd *tfd;
    u8 *buf;
    u64 buf_pa;
    u32 framelen = hdrlen + bodylen;
    u32 cmdlen, total;
    u8 ver;
    int ret = 0;

    pthread_mutex_lock(&tx_lock);
    if (!q->used) {
        ret = -1;
        goto out;
    }

    free = (q->read_ptr - q->write_ptr - 1) & (q->size - 1);
    if (free < 2) {
        ret = -2;
        goto out;
    }

    /* gen3 tx cmd: {le16 len; le16 flags; le32 offload;
     *               dram_info(8); le32 rate_n_flags; u8 rsvd[8]; hdr[]}
     *              = 28 bytes before hdr */
    cmdlen = 28 + framelen;
    total = 4 + cmdlen;           /* legacy cmd header (group 0) + payload */

    idx = txq_index(q, q->write_ptr);
    tfd = (void *)(q->tfd.va + idx * sizeof(struct iwl_tfh_tfd));
    buf = (u8 *)q->bufs.va + (u32)idx * TXQ_ENTRY_BUFSZ;
    buf_pa = q->bufs.pa + (u32)idx * TXQ_ENTRY_BUFSZ;

    if (total > TXQ_ENTRY_BUFSZ - 64) {
        ret = -3;
        goto out;
    }

    memset(tfd, 0, sizeof(*tfd));

    buf[0] = 0x1c;                /* TX_CMD */
    buf[1] = 0;                   /* legacy group */
    seq = ((u16)q->id << SEQ_QUEUE_SHIFT) | (idx << SEQ_INDEX_SHIFT);
    put_unaligned_le16(seq, buf + 2);

    put_unaligned_le16(framelen, buf + 4);          /* cmd.len */
    put_unaligned_le16(encrypted ? 0 : 2, buf + 6); /* flags: 2=ENCRYPT_DIS */
    put_unaligned_le32(0, buf + 8);                 /* offload_assist */
    memset(buf + 12, 0, 8);                         /* dram_info */
    put_unaligned_le32(0, buf + 20);                /* rate_n_flags: fw TLC */
    memset(buf + 24, 0, 8);
    memcpy(buf + 32, hdr, hdrlen);
    if (bodylen)
        memcpy(buf + 32 + hdrlen, body, bodylen);

    ver = iwlm_fw_cmd_ver(&d->fw, 0, 0x1c);
    (void)ver;   /* TX_CMD payload versioning is implicit in gen3 layout */

    memcpy(buf + TXQ_ENTRY_BUFSZ - 64, buf, 20);
    tfd_add_tb(tfd, buf_pa + TXQ_ENTRY_BUFSZ - 64, 20);
    tfd_add_tb(tfd, buf_pa + 20, total - 20);

    txq_update_bc(d, q, idx, framelen, 2);

    q->write_ptr = (q->write_ptr + 1) & (q->size - 1);
    txq_doorbell(d, q);
out:
    pthread_mutex_unlock(&tx_lock);
    return ret;
}

/* reclaim data queue entries up to ssn (not inclusive) */
void iwlm_trans_tx_reclaim(struct iwlm_dev *d, u16 qid, u16 ssn)
{
    struct iwlm_txq *q = &d->trans.dataq;

    if (!q->used || qid != q->id)
        return;
    q->read_ptr = ssn & (q->size - 1);
}

void iwlm_trans_tx_reap(struct iwlm_dev *d)
{
    (void)d;   /* reclaim is driven by TX_CMD responses on the RX ring */
}

/* ---------------- dynamic queue alloc (SCD_QUEUE_CONFIG_CMD v3) ---------------- */

struct iwlm_scd_queue_cfg_cmd {
    __le32 operation;
    __le32 sta_mask;
    u8 tid;
    u8 reserved[3];
    __le32 flags;
    __le32 cb_size;
    __le64 bc_dram_addr;
    __le64 tfdq_dram_addr;
} __packed;

struct iwlm_tx_queue_cfg_rsp {
    __le16 queue_number;
    __le16 flags;
    __le16 write_pointer;
    __le16 reserved;
} __packed;

static int txq_alloc(struct iwlm_dev *d, struct iwlm_txq *q,
                     u32 sta_mask, u8 tid, u16 size)
{
    struct iwlm_scd_queue_cfg_cmd cmd;
    struct iwlm_tx_queue_cfg_rsp rsp;
    u32 rsp_len = sizeof(rsp);
    int ret;
    u8 ver = iwlm_fw_cmd_ver(&d->fw, 5, 0x17);  /* DATA_PATH, SCD_QUEUE_CONFIG */

    if (!q->tfd.va &&
        txq_init(d, q, 0, size))
        return -1;

    memset(&cmd, 0, sizeof(cmd));
    if (ver == 3) {
        cmd.operation = cpu_to_le32(0);          /* IWL_SCD_QUEUE_ADD */
        cmd.sta_mask = cpu_to_le32(sta_mask);
        cmd.tid = tid;
        cmd.flags = 0;
        cmd.cb_size = cpu_to_le32(TFD_QUEUE_CB_SIZE(size));
        cmd.bc_dram_addr = cpu_to_le64(q->bc.pa);
        cmd.tfdq_dram_addr = cpu_to_le64(q->tfd.pa);
        ret = iwlm_trans_send_cmd(d, 5, 0x17, 3, &cmd, sizeof(cmd),
                                  &rsp, &rsp_len, IWLM_HCMD_TIMEOUT_MS);
    } else {
        /* legacy SCD_QUEUE_CFG (legacy group, 0x1d) */
        struct {
            u8 sta_id;
            u8 tid;
            __le16 flags;
            __le32 cb_size;
            __le64 byte_cnt_addr;
            __le64 tfdq_addr;
        } __packed old;
        int sta_id = 0;

        while (sta_mask >>= 1)
            sta_id++;
        memset(&old, 0, sizeof(old));
        old.sta_id = sta_id;
        old.tid = tid;
        old.flags = cpu_to_le16(1);              /* ENABLE_QUEUE */
        old.cb_size = cpu_to_le32(TFD_QUEUE_CB_SIZE(size));
        old.byte_cnt_addr = cpu_to_le64(q->bc.pa);
        old.tfdq_addr = cpu_to_le64(q->tfd.pa);
        ret = iwlm_trans_send_cmd(d, 0, 0x1d, 0, &old, sizeof(old),
                                  &rsp, &rsp_len, IWLM_HCMD_TIMEOUT_MS);
    }
    if (ret || rsp_len < sizeof(rsp))
        return -2;

    q->id = le16_to_cpu(rsp.queue_number);
    q->write_ptr = q->read_ptr =
        le16_to_cpu(rsp.write_pointer) & (size - 1);
    q->size = size;
    q->used = true;
    return q->id;
}

int iwlm_trans_txq_alloc(struct iwlm_dev *d, u32 sta_mask, u8 tid, u16 size)
{
    return txq_alloc(d, &d->trans.dataq, sta_mask, tid, size);
}

int iwlm_trans_txq_alloc_aux(struct iwlm_dev *d, u32 sta_mask, u8 tid,
                             u16 size)
{
    return txq_alloc(d, &d->trans.auxq, sta_mask, tid, size);
}

/* ---------------- RX polling ---------------- */

extern void iwlm_mvm_rx_pkt(struct iwlm_dev *d,
                            const struct iwl_rx_packet *pkt);

void iwlm_trans_rx_poll(struct iwlm_dev *d)
{
    struct iwlm_trans *t = &d->trans;
    struct iwlm_rxq *q = &t->rxq;
    u32 cd_size = RX_CD_SIZE(d->cfg->family);
    u16 r = *(volatile __le16 *)q->stts.va;
    u16 i = q->read;

    r &= (IWLM_NUM_RBDS - 1);
    while (i != r) {
        const u8 *cd = (const u8 *)q->used_cd.va + i * cd_size;
        u16 rbid = le16_to_cpu(get_unaligned_le16(cd));
        u8 flags = (d->cfg->family == IWLM_FAM_BZ) ? cd[2] : cd[6];
        u8 *rb;
        u32 offset = 0;

        if (!rbid || rbid > IWLM_RX_POOL)
            break;          /* device gave garbage: stop, retry later */
        rb = (u8 *)q->bufs.va + (u32)(rbid - 1) * IWLM_RX_BUF_SIZE;

        if (!(flags & 1)) { /* not fragmented: walk packed packets */
            while (offset + 8 < IWLM_RX_BUF_SIZE) {
                const struct iwl_rx_packet *pkt =
                    (const void *)(rb + offset);
                u32 len;

                if (pkt->len_n_flags ==
                    cpu_to_le32(FH_RSCSR_FRAME_INVALID))
                    break;
                len = iwl_rx_packet_len(pkt) + 4;
                if (len < 8 || offset + len > IWLM_RX_BUF_SIZE)
                    break;
                iwlm_mvm_rx_pkt(d, pkt);
                offset += (len + FH_RSCSR_FRAME_ALIGN - 1) &
                          ~(FH_RSCSR_FRAME_ALIGN - 1);
            }
        }
        /* fragmented frames (multi-RB) are dropped, restocked either way */
        rxq_restock_one(d, rbid);
        i = (i + 1) & (IWLM_NUM_RBDS - 1);
    }
    q->read = i;

    /* command queue entries are freed when their response arrives;
     * keep the read pointer chasing write so it never fills up */
    t->cmdq.read_ptr = t->cmdq.write_ptr;
}
