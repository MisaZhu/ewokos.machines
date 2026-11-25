/*
 * nvcc sm_87 cubin modules on GA10B: load, upload and launch.
 *
 * The cubin parser and the QMD v3.0 builder are the host-tested ones from
 * projects/cuda (libs/cuda/src/cubin.c, qmd.c), compiled into this driver.
 *
 * nvcc-generated SASS reads launch state from the head of constant bank 0,
 * which the CUDA driver fills at launch (seen in the vadd disassembly):
 *   c[0x0][0x00] blockDim.x/y/z      c[0x0][0x0c] gridDim.x/y/z
 *   c[0x0][0x28] initial R1 (per-thread stack pointer, top of local memory)
 *   c[0x0][0x118] 64-bit base added to every .E global address (0)
 * followed by the kernel parameters at param_base (0x160).
 */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ewoksys/klog.h>

#include "ga10b.h"
#include "cubin.h"
#include "qmd.h"
#include "../nvcompiler/nv_meta.h"   /* compiled-kernel cb0 layout contract */

#define CB0_BLOCK_DIM      0x00u
#define CB0_GRID_DIM       0x0cu
#define CB0_STACK_PTR      0x28u
#define GA10B_WARPS_PER_SM 48u

static const uint16_t smem_kb[] = { 0, 8, 16, 32, 64, 100, 132, 164 };

static int kernel_submit(ga10b_chan_t* ch, ga10b_module_t* m, int kernel,
                         const uint32_t grid[3], const uint32_t block[3],
                         uint32_t dyn_smem);

typedef struct {
    uint64_t code_va;
    void*    bank_va[CUBIN_NUM_CBANKS];
    uint64_t bank_gpu_va[CUBIN_NUM_CBANKS];
    uint32_t bank_size[CUBIN_NUM_CBANKS];
} mod_kernel_t;

struct ga10b_module {
    struct cubin_image img;
    void* image;                       /* private copy the parser points into */
    mod_kernel_t k[CUBIN_MAX_KERNELS];
};

int ga10b_buf_alloc(ga10b_gmmu_t* mm, uint32_t size, void** va,
                    uint64_t* gpu_va, int cacheable) {
    uint64_t phy;
    size = (size + 4095u) & ~4095u;
    if (ga10b_dma_alloc(size, 4096, va, &phy) != 0)
        return -1;
    *gpu_va = ga10b_gmmu_map_attr(mm, phy, size, cacheable);
    return *gpu_va != 0 ? 0 : -1;
}

void* ga10b_file_read(const char* path, uint32_t* size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    off_t sz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    void* p = sz > 0 ? malloc((size_t)sz) : NULL;
    if (p != NULL && read(fd, p, (size_t)sz) != sz) {
        free(p);
        p = NULL;
    }
    close(fd);
    *size = (uint32_t)sz;
    return p;
}

ga10b_module_t* ga10b_module_load(ga10b_gmmu_t* mm, const void* image,
                                  uint32_t size) {
    ga10b_module_t* m = calloc(1, sizeof(*m));
    if (m == NULL)
        return NULL;
    m->image = malloc(size);
    if (m->image == NULL)
        goto fail;
    memcpy(m->image, image, size);
    if (cubin_parse(&m->img, m->image, size) != CUBIN_OK ||
        cubin_needs_relocation(&m->img)) {
        klog("ga10b: cubin rejected (%u bytes)\n", size);
        goto fail;
    }
    for (int i = 0; i < m->img.nkernels; i++) {
        const struct cubin_kernel* k = &m->img.kernels[i];
        mod_kernel_t* mk = &m->k[i];
        void* code;
        if (ga10b_buf_alloc(mm, k->text_size, &code, &mk->code_va, 1) != 0)
            goto fail;
        memcpy(code, k->text, k->text_size);
        /* constant banks: kernel-owned first, else the module-wide one */
        for (uint32_t b = 0; b < CUBIN_NUM_CBANKS; b++) {
            const struct cubin_cbank* src =
                k->cbank[b].size != 0 ? &k->cbank[b] : &m->img.cbank[b];
            uint32_t sz = src->size;
            if (b == 0 && sz < (uint32_t)k->param_base + k->param_total)
                sz = (uint32_t)k->param_base + k->param_total;
            mk->bank_size[b] = sz;
            if (sz == 0)
                continue;
            /* uncached: bank 0 is rewritten for every launch */
            if (ga10b_buf_alloc(mm, sz, &mk->bank_va[b], &mk->bank_gpu_va[b],
                                0) != 0)
                goto fail;
            if (src->size != 0 && src->data != NULL)
                memcpy(mk->bank_va[b], src->data, src->size);
        }
        /* FNV-1a of the SASS: compare against the host build of the same
         * shader to tell an on-device miscompile from a GPU-side fault */
        uint32_t fnv = 2166136261u;
        for (uint32_t j = 0; j < k->text_size; j++)
            fnv = (fnv ^ k->text[j]) * 16777619u;
        klog("ga10b: kernel %s text=%u fnv=%08x regs=%u frame=%u cb0=%u params=%u\n",
             k->name, k->text_size, fnv, k->reg_count,
             k->frame_size > k->min_stack ? k->frame_size : k->min_stack,
             mk->bank_size[0], k->nparams);
    }
    __asm__ volatile("dsb sy" ::: "memory");
    return m;
fail:
    free(m->image);
    free(m);
    return NULL;
}

int ga10b_module_kernel(const ga10b_module_t* m, const char* name) {
    for (int i = 0; i < m->img.nkernels; i++)
        if (strcmp(m->img.kernels[i].name, name) == 0)
            return i;
    return -1;
}

/* args[i] points at kernel parameter i (cuLaunchKernel convention).  The
 * launch is synchronous, so the kernel's bank 0 is rewritten in place. */
int ga10b_kernel_launch(ga10b_chan_t* ch, ga10b_module_t* m, int kernel,
                        const uint32_t grid[3], const uint32_t block[3],
                        void* const* args) {
    return ga10b_kernel_launch_dyn(ch, m, kernel, grid, block, args, 0);
}

/* dynamic shared memory: the QMD sizes the carveout for static + dyn bytes
 * (extern __shared__ in the kernel); bypasses the 48KB static-smem cap */
int ga10b_kernel_launch_dyn(ga10b_chan_t* ch, ga10b_module_t* m, int kernel,
                            const uint32_t grid[3], const uint32_t block[3],
                            void* const* args, uint32_t dyn_smem) {
    if (kernel < 0 || kernel >= m->img.nkernels)
        return -1;
    const struct cubin_kernel* k = &m->img.kernels[kernel];
    mod_kernel_t* mk = &m->k[kernel];
    uint32_t local = k->frame_size > k->min_stack ? k->frame_size
                                                  : k->min_stack;
    uint32_t* cb0 = (uint32_t*)mk->bank_va[0];
    for (uint32_t i = 0; i < 3; i++) {
        cb0[CB0_BLOCK_DIM / 4u + i] = block[i];
        cb0[CB0_GRID_DIM / 4u + i] = grid[i];
    }
    cb0[CB0_STACK_PTR / 4u] = (local + 0xfu) & ~0xfu;
    if (cubin_pack_params(k, args, (uint8_t*)cb0 + k->param_base,
                          k->param_total) != CUBIN_OK)
        return -1;
    return kernel_submit(ch, m, kernel, grid, block, dyn_smem);
}

/* Build the QMD from the (already-filled) constant banks and submit.  Shared
 * by the nvcc-param launch and the compiled-compute launch below. */
static int kernel_submit(ga10b_chan_t* ch, ga10b_module_t* m, int kernel,
                         const uint32_t grid[3], const uint32_t block[3],
                         uint32_t dyn_smem) {
    const struct cubin_kernel* k = &m->img.kernels[kernel];
    mod_kernel_t* mk = &m->k[kernel];
    struct cuda_qmd_launch l;
    memset(&l, 0, sizeof(l));
    l.dyn_smem = dyn_smem;
    l.program = mk->code_va;
    for (uint32_t b = 0; b < CUBIN_NUM_CBANKS; b++) {
        l.cbuf[b] = mk->bank_gpu_va[b];
        l.cbuf_size[b] = mk->bank_size[b];
    }
    l.semaphore = ch->sem_gpu_va;
    l.payload = ch->sem_val + 1u;
    for (uint32_t i = 0; i < 3; i++) {
        l.grid[i] = grid[i];
        l.block[i] = block[i];
    }
    struct cuda_qmd q;
    if (cuda_qmd_from_kernel(&q, k, &l, GA10B_WARPS_PER_SM, smem_kb,
                             (int)(sizeof(smem_kb) / sizeof(smem_kb[0]))) != 0) {
        klog("ga10b: QMD build failed for %s\n", k->name);
        return -1;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    return ga10b_compute_launch(ch, q.w);
}

/* Launch a compiler-generated compute kernel (nvcompiler cubin).  Unlike the
 * nvcc param-window path, the kernel reads its uniform blob and SSBO base VAs
 * straight from constant bank 0 at the nv_meta.h layout offsets; fill those
 * here, then submit.  unif[] is the per-draw uniform blob (dword offsets from
 * pglc uniform reflection); ssbo_va[i] is the GPU VA bound to SSBO block i. */
int ga10b_kernel_launch_compute(ga10b_chan_t* ch, ga10b_module_t* m, int kernel,
                                const uint32_t grid[3], const uint32_t block[3],
                                const uint32_t* unif, uint32_t unif_dwords,
                                const uint64_t* ssbo_va, uint32_t nssbo,
                                const uint32_t* texdesc,
                                uint32_t texdesc_dwords) {
    if (kernel < 0 || kernel >= m->img.nkernels)
        return -1;
    const struct cubin_kernel* k = &m->img.kernels[kernel];
    mod_kernel_t* mk = &m->k[kernel];
    if (mk->bank_size[0] < NV_CB0_SIZE || nssbo > NV_MAX_SSBO ||
        unif_dwords * 4u > NV_SSBO_BASE_OFF - NV_UNIFORM_BASE_OFF ||
        texdesc_dwords * 4u > NV_MAX_TEX * NV_TEXDESC_STRIDE)
        return -1;
    uint32_t local = k->frame_size > k->min_stack ? k->frame_size
                                                  : k->min_stack;
    uint32_t* cb0 = (uint32_t*)mk->bank_va[0];
    for (uint32_t i = 0; i < 3; i++) {
        cb0[CB0_BLOCK_DIM / 4u + i] = block[i];
        cb0[CB0_GRID_DIM / 4u + i] = grid[i];
    }
    cb0[CB0_STACK_PTR / 4u] = (local + 0xfu) & ~0xfu;
    if (unif != NULL && unif_dwords != 0)
        memcpy((uint8_t*)cb0 + NV_UNIFORM_BASE_OFF, unif, unif_dwords * 4u);
    for (uint32_t i = 0; i < nssbo; i++)
        memcpy((uint8_t*)cb0 + NV_SSBO_BASE_OFF + i * 8u, &ssbo_va[i], 8u);
    memset((uint8_t*)cb0 + NV_TEXDESC_OFF, 0, NV_MAX_TEX * NV_TEXDESC_STRIDE);
    if (texdesc != NULL && texdesc_dwords != 0)
        memcpy((uint8_t*)cb0 + NV_TEXDESC_OFF, texdesc, texdesc_dwords * 4u);
    return kernel_submit(ch, m, kernel, grid, block, 0);
}

/* ---- self-test: the projects/cuda vadd kernel ---- */

#define VADD_BLOCK 256u
#define VADD_GRID  4u
#define VADD_N     (VADD_BLOCK * VADD_GRID)

int ga10b_compute_vadd(ga10b_chan_t* ch, const char* cubin_path) {
    uint32_t fsize = 0;
    void* file = ga10b_file_read(cubin_path, &fsize);
    if (file == NULL) {
        klog("ga10b: cannot read cubin %s\n", cubin_path);
        return -1;
    }
    ga10b_module_t* m = ga10b_module_load(ch->mm, file, fsize);
    free(file);
    int kid = m != NULL ? ga10b_module_kernel(m, "vadd") : -1;
    if (kid < 0) {
        klog("ga10b: cubin has no usable vadd kernel\n");
        return -1;
    }

    void *va, *vb, *vc;
    uint64_t ga, gb, gc;
    if (ga10b_buf_alloc(ch->mm, VADD_N * 4u, &va, &ga, 0) != 0 ||
        ga10b_buf_alloc(ch->mm, VADD_N * 4u, &vb, &gb, 0) != 0 ||
        ga10b_buf_alloc(ch->mm, VADD_N * 4u, &vc, &gc, 0) != 0)
        return -1;
    float* fa = (float*)va;
    float* fb = (float*)vb;
    float* fc = (float*)vc;
    for (uint32_t i = 0; i < VADD_N; i++) {
        fa[i] = (float)i;
        fb[i] = (float)(2u * i) + 0.5f;
        fc[i] = -1.0f;
    }

    const uint32_t grid[3] = { VADD_GRID, 1, 1 };
    const uint32_t block[3] = { VADD_BLOCK, 1, 1 };
    void* args[3] = { &ga, &gb, &gc };
    int r = ga10b_kernel_launch(ch, m, kid, grid, block, args);
    if (!ga10b.hw)
        return r;

    uint32_t bad = 0, first = 0;
    for (uint32_t i = 0; i < VADD_N; i++) {
        if (fc[i] != fa[i] + fb[i]) {
            if (bad == 0)
                first = i;
            bad++;
        }
    }
    klog("ga10b: vadd c[0]=%d c[1]=%d c[255]=%d c[256]=%d c[1023]=%d (x10)\n",
         (int)(fc[0] * 10.0f), (int)(fc[1] * 10.0f), (int)(fc[255] * 10.0f),
         (int)(fc[256] * 10.0f), (int)(fc[1023] * 10.0f));
    if (r != 0 || bad != 0) {
        klog("ga10b: vadd FAIL launch=%d mismatches=%u/%u first=%u\n", r, bad,
             VADD_N, first);
        ga10b_gr_launch_postmortem();
        return -1;
    }
    klog("ga10b: vadd PASS (%u elements, %u blocks x %u threads)\n", VADD_N,
         VADD_GRID, VADD_BLOCK);
    return 0;
}
