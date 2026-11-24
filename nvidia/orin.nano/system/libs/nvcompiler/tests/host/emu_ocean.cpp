/* Host differential test: compile a fragment shader (the real ocean one by
 * default), then run the emitted sm_87 SASS in the warp emulator and the
 * lowered NIR in the reference interpreter over a whole frame and compare every
 * pixel.
 *
 *   emu_ocean [-f shader.frag] [width height [time]]
 *
 * Writes /tmp/nv-host/<name>.emu.lst (emulator disassembly, checked against
 * nvdisasm by check_disasm.py), <name>.cubin and <name>_emu.ppm / _ref.ppm. */
#include "ocean_glsl.h"
#include "pglcompiler.h"
#include "nv_backend.h"
#include "nv_pglc.h"
#include "nv_emu.h"
#include "nir_ref.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

static nir_shader *g_nir;
static std::vector<uint32_t> g_code;
static struct nv_kernel_meta g_meta;

static void hook(nir_shader *nir, const struct nv_be_result *res, void *) {
    g_nir = nir_shader_clone(NULL, nir);
    g_code.assign(res->bin, res->bin + res->bin_words);
    g_meta = res->meta;
}

static void write_ppm(const char *path, const std::vector<uint32_t> &fb,
                      unsigned w, unsigned h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (unsigned i = 0; i < w * h; i++) {
        unsigned char rgb[3] = { (unsigned char)(fb[i] >> 16),
                                 (unsigned char)(fb[i] >> 8),
                                 (unsigned char)fb[i] };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static std::string out_path(const std::string &name, const char *suffix) {
    return "/tmp/nv-host/" + name + suffix;
}

int main(int argc, char **argv) {
    std::string fs = ocean_gen::build_ocean_fs(2, 3, 4);
    std::string name = "ocean";
    if (argc > 2 && !strcmp(argv[1], "-f")) {
        FILE *f = fopen(argv[2], "rb");
        if (!f) {
            printf("cannot open %s\n", argv[2]);
            return 1;
        }
        fs.clear();
        char buf[4096];
        size_t got;
        while ((got = fread(buf, 1, sizeof(buf), f)) > 0)
            fs.append(buf, got);
        fclose(f);
        name = argv[2];
        size_t slash = name.find_last_of('/');
        if (slash != std::string::npos) name = name.substr(slash + 1);
        size_t dot = name.find_last_of('.');
        if (dot != std::string::npos) name = name.substr(0, dot);
        argc -= 2;
        argv += 2;
    }
    unsigned W = argc > 2 ? (unsigned)atoi(argv[1]) : 160;
    unsigned H = argc > 2 ? (unsigned)atoi(argv[2]) : 120;
    float time = argc > 3 ? (float)atof(argv[3]) : 1.0f;
    /* "samples": only the pixels ocean's "check" mode prints, in its format */
    bool samples = argc > 4 && !strcmp(argv[4], "samples");

    struct pglc_program prog;
    nv_be_set_test_hook(hook, NULL);
    if (!pglc_compile_compute(fs.c_str(), &prog) || !g_nir) {
        printf("%s: compile FAIL: %s\n", name.c_str(), prog.log);
        return 1;
    }
    struct pglc_stage *st = &prog.stage[PGLC_STAGE_COMPUTE];
    struct nv_pglc_stage *ab = (struct nv_pglc_stage *)st->backend;
    if (ab && ab->cubin) {
        FILE *f = fopen(out_path(name, ".cubin").c_str(), "wb");
        if (f) { fwrite(ab->cubin, 1, ab->cubin_size, f); fclose(f); }
    }
    unsigned ninstr = (unsigned)g_code.size() / 4;
    /* same hash the driver logs at module load ("fnv=") */
    uint32_t fnv = 2166136261u;
    for (size_t i = 0; i < g_code.size() * 4; i++)
        fnv = (fnv ^ ((const uint8_t *)g_code.data())[i]) * 16777619u;
    printf("%s: %u instrs (text=%u fnv=%08x), %u GPRs declared, local %ux%ux%u\n",
           name.c_str(), ninstr, ninstr * 16, fnv, g_meta.num_gprs,
           g_meta.local_size[0], g_meta.local_size[1], g_meta.local_size[2]);

    /* uniform blob, by reflected name */
    std::vector<uint32_t> unif(st->uniform_blob_dwords + 4, 0);
    for (unsigned i = 0; i < st->num_uniform_bindings; i++) {
        const struct pglc_uniform_binding *b = &st->uniform_bindings[i];
        float v[4] = { 0, 0, 0, 0 };
        uint32_t *dst = &unif[b->dword_offset];
        if (!strcmp(b->name, "pglc_cs_stride")) {
            dst[0] = W;
        } else if (!strcmp(b->name, "iGlobalTime") || !strcmp(b->name, "iTime")) {
            v[0] = time; memcpy(dst, v, 4);
        } else if (!strcmp(b->name, "iResolution")) {
            v[0] = (float)W; v[1] = (float)H; v[2] = 1.0f;
            memcpy(dst, v, 4 * b->num_components);
        } else {
            printf("note: uniform %s left zero\n", b->name);
        }
        printf("uniform %-16s dword %u comps %u\n", b->name, b->dword_offset,
               b->num_components);
    }

    const uint32_t *blk = g_meta.local_size;
    uint32_t grid[3] = { (W + blk[0] - 1) / blk[0], (H + blk[1] - 1) / blk[1], 1 };
    const uint64_t fb_va = 0x7f0000100000ull;
    std::vector<uint32_t> fb_emu(W * H, 0xdeadbeefu), fb_ref(W * H, 0xdeadbeefu);

    std::vector<uint8_t> cb0(NV_CB0_SIZE, 0);
    memcpy(&cb0[0x00], blk, 12);
    memcpy(&cb0[0x0c], grid, 12);
    memcpy(&cb0[NV_UNIFORM_BASE_OFF], unif.data(), st->uniform_blob_dwords * 4);
    memcpy(&cb0[NV_SSBO_BASE_OFF], &fb_va, 8);

    struct nv_emu_env env;
    memset(&env, 0, sizeof(env));
    env.code = g_code.data();
    env.ninstr = ninstr;
    env.num_gprs = g_meta.num_gprs;
    env.cb0 = cb0.data();
    env.cb0_size = (uint32_t)cb0.size();
    env.mem[0].va = fb_va;
    env.mem[0].p = (uint8_t *)fb_emu.data();
    env.mem[0].size = fb_emu.size() * 4;
    env.nmem = 1;
    memcpy(env.block, blk, 12);

    char err[512] = "";
    struct nv_emu *emu = nv_emu_create(&env, err, sizeof(err));
    if (!emu) {
        printf("emulator: %s\n", err);
        return 1;
    }
    FILE *lst = fopen(out_path(name, ".emu.lst").c_str(), "w");
    for (unsigned i = 0; lst && i < ninstr; i++) {
        char line[160];
        nv_emu_disasm(emu, i, line, sizeof(line));
        fprintf(lst, "%04x %s\n", i * 16, line);
    }
    if (lst) fclose(lst);

    struct nir_ref_env renv;
    memset(&renv, 0, sizeof(renv));
    renv.uniforms = (const uint8_t *)unif.data();
    renv.uniform_bytes = st->uniform_blob_dwords * 4;
    renv.ssbo[0].p = (uint8_t *)fb_ref.data();
    renv.ssbo[0].size = fb_ref.size() * 4;
    memcpy(renv.num_workgroups, grid, 12);
    struct nir_ref *ref = nir_ref_create(g_nir, &renv);

    if (samples) {
        const unsigned C = 16, R = 9;
        unsigned bad = 0;
        for (unsigned j = 0; j < R; j++) {
            printf("ocean: check %ux%u row%u:", W, H, j);
            for (unsigned i = 0; i < C; i++) {
                /* GL row y is framebuffer row H-1-y */
                unsigned x = (2 * i + 1) * W / (2 * C);
                unsigned y = H - 1 - (2 * j + 1) * H / (2 * R);
                uint32_t cta[3] = { x / blk[0], y / blk[1], 0 };
                uint32_t lid[3] = { x % blk[0], y % blk[1], 0 };
                if (nv_emu_run_cta(emu, cta, err, sizeof(err)) != 0 ||
                    nir_ref_run(ref, cta, lid, err, sizeof(err)) != 0) {
                    printf("\nFAULT: %s\n", err);
                    return 1;
                }
                uint32_t p = fb_ref[y * W + x];
                bad += p != fb_emu[y * W + x] || p == 0xdeadbeefu;
                printf(" %02x%02x%02x", (p >> 16) & 0xff, (p >> 8) & 0xff, p & 0xff);
            }
            printf("\n");
        }
        printf("%s\n", bad == 0 ? "SAMPLES: emulator == reference" : "SAMPLES FAIL");
        return bad == 0 ? 0 : 1;
    }

    for (uint32_t gy = 0; gy < grid[1]; gy++) {
        for (uint32_t gx = 0; gx < grid[0]; gx++) {
            uint32_t cta[3] = { gx, gy, 0 };
            if (nv_emu_run_cta(emu, cta, err, sizeof(err)) != 0) {
                printf("EMU FAULT: %s\n", err);
                return 1;
            }
            for (uint32_t ly = 0; ly < blk[1]; ly++)
                for (uint32_t lx = 0; lx < blk[0]; lx++) {
                    uint32_t lid[3] = { lx, ly, 0 };
                    if (nir_ref_run(ref, cta, lid, err, sizeof(err)) != 0) {
                        printf("REF ERROR: %s\n", err);
                        return 1;
                    }
                }
        }
    }

    unsigned bad = 0, unwritten = 0, maxdiff = 0;
    for (unsigned i = 0; i < W * H; i++) {
        if (fb_ref[i] == 0xdeadbeefu) unwritten++;
        if (fb_emu[i] == fb_ref[i]) continue;
        for (int sh = 0; sh < 32; sh += 8) {
            int d = (int)((fb_emu[i] >> sh) & 0xff) - (int)((fb_ref[i] >> sh) & 0xff);
            if (d < 0) d = -d;
            if ((unsigned)d > maxdiff) maxdiff = (unsigned)d;
        }
        if (bad++ < 8)
            printf("mismatch at (%u,%u): emu %08x ref %08x\n", i % W, i / W,
                   fb_emu[i], fb_ref[i]);
    }
    write_ppm(out_path(name, "_emu.ppm").c_str(), fb_emu, W, H);
    write_ppm(out_path(name, "_ref.ppm").c_str(), fb_ref, W, H);
    printf("%ux%u t=%.2f: %llu lane-steps, %u/%u pixels differ (max channel "
           "diff %u), %u pixels unwritten by the reference\n", W, H, time,
           (unsigned long long)nv_emu_steps(emu), bad, W * H, maxdiff, unwritten);
    printf("%s\n", bad == 0 && unwritten == 0 ? "DIFFERENTIAL PASS" : "DIFFERENTIAL FAIL");
    nv_emu_destroy(emu);
    nir_ref_destroy(ref);
    pglc_program_free(&prog);
    return bad == 0 && unwritten == 0 ? 0 : 1;
}
