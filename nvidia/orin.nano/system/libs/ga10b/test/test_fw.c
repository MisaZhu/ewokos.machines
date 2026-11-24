/*
 * Host-side test: run the GA10B firmware parse/validation layer against
 * the real prod blobs (extracted from linux-firmware-nvidia-tegra
 * 36.4.3).  Build & run on the dev machine, no EwokOS / no hardware:
 *
 *   cc -I.. -o /tmp/test_fw test_fw.c ../ga10b_fw_parse.c && \
 *       /tmp/test_fw /Users/tony/work/gpu-fw/ga10b
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ga10b_fw.h"

static int _fails;

#define CHECK(cond, msg) do { \
    if (cond) { printf("PASS %s\n", msg); } \
    else { printf("FAIL %s\n", msg); _fails++; } \
} while (0)

static uint8_t* slurp(const char* dir, const char* name, uint32_t* size) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        printf("FAIL open %s\n", path);
        _fails++;
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = malloc((size_t)sz);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        printf("FAIL read %s\n", path);
        _fails++;
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (uint32_t)sz;
    return buf;
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "/Users/tony/work/gpu-fw/ga10b";
    uint32_t sz;

    /* PMU desc + image */
    uint8_t* desc = slurp(dir, GA10B_FW_PMU_DESC, &sz);
    uint32_t img_sz;
    uint8_t* img = slurp(dir, GA10B_FW_PMU_IMAGE, &img_sz);
    ga10b_pmu_ucode_desc_t pmu;
    CHECK(desc && img && ga10b_fw_parse_pmu_desc(desc, sz, img_sz, &pmu) == 0,
          "pmu desc parse");
    if (desc && img) {
        CHECK(pmu.version == 4, "pmu desc version == 4");
        CHECK(pmu.bootloader_offset == 0x7000 && pmu.bootloader_size == 0xff0,
              "pmu bootloader region");
        CHECK(pmu.next_core_elf_offset == 0x8000 &&
              pmu.next_core_elf_size == 0x1d568, "pmu elf region");
        CHECK(pmu.manifest_offset == 0 && pmu.manifest_size == 0x800,
              "pmu manifest region");
        CHECK(pmu.is_monitor_enabled == 1, "pmu monitor enabled");
        /* negative: truncated image must fail bounds check */
        CHECK(ga10b_fw_parse_pmu_desc(desc, sz, 0x1000, &pmu) != 0,
              "pmu desc rejects truncated image");
        CHECK(ga10b_fw_parse_pmu_desc(desc, 16, img_sz, &pmu) != 0,
              "pmu desc rejects short desc");
    }
    free(desc);
    free(img);

    /* PKC signatures */
    static const char* sigs[] = {
        GA10B_FW_PMU_SIG, GA10B_FW_FECS_SIG, GA10B_FW_GPCCS_SIG
    };
    static const uint32_t sig_ids[] = {
        GA10B_FALCON_PMU_NEXT_CORE, GA10B_FALCON_FECS, GA10B_FALCON_GPCCS
    };
    for (int i = 0; i < 3; i++) {
        uint8_t* b = slurp(dir, sigs[i], &sz);
        char msg[128];
        snprintf(msg, sizeof(msg), "pkc sig %s", sigs[i]);
        CHECK(b && ga10b_fw_check_pkc_sig(b, sz, sig_ids[i]) == 0, msg);
        if (b) {
            CHECK(ga10b_fw_check_pkc_sig(b, sz - 1, sig_ids[i]) != 0,
                  "pkc sig rejects wrong size");
            CHECK(ga10b_fw_check_pkc_sig(b, sz, sig_ids[i] + 100) != 0,
                  "pkc sig rejects wrong falcon id");
        }
        free(b);
    }

    /* FECS/GPCCS encrypted headers */
    static const char* encs[] = { GA10B_FW_FECS_ENC, GA10B_FW_GPCCS_ENC };
    for (int i = 0; i < 2; i++) {
        uint8_t* b = slurp(dir, encs[i], &sz);
        char msg[128];
        snprintf(msg, sizeof(msg), "ctxsw enc %s", encs[i]);
        CHECK(b && ga10b_fw_check_ctxsw_enc(b, sz) == 0, msg);
        free(b);
    }

    /* ACR set */
    uint32_t m_sz, t_sz, d_sz;
    uint8_t* m = slurp(dir, GA10B_FW_ACR_MANIFEST, &m_sz);
    uint8_t* t = slurp(dir, GA10B_FW_ACR_TEXT, &t_sz);
    uint8_t* d = slurp(dir, GA10B_FW_ACR_DATA, &d_sz);
    CHECK(m && t && d && ga10b_fw_check_acr_set(m_sz, t_sz, d_sz) == 0,
          "acr set sanity");
    CHECK(ga10b_fw_check_acr_set(m_sz, t_sz, 0) != 0,
          "acr set rejects missing data");
    free(m);
    free(t);
    free(d);

    /* netlist present */
    static const char* nets[] = {
        GA10B_FW_NET_A, GA10B_FW_NET_B, GA10B_FW_NET_C, GA10B_FW_NET_D
    };
    for (int i = 0; i < 4; i++) {
        uint8_t* b = slurp(dir, nets[i], &sz);
        char msg[128];
        snprintf(msg, sizeof(msg), "netlist %s non-empty", nets[i]);
        CHECK(b && sz > 0, msg);
        free(b);
    }

    /* ACR descriptor fill */
    ga10b_flcn2_acr_desc_t ad;
    ga10b_fw_fill_acr_desc(&ad, 0x12345000ULL, 1000, 0x20000000ULL);
    CHECK(ad.nonwpr_ucode_blob_size == 1024, "acr desc blob size 256-align");
    CHECK(ad.nonwpr_ucode_blob_start == 0x12345000ULL &&
          ad.ls_pmu_desc == 0x20000000ULL && ad.regions.no_regions == 1,
          "acr desc fields");

    /* ---- LS blob construction against the real prod blobs ---- */
    uint32_t enc_f_sz, enc_g_sz, net_sz, sig_f_sz, sig_g_sz;
    uint8_t* enc_f = slurp(dir, GA10B_FW_FECS_ENC, &enc_f_sz);
    uint8_t* enc_g = slurp(dir, GA10B_FW_GPCCS_ENC, &enc_g_sz);
    uint8_t* netc = slurp(dir, GA10B_FW_NET_C, &net_sz);
    uint8_t* sig_f = slurp(dir, GA10B_FW_FECS_SIG, &sig_f_sz);
    uint8_t* sig_g = slurp(dir, GA10B_FW_GPCCS_SIG, &sig_g_sz);

    ga10b_ctxsw_bootloader_desc_t fb, gb;
    CHECK(enc_f && ga10b_fw_parse_ctxsw_boot(enc_f, enc_f_sz, &fb) == 0,
          "fecs boot desc parse");
    CHECK(enc_g && ga10b_fw_parse_ctxsw_boot(enc_g, enc_g_sz, &gb) == 0,
          "gpccs boot desc parse");
    CHECK(fb.imem_offset == 0x9e00 && gb.imem_offset == 0x3800,
          "ctxsw boot imem offsets (fecs 9e00 / gpccs 3800)");

    uint32_t fi_o, fi_s, fd_o, fd_s, gi_o, gi_s, gd_o, gd_s;
    int net_ok = netc &&
        ga10b_fw_netlist_find(netc, net_sz, GA10B_NET_FECS_INST, &fi_o, &fi_s) == 0 &&
        ga10b_fw_netlist_find(netc, net_sz, GA10B_NET_FECS_DATA, &fd_o, &fd_s) == 0 &&
        ga10b_fw_netlist_find(netc, net_sz, GA10B_NET_GPCCS_INST, &gi_o, &gi_s) == 0 &&
        ga10b_fw_netlist_find(netc, net_sz, GA10B_NET_GPCCS_DATA, &gd_o, &gd_s) == 0;
    CHECK(net_ok, "NETC ctxsw regions 0..3 found");
    CHECK(ga10b_fw_netlist_find(netc, net_sz, 9999, &fi_o, &fi_s) != 0,
          "netlist rejects unknown region");

    if (net_ok && enc_f && enc_g && sig_f && sig_g) {
        ga10b_ls_image_desc_t fd, gd;
        ga10b_fw_ctxsw_image_desc(&fb, fi_s, fd_s, 0, &fd);
        ga10b_fw_ctxsw_image_desc(&gb, gi_s, gd_s, 1, &gd);
        CHECK(fd.app_start_offset == 256 && gd.app_start_offset == 256,
              "ctxsw app starts at 256");
        CHECK(fd.image_size == 256 + ((fi_s + 255) & ~255u) +
                              ((fd_s + 255) & ~255u), "fecs image size");

        uint32_t surf_sz = fd.image_size + gd.image_size;
        uint8_t* surf = malloc(surf_sz);
        memset(surf, 0, surf_sz);
        memcpy(surf, enc_f + 16, fb.size);
        memcpy(surf + fd.app_start_offset, netc + fi_o, fi_s);
        memcpy(surf + fd.app_start_offset + fd.app_resident_data_offset,
               netc + fd_o, fd_s);
        uint8_t* gs = surf + fd.image_size;
        memcpy(gs, enc_g + 16, gb.size);
        memcpy(gs + gd.app_start_offset, netc + gi_o, gi_s);
        memcpy(gs + gd.app_start_offset + gd.app_resident_data_offset,
               netc + gd_o, gd_s);

        ga10b_lsf_ucode_desc_wrapper_t ws_f, ws_g;
        memcpy(&ws_f, sig_f, sizeof(ws_f));
        memcpy(&ws_g, sig_g, sizeof(ws_g));
        ws_f.v2.falcon_id = GA10B_FALCON_FECS;
        ws_g.v2.falcon_id = GA10B_FALCON_GPCCS;

        ga10b_ls_image_t img[2];
        memset(img, 0, sizeof(img));
        img[0].falcon_id = GA10B_FALCON_FECS;
        img[0].desc = fd; img[0].data = surf; img[0].data_size = fd.image_size;
        img[0].sig = &ws_f;
        img[1].falcon_id = GA10B_FALCON_GPCCS;
        img[1].is_priv_load = 1;
        img[1].desc = gd; img[1].data = gs; img[1].data_size = gd.image_size;
        img[1].sig = &ws_g;

        uint64_t wpr_base = 0x80000000ULL;
        uint32_t cap = ga10b_fw_ls_blob_size(img, 2);
        CHECK(cap > 0 && (cap & 255) == 0, "blob size 256-aligned");
        uint8_t* blob = malloc(cap);
        int built = ga10b_fw_build_ls_blob(blob, cap, img, 2, wpr_base);
        CHECK(built == (int)cap, "blob build fills capacity");
        CHECK(ga10b_fw_build_ls_blob(blob, cap - 1, img, 2, wpr_base) < 0,
              "blob build rejects short buffer");

        ga10b_lsf_wpr_header_t wh[3];
        memcpy(wh, blob, sizeof(wh));
        CHECK(wh[0].falcon_id == GA10B_FALCON_FECS &&
              wh[1].falcon_id == GA10B_FALCON_GPCCS &&
              wh[2].falcon_id == GA10B_FALCON_INVALID,
              "wpr headers ordered + terminator");
        CHECK(wh[0].bootstrap_owner == GA10B_FALCON_GSPLITE &&
              wh[0].status == GA10B_LSF_IMAGE_STATUS_COPY &&
              wh[0].lazy_bootstrap == 0, "wpr header owner/status/lazy");
        CHECK((wh[0].lsb_offset & 255) == 0 && (wh[1].lsb_offset & 255) == 0,
              "lsb offsets 256-aligned");

        ga10b_lsf_lsb_header_v1_t lsf, lsg;
        memcpy(&lsf, blob + wh[0].lsb_offset, sizeof(lsf));
        memcpy(&lsg, blob + wh[1].lsb_offset, sizeof(lsg));
        CHECK((lsf.ucode_off & 4095) == 0 && (lsg.ucode_off & 4095) == 0,
              "ucode offsets 4096-aligned");
        CHECK(lsf.signature.v2.falcon_id == GA10B_FALCON_FECS &&
              lsg.signature.v2.falcon_id == GA10B_FALCON_GPCCS,
              "lsb signature falcon ids");
        CHECK(lsf.bl_imem_off == 0x9e00 && lsg.bl_imem_off == 0x3800,
              "lsb bl imem offsets");
        CHECK(lsf.flags == 0 &&
              (lsg.flags & GA10B_LSF_FLAG_FORCE_PRIV_LOAD) != 0,
              "gpccs priv-load flag, fecs none");
        CHECK(lsf.bl_code_size == 256 &&
              lsf.app_code_off == 256 &&
              lsf.app_data_off == lsf.ucode_size,
              "fecs lsb code/data partition");

        ga10b_flcn_bl_dmem_desc_t bl;
        memcpy(&bl, blob + lsf.bl_data_off, sizeof(bl));
        uint64_t code_pa = ((uint64_t)bl.code_dma_base.hi << 32) |
                           bl.code_dma_base.lo;
        CHECK(code_pa == wpr_base + lsf.ucode_off + 256,
              "fecs BL code_dma_base = wpr+ucode_off+app_start");
        CHECK(bl.ctx_dma == 0 && bl.argc == 1 &&
              bl.non_sec_code_size == fi_s, "fecs BL desc fields");

        /* ucode image bytes landed at ucode_off (boot image head) */
        CHECK(memcmp(blob + lsf.ucode_off, enc_f + 16, 256) == 0,
              "fecs ucode boot image copied");
        CHECK(memcmp(blob + lsg.ucode_off, enc_g + 16, 256) == 0,
              "gpccs ucode boot image copied");

        /* next-core (PMU RISC-V) image: LSB keeps bl_data_off/size from the
         * layout (lsf_calc_wpr_size_pkc), with no BL descriptor written */
        {
            ga10b_pmu_ucode_desc_t nd;
            memset(&nd, 0, sizeof(nd));
            nd.bootloader_offset = 0x100;
            nd.bootloader_size = 0x300;
            nd.bootloader_param_size = 0x40;
            nd.next_core_elf_offset = 0x1000;
            nd.next_core_elf_size = 0x2345;
            uint8_t pimg[0x4000];
            memset(pimg, 0xa5, sizeof(pimg));
            ga10b_ls_image_t ni;
            memset(&ni, 0, sizeof(ni));
            ni.falcon_id = GA10B_FALCON_FECS;
            ni.is_next_core = 1;
            ni.ndesc = &nd;
            ni.data = pimg;
            ni.data_size = sizeof(pimg);
            uint32_t ncap = ga10b_fw_ls_blob_size(&ni, 1);
            uint8_t* nblob = malloc(ncap);
            CHECK(ga10b_fw_build_ls_blob(nblob, ncap, &ni, 1, wpr_base) ==
                  (int)ncap, "next-core blob build");
            ga10b_lsf_wpr_header_t nwh;
            ga10b_lsf_lsb_header_v1_t nl;
            memcpy(&nwh, nblob, sizeof(nwh));
            memcpy(&nl, nblob + nwh.lsb_offset, sizeof(nl));
            uint32_t bl_sz = ((uint32_t)sizeof(ga10b_flcn_bl_dmem_desc_t) +
                              255u) & ~255u;
            CHECK(nl.ucode_size == 0x440 &&
                  nl.data_size == 0x2345 + (0x3000 - 0x2785),
                  "next-core lsb ucode/data sizes");
            CHECK(nl.bl_code_size == 0 && nl.bl_imem_off == 0,
                  "next-core lsb has no BL code");
            CHECK(nl.bl_data_size == bl_sz &&
                  nl.bl_data_off ==
                      ((nl.ucode_off + (uint32_t)sizeof(pimg) + 255u) & ~255u),
                  "next-core lsb keeps bl_data layout");
            int zero = 1;
            for (uint32_t k = 0; k < bl_sz; k++)
                if (nblob[nl.bl_data_off + k] != 0)
                    zero = 0;
            CHECK(zero, "next-core has no BL descriptor bytes");
            CHECK(ncap == ((nl.bl_data_off + bl_sz + GA10B_PMU_WPR_RSVD +
                            255u) & ~255u), "next-core blob reserves 0x8000");
            free(nblob);
        }

        free(surf);
        free(blob);
    }
    free(enc_f); free(enc_g); free(netc); free(sig_f); free(sig_g);

    printf("%s (%d failures)\n", _fails == 0 ? "ALL PASS" : "FAILED", _fails);
    return _fails == 0 ? 0 : 1;
}