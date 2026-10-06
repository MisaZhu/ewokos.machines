/*
 * fwfile.c - Intel .ucode TLV parser (condensed from Linux iwl-drv.c).
 *
 * File layout: iwl_tlv_ucode_header followed by 4-byte aligned TLVs.
 * We extract: runtime sections (IWL_UCODE_TLV_SEC_RT / SECURE_SEC_RT),
 * cpu count, phy config (SKU), capabilities, api set, cmd versions,
 * scan channel count, num stations and the IML image.
 */
#include "../iwlm.h"
#include "../utils/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define IWL_TLV_UCODE_MAGIC     0x0a4c5749

struct iwl_tlv_ucode_header {
    __le32 zero;
    __le32 magic;
    u8 human_readable[64];
    __le32 ver;
    __le32 build;
    __le64 ignore;
    u8 data[];
} __packed;

struct iwl_ucode_tlv {
    __le32 type;
    __le32 length;      /* not including type/length */
    u8 data[];
} __packed;

enum {
    IWL_UCODE_TLV_SEC_RT              = 19,
    IWL_UCODE_TLV_SEC_INIT            = 20,
    IWL_UCODE_TLV_PHY_SKU             = 23,
    IWL_UCODE_TLV_SECURE_SEC_RT       = 24,
    IWL_UCODE_TLV_NUM_OF_CPU          = 27,
    IWL_UCODE_TLV_API_CHANGES_SET     = 29,
    IWL_UCODE_TLV_ENABLED_CAPABILITIES = 30,
    IWL_UCODE_TLV_N_SCAN_CHANNELS     = 31,
    IWL_UCODE_TLV_PAGING              = 32,
    IWL_UCODE_TLV_FW_VERSION          = 36,
    IWL_UCODE_TLV_CMD_VERSIONS        = 48,
    IWL_UCODE_TLV_IML                 = 52,
    IWL_UCODE_TLV_FW_NUM_STATIONS     = 0x100,
};

#define CPU1_CPU2_SEPARATOR_SECTION   0xFFFFCCCC
#define PAGING_SEPARATOR_SECTION      0xAAAABBBB

struct iwl_fw_cmd_version {
    u8 cmd;
    u8 group;
    u8 cmd_ver;
    u8 notif_ver;
} __packed;

/* API_CHANGES_SET / ENABLED_CAPABILITIES are arrays of
 * { __le32 api_index; __le32 api_flags; } pairs; bit i of each flags
 * word lands at bit (i + 32 * api_index) of the target bitset. */
static void store_bitset(u32 *dst, const u8 *src, u32 len)
{
    u32 j;
    for (j = 0; j + 8 <= len; j += 8) {
        u32 idx = get_unaligned_le32(src + j);
        u32 flags = get_unaligned_le32(src + j + 4);
        if (idx < 8)
            dst[idx] |= flags;
    }
}

static void set_cmd_ver(struct iwlm_fw *fw, u8 grp, u8 cmd, u8 ver)
{
    u32 id = ((u32)grp << 8) | cmd;
    fw->cmd_ver[id >> 3] |= BIT(id & 7);
    fw->cmd_vers[id] = ver;
}

int iwlm_fw_load(struct iwlm_dev *d, const char *path)
{
    FILE *f = fopen(path, "rb");
    struct iwlm_fw *fw = &d->fw;
    struct iwl_tlv_ucode_header *hdr;
    u8 *p, *end;
    bool have_rt = false;

    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    fw->file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    fw->file = malloc(fw->file_size);
    if (!fw->file || fread(fw->file, 1, fw->file_size, f) != fw->file_size) {
        fclose(f);
        return -2;
    }
    fclose(f);

    if (fw->file_size < sizeof(*hdr))
        return -3;
    hdr = (void *)fw->file;
    if (hdr->magic != cpu_to_le32(IWL_TLV_UCODE_MAGIC))
        return -4;
    memcpy(fw->ver_str, hdr->human_readable,
           fw->file_size > offsetof(struct iwl_tlv_ucode_header, ver)
               ? 63 : 0);
    fw->ver_str[63] = 0;

    fw->n_scan_channels = 52;       /* sane default if TLV missing */
    fw->num_stations = 32;

    p = hdr->data;
    end = fw->file + fw->file_size;
    while (p + 8 <= end) {
        const struct iwl_ucode_tlv *tlv = (const void *)p;
        u32 type = le32_to_cpu(tlv->type);
        u32 len = le32_to_cpu(tlv->length);
        const u8 *data = tlv->data;
        u32 adv = 8 + ((len + 3) & ~3u);

        if (p + adv > end + 3)
            break;
        if (p + 8 + len > end)
            break;

        switch (type) {
        case IWL_UCODE_TLV_SEC_RT:
        case IWL_UCODE_TLV_SECURE_SEC_RT:
            if (len >= 4 && fw->n_sec < IWLM_FW_MAX_SEC) {
                struct iwlm_fw_sec *s = &fw->sec[fw->n_sec];
                s->offset = get_unaligned_le32(data);
                s->data = data + 4;
                s->size = len - 4;
                fw->n_sec++;
                have_rt = true;
            }
            break;
        case IWL_UCODE_TLV_NUM_OF_CPU:
            if (len == 4 && get_unaligned_le32(data) == 2)
                fw->dual_cpus = true;
            break;
        case IWL_UCODE_TLV_PHY_SKU:
            if (len == 4)
                fw->phy_config = get_unaligned_le32(data);
            break;
        case IWL_UCODE_TLV_API_CHANGES_SET:
            store_bitset(fw->api, data, len);
            break;
        case IWL_UCODE_TLV_ENABLED_CAPABILITIES:
            store_bitset(fw->capa, data, len);
            break;
        case IWL_UCODE_TLV_N_SCAN_CHANNELS:
            if (len == 4)
                fw->n_scan_channels = get_unaligned_le32(data);
            break;
        case IWL_UCODE_TLV_FW_NUM_STATIONS:
            if (len == 4)
                fw->num_stations = get_unaligned_le32(data);
            break;
        case IWL_UCODE_TLV_CMD_VERSIONS: {
            u32 i, n = len / sizeof(struct iwl_fw_cmd_version);
            const struct iwl_fw_cmd_version *v = (const void *)data;
            for (i = 0; i < n; i++)
                set_cmd_ver(fw, v[i].group, v[i].cmd, v[i].cmd_ver);
            break;
        }
        case IWL_UCODE_TLV_IML:
            fw->iml = data;
            fw->iml_size = len;
            break;
        default:
            break;
        }
        p += adv;
    }

    if (!have_rt)
        return -5;

    /* count lmac/umac/paging sections for the ctxt-info dram map:
     * sections before the separator are LMAC (cpu1), after it UMAC (cpu2),
     * after the paging separator come the paging image */
    {
        int i, phase = 0;
        for (i = 0; i < fw->n_sec; i++) {
            u32 ofs = fw->sec[i].offset;
            if (ofs == CPU1_CPU2_SEPARATOR_SECTION) {
                phase = 1;
                continue;
            }
            if (ofs == PAGING_SEPARATOR_SECTION) {
                phase = 2;
                continue;
            }
            if (phase == 0)
                fw->lmac_cnt++;
            else if (phase == 1)
                fw->umac_cnt++;
            else
                fw->paging_cnt++;
        }
    }
    return 0;
}

/* ---------------- pnvm file ---------------- */

#define IWL_UCODE_TLV_PNVM_VERSION 62
#define IWL_UCODE_TLV_PNVM_SKU     64
#define IWL_UCODE_TLV_HW_TYPE      58

static int pnvm_read_file(struct iwlm_fw *fw, const char *path)
{
    FILE *f = fopen(path, "rb");
    long sz;

    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    fw->pnvm = malloc(sz);
    if (!fw->pnvm || fread(fw->pnvm, 1, sz, f) != (size_t)sz) {
        fclose(f);
        free(fw->pnvm);
        fw->pnvm = NULL;
        return -2;
    }
    fclose(f);
    fw->pnvm_size = sz;
    return 0;
}

/*
 * iwlwifi fw/pnvm.c condensed: top-level TLVs are scanned for a
 * PNVM_SKU section matching the sku_id reported by the alive
 * notification; inside it a HW_TYPE tlv must match the running mac/rf
 * type, and every SEC_RT tlv is one DRAM payload.
 */
static int pnvm_parse_section(struct iwlm_dev *d, const u8 *p, u32 len)
{
    struct iwlm_fw *fw = &d->fw;
    u32 mac_type = CSR_HW_REV_TYPE(d->hw_rev);
    u32 rf_type = CSR_HW_RFID_TYPE(d->hw_rf_id);
    bool hw_match = false;

    while (len >= 8) {
        u32 type = get_unaligned_le32(p);
        u32 tlen = get_unaligned_le32(p + 4);
        const u8 *data = p + 8;

        if (tlen > len - 8)
            return -1;
        switch (type) {
        case IWL_UCODE_TLV_PNVM_VERSION:
            if (tlen >= 4)
                fw->pnvm_version = get_unaligned_le32(data);
            break;
        case IWL_UCODE_TLV_HW_TYPE:
            if (tlen >= 4 &&
                get_unaligned_le16(data) == mac_type &&
                get_unaligned_le16(data + 2) == rf_type)
                hw_match = true;
            break;
        case IWL_UCODE_TLV_SEC_RT:
            if (tlen >= 4 && get_unaligned_le32(data) != 0xddddeeee &&
                fw->pnvm_n_chunks < IWLM_PNVM_MAX_CHUNKS) {
                fw->pnvm_chunk[fw->pnvm_n_chunks] = data + 4;
                fw->pnvm_chunk_len[fw->pnvm_n_chunks] = tlen - 4;
                fw->pnvm_n_chunks++;
            }
            break;
        case IWL_UCODE_TLV_PNVM_SKU:
            goto out;       /* next sku section starts */
        default:
            break;
        }
        tlen = (tlen + 3) & ~3u;
        if (tlen > len - 8)
            break;
        p += 8 + tlen;
        len -= 8 + tlen;
    }
out:
    if (!hw_match || !fw->pnvm_n_chunks)
        return -2;
    return 0;
}

static int pnvm_parse(struct iwlm_dev *d)
{
    struct iwlm_fw *fw = &d->fw;
    const u8 *p = fw->pnvm;
    u32 len = fw->pnvm_size;
    const u32 *sku = d->trans.sku_id;

    while (len >= 8) {
        u32 type = get_unaligned_le32(p);
        u32 tlen = get_unaligned_le32(p + 4);
        u32 adv = 8 + ((tlen + 3) & ~3u);

        if (tlen > len - 8)
            break;
        if (type == IWL_UCODE_TLV_PNVM_SKU && tlen >= 12 &&
            get_unaligned_le32(p + 8) == sku[0] &&
            get_unaligned_le32(p + 12) == sku[1] &&
            get_unaligned_le32(p + 16) == sku[2]) {
            /* the matching payloads follow this tlv */
            if (adv > len)
                adv = 8 + tlen;
            return pnvm_parse_section(d, p + adv, len - adv);
        }
        if (adv > len)
            break;
        p += adv;
        len -= adv;
    }
    return -1;
}

/*
 * Full pnvm handshake, run after the alive notification: parse the
 * .pnvm payloads, hand them to the fw through prph_scratch, ring the
 * pnvm doorbell and wait for PNVM_INIT_COMPLETE_NTFY. Zero sku_id means
 * the hardware carries full NVM on its own (older flows) - nothing to
 * do.
 */
int iwlm_fw_pnvm_handshake(struct iwlm_dev *d)
{
    struct iwlm_trans *t = &d->trans;
    struct iwlm_fw *fw = &d->fw;
    int timeout;

    if (!t->sku_id[0] && !t->sku_id[1] && !t->sku_id[2])
        return 0;

    if (!fw->pnvm) {
        klog("iwlm: pnvm file missing but sku %08x:%08x:%08x\n",
             t->sku_id[0], t->sku_id[1], t->sku_id[2]);
        return -1;
    }
    if (pnvm_parse(d)) {
        klog("iwlm: no pnvm section for sku %08x:%08x:%08x hw %x/%x\n",
             t->sku_id[0], t->sku_id[1], t->sku_id[2],
             CSR_HW_REV_TYPE(d->hw_rev), CSR_HW_RFID_TYPE(d->hw_rf_id));
        return -2;
    }

    if (iwlm_trans_load_pnvm(d))
        return -3;

    /* kick the pnvm doorbell, fw acks with PNVM_INIT_COMPLETE_NTFY */
    t->pnvm_done = 0;
    iwlm_write_prph(d, UREG_DOORBELL_TO_ISR6, UREG_DOORBELL_TO_ISR6_PNVM);
    for (timeout = 0; timeout < 3000; timeout++) {
        iwlm_trans_rx_poll(d);
        if (t->pnvm_done)
            return 0;
        usleep(1000);
    }
    klog("iwlm: pnvm handshake timeout\n");
    return -4;
}

/* ---------------- firmware discovery ---------------- */

/*
 * Try "<dir>/<fw_pre>-<api>.ucode" from api_max down to api_min, like
 * iwl_request_firmware. The matching "<fw_pre>.pnvm" is loaded too when
 * present (AX210+ hardware needs it for the pnvm handshake).
 */
int iwlm_fw_find_and_load(struct iwlm_dev *d, const char *dir)
{
    char path[256];
    int ver;

    for (ver = d->cfg->api_max; ver >= d->cfg->api_min; ver--) {
        snprintf(path, sizeof(path), "%s/%s-%d.ucode", dir,
                 d->cfg->fw_pre, ver);
        if (iwlm_fw_load(d, path))
            continue;
        d->fw.api_ver = ver;
        klog("iwlm: fw %s-%d: %s lmac %d umac %d paging %d iml %d\n",
             d->cfg->fw_pre, ver, d->fw.ver_str,
             d->fw.lmac_cnt, d->fw.umac_cnt, d->fw.paging_cnt,
             (int)d->fw.iml_size);

        snprintf(path, sizeof(path), "%s/%s.pnvm", dir, d->cfg->fw_pre);
        if (pnvm_read_file(&d->fw, path))
            klog("iwlm: %s.pnvm not found\n", d->cfg->fw_pre);
        return 0;
    }
    klog("iwlm: no firmware for %s api %d..%d in %s\n",
         d->cfg->fw_pre, d->cfg->api_min, d->cfg->api_max, dir);
    return -1;
}

void iwlm_fw_free(struct iwlm_dev *d)
{
    free(d->fw.file);
    free(d->fw.pnvm);
    memset(&d->fw, 0, sizeof(d->fw));
}
