/* Host semantic test for nv_tex.c software sampling: compile small sampler
 * shaders through the real frontend+backend, run the SASS in the warp
 * emulator, and compare every pixel against an independent C reference
 * sampler (bilinear REPEAT 2D / NEAREST / cube face selection) over random
 * non-power-of-two images.  Unlike the emu_ocean differential (which runs the
 * same lowered NIR on both sides), this checks the lowering's semantics.
 *
 *   make -f tests/host/Makefile.host tool-test_tex && /tmp/nv-host/test_tex
 */
#include "pglcompiler.h"
#include "nv_backend.h"
#include "nv_pglc.h"
#include "nv_emu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>

static nir_shader* g_nir;
static std::vector<uint32_t> g_code;
static struct nv_kernel_meta g_meta;

static void hook(nir_shader* nir, const struct nv_be_result* res, void*) {
    g_nir = nir_shader_clone(NULL, nir);
    g_code.assign(res->bin, res->bin + res->bin_words);
    g_meta = res->meta;
}

/* ---- independent C reference samplers ---------------------------------- */

static int wrapmod(int i, int n) { i %= n; return i < 0 ? i + n : i; }
static int clampi(int i, int lo, int hi) { return i < lo ? lo : i > hi ? hi : i; }

static void texel(const uint8_t* img, int w, int x, int y, float out[4]) {
    const uint8_t* p = img + ((size_t)y * w + x) * 4;
    for (int c = 0; c < 4; c++) out[c] = p[c] / 255.0f;
}

/* bilinear with REPEAT (linear) or NEAREST, portablegl-style math */
static void ref_sample2d(const uint8_t* img, int w, int h, float u, float v,
                         int linear, float out[4]) {
    float xw = u * w, yh = v * h;
    if (!linear) {
        int i = wrapmod((int)floorf(xw), w), j = wrapmod((int)floorf(yh), h);
        texel(img, w, i, j, out);
        return;
    }
    int i0 = wrapmod((int)floorf(xw - 0.5f), w);
    int j0 = wrapmod((int)floorf(yh - 0.5f), h);
    int i1 = wrapmod(i0 + 1, w), j1 = wrapmod(j0 + 1, h);
    float a = xw - 0.5f - floorf(xw - 0.5f);
    float b = yh - 0.5f - floorf(yh - 0.5f);
    float t00[4], t10[4], t01[4], t11[4];
    texel(img, w, i0, j0, t00); texel(img, w, i1, j0, t10);
    texel(img, w, i0, j1, t01); texel(img, w, i1, j1, t11);
    for (int c = 0; c < 4; c++)
        out[c] = t00[c] * (1 - a) * (1 - b) + t10[c] * a * (1 - b) +
                 t01[c] * (1 - a) * b + t11[c] * a * b;
}

/* GL major-axis cube face selection + per-face clamp bilinear */
static void ref_sample_cube(const uint8_t* faces, int n, float x, float y,
                            float z, float out[4]) {
    float ax = fabsf(x), ay = fabsf(y), az = fabsf(z);
    int face; float s, t, ma;
    if (ax >= ay && ax >= az) {
        ma = ax; t = -y;
        if (x >= 0) { face = 0; s = -z; } else { face = 1; s = z; }
    } else if (ay >= az) {
        ma = ay; s = x;
        if (y >= 0) { face = 2; t = z; } else { face = 3; t = -z; }
    } else {
        ma = az; t = -y;
        if (z >= 0) { face = 4; s = x; } else { face = 5; s = -x; }
    }
    float u = 0, v = 0;
    if (ma != 0) { u = (s / ma + 1) * 0.5f; v = (t / ma + 1) * 0.5f; }
    const uint8_t* img = faces + (size_t)face * n * n * 4;
    float xw = u * n - 0.5f, yh = v * n - 0.5f;
    int i0 = (int)floorf(xw), j0 = (int)floorf(yh);
    float a = xw - i0, b = yh - j0;
    int ia = clampi(i0, 0, n - 1), ib = clampi(i0 + 1, 0, n - 1);
    int ja = clampi(j0, 0, n - 1), jb = clampi(j0 + 1, 0, n - 1);
    float t00[4], t10[4], t01[4], t11[4];
    texel(img, n, ia, ja, t00); texel(img, n, ib, ja, t10);
    texel(img, n, ia, jb, t01); texel(img, n, ib, jb, t11);
    for (int c = 0; c < 4; c++)
        out[c] = t00[c] * (1 - a) * (1 - b) + t10[c] * a * (1 - b) +
                 t01[c] * (1 - a) * b + t11[c] * a * b;
}

/* ---- one case ----------------------------------------------------------- */

struct case_def {
    const char* name;
    const char* fs;
    int w, h;              /* framebuffer */
    int tw, th, depth;     /* texture */
    int linear;
    int cube;
    int zero_dir;
};

static uint32_t rng = 12345;
static uint8_t rnd_byte(void) { rng = rng * 1664525u + 1013904223u; return (uint8_t)(rng >> 24); }

static int run_case(const case_def& c) {
    std::vector<uint8_t> img((size_t)c.tw * c.th * 4 * c.depth);
    for (size_t i = 0; i < img.size(); i++) img[i] = rnd_byte();

    struct pglc_program prog;
    g_nir = NULL;
    nv_be_set_test_hook(hook, NULL);
    if (!pglc_compile_compute(c.fs, &prog) || !g_nir) {
        printf("%s: compile FAIL: %s\n", c.name, prog.log);
        return 1;
    }
    struct pglc_stage* st = &prog.stage[PGLC_STAGE_COMPUTE];
    unsigned W = c.w, H = c.h;
    std::vector<uint32_t> unif(st->uniform_blob_dwords + 4, 0);
    for (unsigned i = 0; i < st->num_uniform_bindings; i++) {
        const struct pglc_uniform_binding* b = &st->uniform_bindings[i];
        if (!strcmp(b->name, "pglc_cs_stride")) unif[b->dword_offset] = W;
        else if (!strcmp(b->name, "iResolution")) {
            float v[3] = { (float)W, (float)H, 1.0f };
            memcpy(&unif[b->dword_offset], v, 12);
        }
    }
    const uint32_t* blk = g_meta.local_size;
    uint32_t grid[3] = { (W + blk[0] - 1) / blk[0], (H + blk[1] - 1) / blk[1], 1 };
    const uint64_t fb_va = 0x7f0000100000ull, tex_va = 0x7f0010100000ull;
    std::vector<uint32_t> fb(W * H, 0xdeadbeefu);
    /* staged texture: cube layers 64B-aligned like the client upload */
    uint32_t face_bytes = (uint32_t)c.tw * c.th * 4;
    uint32_t face_stride = c.depth == 6 ? (face_bytes + 63) & ~63u : face_bytes;
    std::vector<uint8_t> staged((size_t)face_stride * c.depth, 0);
    for (int f = 0; f < c.depth; f++)
        memcpy(staged.data() + (size_t)f * face_stride,
               img.data() + (size_t)f * face_bytes, face_bytes);

    std::vector<uint8_t> cb0(NV_CB0_SIZE, 0);
    memcpy(&cb0[0x00], blk, 12);
    memcpy(&cb0[0x0c], grid, 12);
    memcpy(&cb0[NV_UNIFORM_BASE_OFF], unif.data(), st->uniform_blob_dwords * 4);
    memcpy(&cb0[NV_SSBO_BASE_OFF], &fb_va, 8);
    memcpy(&cb0[NV_SSBO_BASE_OFF + 8], &tex_va, 8);
    uint32_t* td = (uint32_t*)&cb0[NV_TEXDESC_OFF];
    td[0] = c.tw; td[1] = c.th; td[2] = c.tw;
    td[3] = c.depth == 6 ? face_stride / 4 : 0;
    td[4] = c.linear;

    struct nv_emu_env env;
    memset(&env, 0, sizeof(env));
    env.code = g_code.data();
    env.ninstr = (unsigned)g_code.size() / 4;
    env.num_gprs = g_meta.num_gprs;
    env.cb0 = cb0.data();
    env.cb0_size = (uint32_t)cb0.size();
    env.mem[0] = { fb_va, (uint8_t*)fb.data(), fb.size() * 4 };
    env.mem[1] = { tex_va, staged.data(), staged.size() };
    env.nmem = 2;
    memcpy(env.block, blk, 12);
    char err[512];
    struct nv_emu* emu = nv_emu_create(&env, err, sizeof(err));
    if (!emu) { printf("%s: emu: %s\n", c.name, err); return 1; }
    for (uint32_t gy = 0; gy < grid[1]; gy++)
        for (uint32_t gx = 0; gx < grid[0]; gx++) {
            uint32_t cta[3] = { gx, gy, 0 };
            if (nv_emu_run_cta(emu, cta, err, sizeof(err)) != 0) {
                printf("%s: EMU FAULT: %s\n", c.name, err);
                return 1;
            }
        }

    unsigned bad = 0, maxdiff = 0;
    for (unsigned y = 0; y < H; y++)
        for (unsigned x = 0; x < W; x++) {
            /* the frontend's gl_FragCoord: (x+0.5, H-1-y+0.5) */
            float fx = x + 0.5f, fy = (float)(H - 1 - y) + 0.5f;
            float u = fx / W, v = fy / H;
            float ref[4];
            if (c.cube) {
                float dx = u * 2 - 1, dy = v * 2 - 1;
                float dz = sinf(u * 6.2831853f) * 0.8f;
                if (c.zero_dir) dx = dy = dz = 0;
                float len = sqrtf(dx * dx + dy * dy + dz * dz);
                if (len > 0) { dx /= len; dy /= len; dz /= len; }
                ref_sample_cube(img.data(), c.tw, dx, dy, dz, ref);
            } else {
                ref_sample2d(img.data(), c.tw, c.th, u * 2.3f - 0.65f,
                             v * 2.3f - 0.65f, c.linear, ref);
            }
            uint32_t px = fb[y * W + x];
            uint8_t got[4] = { (uint8_t)(px >> 16), (uint8_t)(px >> 8),
                               (uint8_t)px, (uint8_t)(px >> 24) };
            for (int ch = 0; ch < 4; ch++) {
                int want = (int)(ref[ch] * 255.0f + 0.5f);
                int d = abs((int)got[ch] - want);
                if ((unsigned)d > maxdiff) maxdiff = (unsigned)d;
                if (d > 1) {
                    if (bad < 4)
                        printf("%s: (%u,%u) ch%d got %d want %d\n",
                               c.name, x, y, ch, got[ch], want);
                    bad++;
                }
            }
        }
    printf("%s: %ux%u tex %dx%d%s %s: %u bad channels (max diff %u) %s\n",
           c.name, W, H, c.tw, c.th, c.cube ? " cube" : "",
           c.linear ? "LINEAR" : "NEAREST", bad, maxdiff,
           bad == 0 ? "PASS" : "FAIL");
    nv_emu_destroy(emu);
    if (g_nir) ralloc_free(g_nir);
    pglc_program_free(&prog);
    return bad == 0 ? 0 : 1;
}

static const char* fs_2d =
    "#version 100\n"
    "precision highp float;\n"
    "uniform vec3 iResolution;\n"
    "uniform sampler2D iChannel0;\n"
    "void main() {\n"
    "    vec2 uv = gl_FragCoord.xy / iResolution.xy;\n"
    "    gl_FragColor = texture2D(iChannel0, uv * 2.3 - 0.65);\n"
    "}\n";

static const char* fs_cube =
    "#version 100\n"
    "precision highp float;\n"
    "uniform vec3 iResolution;\n"
    "uniform samplerCube iChannel0;\n"
    "void main() {\n"
    "    vec2 uv = gl_FragCoord.xy / iResolution.xy;\n"
    "    vec3 dir = vec3(uv * 2.0 - 1.0, sin(uv.x * 6.2831853) * 0.8);\n"
    "    gl_FragColor = textureCube(iChannel0, normalize(dir));\n"
    "}\n";

/* the zero vector (refract() total internal reflection) samples face 0 at
 * (0,0), matching the CPU interpreter; normalize(0) is NOT tested here: the
 * mesa frontend folds it to NaN (GLSL-undefined), unlike the CPU guard. */
static const char* fs_cube_zero =
    "#version 100\n"
    "precision highp float;\n"
    "uniform vec3 iResolution;\n"
    "uniform samplerCube iChannel0;\n"
    "void main() {\n"
    "    gl_FragColor = textureCube(iChannel0, vec3(0.0));\n"
    "}\n";

int main(void) {
    int fails = 0;
    case_def cases[] = {
        { "2d_linear_repeat", fs_2d, 64, 48, 8, 6, 1, 1, 0, 0 },
        { "2d_nearest_repeat", fs_2d, 64, 48, 8, 6, 1, 0, 0, 0 },
        { "cube_linear", fs_cube, 64, 48, 4, 4, 6, 1, 1, 0 },
        { "cube_zero_vec", fs_cube_zero, 8, 8, 4, 4, 6, 1, 1, 1 },
    };
    for (const case_def& c : cases) fails += run_case(c);
    printf("%s\n", fails ? "TEX SEMANTICS FAIL" : "TEX SEMANTICS PASS");
    return fails ? 1 : 0;
}