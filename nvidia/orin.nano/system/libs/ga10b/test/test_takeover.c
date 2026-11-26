/*
 * test_takeover.c -- host test of GPU hand-over between processes.
 *
 * The GPU cannot be power-cycled, so what one process leaves on the hardware
 * is what the next one finds.  Each "process" here is a fresh exec of this
 * binary (driver statics really are gone); the hardware -- BAR0 plus a small
 * PMU/FECS/FIFO model -- and the device-lifetime segment live in files that
 * every generation maps.
 *
 * Links the REAL ga10b_dev.c / ga10b_pmu.c / ga10b_pmem.c / ga10b_fifo.c /
 * ga10b_mm.c / ga10b_falcon.c; power layer, ACR and GR are stubs that record
 * their call order.
 *
 * The model is deliberately strict where the old driver went wrong:
 *   - the PMU RTOS reaches its boot args, instance block, page tables and
 *     super surface by physical address, and may only ever find them in the
 *     device-lifetime segment;
 *   - FECS answers the start handshake only once after each (re)bootstrap by
 *     the PMU, and a GR engine reset stops it;
 *   - a runlist preempt saves the loaded context into its owner's memory,
 *     which counts as corruption once that owner is gone.
 *
 * Build: cc -I. -Iinclude -Itest -Itest/host -Wall -Wextra \
 *   -o /tmp/test_takeover test/test_takeover.c test/host_shim.c ga10b_dev.c \
 *   ga10b_pmu.c ga10b_pmem.c ga10b_fifo.c ga10b_mm.c ga10b_ce.c \
 *   ga10b_falcon.c ga10b_fw_parse.c
 * Run:   /tmp/test_takeover [fw_dir]   (default /Users/tony/work/gpu-fw/ga10b)
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ga10b.h"
#include "ga10b_regs.h"
#include "ga10b_fifo.h"
#include "ga10b_mm.h"
#include "host_shim.h"

static int fails;
#define EXPECT(cond) do { \
    if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } \
} while (0)

/* ---- hardware that survives the process: model state + BAR0 ---- */
#define BAR0_BYTES   0x01000000u
#define RL0          0x00800000u      /* gr0 + grcopy LCE0/1 */
#define RL1          0x00810000u      /* LCE2 */
#define CHRAM0       0x00820000u
#define CHRAM1       0x00830000u
#define RESET_GR     1u
#define N_CHRAM      2048u

#define PMU_CPUCTL   (GA10B_PMU_PRISCV_BASE + GA10B_PRISCV_CPUCTL)
#define PMU_RETCODE  (GA10B_PMU_PRISCV_BASE + GA10B_PRISCV_BR_RETCODE)
#define PMU_HWCFG2   (GA10B_PMU_FALCON_BASE + GA10B_FALCON_HWCFG2)
#define PMU_MBOX0    (GA10B_PMU_FALCON_BASE + GA10B_FALCON_MAILBOX0)
#define PMU_MBOX1    (GA10B_PMU_FALCON_BASE + GA10B_FALCON_MAILBOX1)
#define PMU_INSTBLK  0x0010a480u
#define PMU_LPQ_HEAD 0x0010a804u      /* physical queue 1 */
#define PMU_LPQ_TAIL 0x0010a824u
#define PMU_MSG_HEAD 0x0010a4c8u
#define FECS_SIG     0x00409280u
#define EXTMEM2      0x8060000000000000ULL

typedef struct {
    uint32_t boots;            /* RTOS boots */
    uint32_t wpr_inits;        /* INIT_WPR_REGION RPCs */
    uint32_t bootstraps;       /* BOOTSTRAP_GR_FALCONS RPCs */
    uint32_t rpc_errors;       /* malformed / unexpected RPCs */
    uint32_t bad_phys;         /* RTOS touched memory outside the segment */
    uint64_t inst_phys;        /* instance block the RTOS bound itself to */
    uint64_t ss_gpu_va;
    uint32_t fecs_fresh;       /* bootstrapped and not yet started */
    uint32_t fecs_starts;
    uint32_t ctx_owner;        /* uuid owning the context loaded on GR */
    uint32_t stale_saves;      /* contexts saved into a dead owner's memory */
    uint32_t pad[5];
} model_t;

static model_t* M;
static uint8_t* pmem;

/* ---- the RTOS's view of memory: physical, device-lifetime segment only ---- */
static uint8_t* phys(uint64_t pa, uint32_t len) {
    if (pa < SHIM_PMEM_PHY || pa + len > SHIM_PMEM_PHY + GA10B_PMEM_SIZE) {
        M->bad_phys++;
        return NULL;
    }
    return pmem + (pa - SHIM_PMEM_PHY);
}

/* walk the VM the RTOS bound itself to, as the hardware would */
static uint8_t* gpu_page(uint64_t va) {
    static const uint32_t lo[5] = {47, 38, 29, 21, 12};
    static const uint32_t ne[5] = {4, 512, 512, 256, 512};
    static const uint32_t ew[5] = {2, 2, 2, 4, 2};
    const uint32_t* inst = (const uint32_t*)phys(M->inst_phys, 4096);
    if (inst == NULL)
        return NULL;
    uint64_t tab = ((uint64_t)(inst[128] >> 12) << 12) |
                   ((uint64_t)inst[129] << 32);
    for (uint32_t l = 0; l < 5; l++) {
        const uint32_t* t = (const uint32_t*)phys(tab, 4096);
        if (t == NULL)
            return NULL;
        const uint32_t* e = t + ((va >> lo[l]) & (ne[l] - 1)) * ew[l];
        uint32_t w0 = (l == 3) ? e[2] : e[0];
        uint32_t w1 = (l == 3) ? e[3] : e[1];
        if (l == 4 ? !(w0 & GA10B_GMMU_PTE_VALID) : !ga10b_gmmu_pde_valid(w0)) {
            M->bad_phys++;
            return NULL;
        }
        tab = ga10b_gmmu_pde_pa(w0, w1);
    }
    return phys(tab, 4096);
}

static int gpu_copy(uint64_t va, void* buf, uint32_t len, int write) {
    uint8_t* b = buf;
    while (len != 0) {
        uint32_t in = 4096u - (uint32_t)(va & 4095u);
        if (in > len)
            in = len;
        uint8_t* p = gpu_page(va & ~4095ULL);
        if (p == NULL)
            return -1;
        if (write)
            memcpy(p + (va & 4095u), b, in);
        else
            memcpy(b, p + (va & 4095u), in);
        va += in;
        b += in;
        len -= in;
    }
    return 0;
}

static void pmu_post(const uint8_t* msg) {
    uint32_t head = ga10b.bar0[PMU_MSG_HEAD / 4];
    gpu_copy(M->ss_gpu_va + 0x10200u + head * 64u, (void*)msg, 64, 1);
    ga10b.bar0[PMU_MSG_HEAD / 4] = (head + 1u) % 16u;
}

static void pmu_boot(void) {
    uint64_t args = (((uint64_t)ga10b.bar0[PMU_MBOX1 / 4] << 32) |
                     ga10b.bar0[PMU_MBOX0 / 4]) - EXTMEM2;
    const uint8_t* a = phys(args, 184);
    if (a == NULL)
        return;
    uint32_t ss_lo, ss_hi, bind;
    memcpy(&ss_lo, a + 120 + 44, 4);      /* cmd_line_args.super_surface */
    memcpy(&ss_hi, a + 120 + 48, 4);
    memcpy(&bind, a + 120 + 60, 4);       /* ctx_bind_addr */
    M->inst_phys = (uint64_t)(bind & 0x0fffffffu) << 12;
    M->ss_gpu_va = ((uint64_t)ss_hi << 32) | ss_lo;
    uint32_t hdr[2] = { 0, 0 };
    if (gpu_copy(M->ss_gpu_va + 0x10600u, hdr, 8, 0) != 0)
        return;
    if (hdr[0] != ss_lo || hdr[1] != ss_hi)
        M->rpc_errors++;
    M->boots++;
    ga10b.bar0[PMU_RETCODE / 4] = 3;
    ga10b.bar0[PMU_HWCFG2 / 4] &= ~(1u << 13);
    ga10b.bar0[PMU_INSTBLK / 4] = bind;
    uint8_t init[64];
    memset(init, 0, sizeof(init));
    init[4] = 0x0f;                       /* INIT unit, func 0 */
    init[22] = 1;                         /* LPQ = physical queue 1 */
    init[38] = 0x00;
    init[39] = 0x08;                      /* sw_managed 0 + 2048 */
    pmu_post(init);
}

static void pmu_cmd(uint32_t pos) {
    uint8_t e[2048];
    uint64_t va = M->ss_gpu_va + 0x200u + 0x8000u + pos * 2048u;
    if (gpu_copy(va, e, sizeof(e), 0) != 0)
        return;
    uint8_t func = e[21];
    if (e[0] != pos || e[8] != 0x0a || e[12] != 0x80 || e[20] != 0x0a)
        M->rpc_errors++;
    if (func == 0x00) {
        if (M->wpr_inits != 0)
            M->rpc_errors++;              /* once per RTOS boot */
        M->wpr_inits++;
    } else if (func == 0x03) {
        if (M->wpr_inits == 0 || e[32] != 0x0c)
            M->rpc_errors++;
        M->bootstraps++;
        M->fecs_fresh = 1;
        ga10b.bar0[FECS_SIG / 4] = 0xaf88;
    } else {
        M->rpc_errors++;
    }
    e[23] = 0;                            /* flcn_status */
    gpu_copy(va + 23, e + 23, 1, 1);
    ga10b.bar0[PMU_LPQ_TAIL / 4] = ga10b.bar0[PMU_LPQ_HEAD / 4];
    uint8_t msg[64];
    memset(msg, 0, sizeof(msg));
    msg[4] = 0x0a;
    msg[7] = e[11];                       /* seq_id */
    pmu_post(msg);
}

/* ---- call-order log: MMIO trace index at which each stub ran ---- */
enum { EV_STAGE_INIT, EV_GR_ENABLE, EV_ACR_INIT, EV_ACR_BOOT, EV_GR_INIT,
       EV_GR_RESET, EV_N };
static int ev_at[EV_N];
static uint32_t ev_n[EV_N];
static void ev(int e) {
    if (ev_n[e]++ == 0)
        ev_at[e] = (int)shim_nwr;
}

static void gr_engine_stopped(void) {
    M->fecs_fresh = 0;
    M->ctx_owner = 0;
    ga10b.bar0[FECS_SIG / 4] = 0;
}

static void engine_hook(uint32_t reset_id, int enable) {
    if (reset_id == RESET_GR && !enable) {
        ev(EV_GR_RESET);
        gr_engine_stopped();
    }
}

static void wr_hook(uint32_t off, uint32_t val, uint32_t old) {
    if (off == PMU_CPUCTL && val == 1u) {
        pmu_boot();
    } else if (off == PMU_LPQ_HEAD) {
        pmu_cmd(old % 16u);
    } else if ((off >= CHRAM0 && off < CHRAM0 + 4u * N_CHRAM) ||
               (off >= CHRAM1 && off < CHRAM1 + 4u * N_CHRAM)) {
        uint32_t st = old;
        if (val == GA10B_CHRAM_ENABLE) {
            st |= GA10B_CHRAM_IN_USE;
            if (off < CHRAM1)
                M->ctx_owner = shim_self_uuid;
        } else if (val == GA10B_CHRAM_DISABLE) {
            st &= ~GA10B_CHRAM_IN_USE;
        } else if (val == GA10B_CHRAM_CLEAR) {
            st = 0;
        }
        ga10b.bar0[off / 4] = st;
    } else if (off == RL0 + GA10B_RL_PREEMPT) {
        if (M->ctx_owner != 0 && M->ctx_owner != shim_self_uuid)
            M->stale_saves++;
        M->ctx_owner = 0;
        ga10b.bar0[off / 4] = 0;          /* preempt completes */
    }
}

/* ---- power layer / ACR / GR stubs ---- */
int ga10b_power_on(void) { return 0; }
int ga10b_power_off(void) { return 0; }
int ga10b_detect(void) { ev(EV_STAGE_INIT); return 0; }
int ga10b_fuse_read(void) { return 0; }
int ga10b_gpc_clk_init(void) { return 0; }
int ga10b_mc_intr_mask(void) { return 0; }
int ga10b_priv_ring_init(void) { return 0; }
int ga10b_bus_init(void) { return 0; }
int ga10b_cg_early_init(void) { return 0; }
int ga10b_fb_ltc_early_init(void) { return 0; }
int ga10b_fb_init_hw(void) { return 0; }

int ga10b_acr_done(void) {
    return (ga10b_rd(PMU_RETCODE) & 3u) == 3u &&
           (ga10b_rd(PMU_HWCFG2) & (1u << 13)) == 0u;
}
int ga10b_acr_init(const char* fw_dir) { (void)fw_dir; ev(EV_ACR_INIT); return 0; }
int ga10b_acr_boot_gsp(void) { ev(EV_ACR_BOOT); return 0; }

/* gr_reset_engine: the GR engine (and with it FECS/GPCCS) restarts */
int ga10b_gr_enable_hw(const char* fw_dir) {
    (void)fw_dir;
    ev(EV_GR_ENABLE);
    gr_engine_stopped();
    return 0;
}

/* FECS reports INIT_DONE only when it was (re)bootstrapped since its last
 * start: the handshake the second process used to time out on */
int ga10b_gr_init(const char* fw_dir) {
    (void)fw_dir;
    ev(EV_GR_INIT);
    if (ga10b_rd(FECS_SIG) != 0xaf88 || !M->fecs_fresh)
        return -1;
    M->fecs_fresh = 0;
    M->fecs_starts++;
    return 0;
}

int ga10b_buf_alloc(ga10b_gmmu_t* mm, uint32_t size, void** va,
                    uint64_t* gpu_va, int cacheable) {
    (void)mm; (void)size; (void)va; (void)gpu_va; (void)cacheable;
    return -1;
}
ga10b_module_t* ga10b_module_load(ga10b_gmmu_t* mm, const void* image,
                                  uint32_t size) {
    (void)mm; (void)image; (void)size;
    return NULL;
}
int ga10b_kernel_launch(ga10b_chan_t* ch, ga10b_module_t* m, int kernel,
                        const uint32_t grid[3], const uint32_t block[3],
                        void* const* args) {
    (void)ch; (void)m; (void)kernel; (void)grid; (void)block; (void)args;
    return -1;
}
int ga10b_kernel_launch_dyn(ga10b_chan_t* ch, ga10b_module_t* m, int kernel,
                            const uint32_t grid[3], const uint32_t block[3],
                            void* const* args, uint32_t dyn_smem) {
    (void)ch; (void)m; (void)kernel; (void)grid; (void)block; (void)args;
    (void)dyn_smem;
    return -1;
}
int ga10b_kernel_queue_dyn(ga10b_chan_t* ch, ga10b_module_t* m, int kernel,
                           const uint32_t grid[3], const uint32_t block[3],
                           void* const* args, uint32_t dyn_smem) {
    (void)ch; (void)m; (void)kernel; (void)grid; (void)block; (void)args;
    (void)dyn_smem;
    return -1;
}
int ga10b_compute_drain(ga10b_chan_t* ch, uint32_t* retired) {
    (void)ch; (void)retired;
    return -1;
}
void ga10b_compute_timestamps(int enable) {
    (void)enable;
}
int ga10b_compute_timestamp(uint32_t slot, uint64_t* timestamp) {
    (void)slot; (void)timestamp;
    return -1;
}
int ga10b_kernel_launch_compute(ga10b_chan_t* ch, ga10b_module_t* m, int kernel,
                                const uint32_t grid[3], const uint32_t block[3],
                                const uint32_t* unif, uint32_t unif_dwords,
                                const uint64_t* ssbo_va, uint32_t nssbo,
                                const uint32_t* texdesc,
                                uint32_t texdesc_dwords) {
    (void)ch; (void)m; (void)kernel; (void)grid; (void)block; (void)unif;
    (void)unif_dwords; (void)ssbo_va; (void)nssbo;
    (void)texdesc; (void)texdesc_dwords;
    return -1;
}

/* ---- one generation ---- */
static const ga10b_devtable_t devtable = {
    .count = 4,
    .dev = {
        { GA10B_DEVTYPE_GRAPHICS, 0, 0x400000, RESET_GR, RL0, 0, 1 },
        { GA10B_DEVTYPE_LCE, 0, 0x104000, 2, RL0, 1, 1 },
        { GA10B_DEVTYPE_LCE, 1, 0x104080, 3, RL0, 2, 1 },
        { GA10B_DEVTYPE_LCE, 2, 0x104100, 4, RL1, 0, 1 },
    },
};

static void* map_file(const char* path, size_t size) {
    int fd = open(path, O_RDWR);
    if (fd < 0)
        return NULL;
    void* p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return p == MAP_FAILED ? NULL : p;
}

static uint32_t chram_in_use(void) {
    uint32_t n = 0;
    for (uint32_t c = 0; c < N_CHRAM; c++)
        n += (ga10b.bar0[(CHRAM0 + 4u * c) / 4] != 0u) +
             (ga10b.bar0[(CHRAM1 + 4u * c) / 4] != 0u);
    return n;
}

static int first_wr(uint32_t off) {
    for (uint32_t i = 0; i < shim_nwr; i++)
        if (shim_trace[i].off == off)
            return (int)i;
    return -1;
}

/* bring the GPU up like ga10b_dev_open and leave one channel on gr0 */
static int bring_up(const char* fw, ga10b_gmmu_t* mm, ga10b_chan_t* ch) {
    if (ga10b_stage_gr(fw) != 0)
        return -1;
    if (ga10b_gmmu_init(mm) != 0 || ga10b_fifo_init() != 0)
        return -1;
    return ga10b_chan_create(mm, ch, GA10B_DEVTYPE_GRAPHICS, 0);
}

static int generation(const char* step, const char* hw_path,
                      const char* pmem_path, const char* fw) {
    uint8_t* hw = map_file(hw_path, 4096 + BAR0_BYTES);
    pmem = map_file(pmem_path, GA10B_PMEM_SIZE);
    if (hw == NULL || pmem == NULL) {
        printf("FAIL: cannot map %s / %s\n", hw_path, pmem_path);
        return 1;
    }
    M = (model_t*)hw;
    ga10b.bar0 = (volatile uint32_t*)(hw + 4096);
    ga10b.hw = 1;
    ga10b.mapped = 1;
    shim_pmem = pmem;
    shim_devtable = &devtable;
    shim_wr_hook = wr_hook;
    shim_engine_hook = engine_hook;

    static ga10b_gmmu_t mm;
    static ga10b_chan_t ch;
    model_t before = *M;

    if (strcmp(step, "cold") == 0 || strcmp(step, "cold-killed") == 0) {
        /* power-on state: PMU held by its BROM, nothing scheduled */
        ga10b.bar0[PMU_HWCFG2 / 4] = 1u << 13;
        ga10b.bar0[(RL0 + GA10B_RL_CHANNEL_CONFIG) / 4] = CHRAM0 | 0xbu;
        ga10b.bar0[(RL1 + GA10B_RL_CHANNEL_CONFIG) / 4] = CHRAM1 | 0xbu;
        EXPECT(bring_up(fw, &mm, &ch) == 0);
        EXPECT(M->boots == 1 && M->wpr_inits == 1 && M->bootstraps == 1);
        EXPECT(M->fecs_starts == 1);
        EXPECT(ev_n[EV_ACR_INIT] == 1 && ev_n[EV_ACR_BOOT] == 1);
        EXPECT(ev_n[EV_GR_RESET] == 0);           /* no takeover on a cold GPU */
        /* GR reset precedes the ACR, the PMU RPCs follow it */
        EXPECT(ev_at[EV_GR_ENABLE] <= ev_at[EV_ACR_INIT]);
        EXPECT(first_wr(PMU_LPQ_HEAD) >= ev_at[EV_ACR_BOOT]);
        EXPECT(chram_in_use() == 1);
        if (strcmp(step, "cold-killed") == 0) {
            EXPECT(M->bad_phys == 0 && M->rpc_errors == 0);
            fflush(stdout);
            _exit(fails != 0);                    /* no exit handlers */
        }
    } else if (strcmp(step, "next") == 0 || strcmp(step, "next-killed") == 0) {
        int killed = strcmp(step, "next-killed") == 0;
        /* a cleanly exited owner took its channel off the hardware itself */
        EXPECT(chram_in_use() == (killed ? 1u : 0u));
        EXPECT((M->ctx_owner != 0) == killed);
        EXPECT(bring_up(fw, &mm, &ch) == 0);
        /* attached to the running RTOS: no boot, no ACR, no second WPR init */
        EXPECT(M->boots == before.boots);
        EXPECT(M->wpr_inits == before.wpr_inits);
        EXPECT(ev_n[EV_ACR_INIT] == 0 && ev_n[EV_ACR_BOOT] == 0);
        EXPECT(shim_count_off(PMU_CPUCTL) == 0);
        /* nvgpu_gr_reset: engine reset, then the PMU bootstraps FECS again */
        EXPECT(M->bootstraps == before.bootstraps + 1);
        EXPECT(M->fecs_starts == before.fecs_starts + 1);
        EXPECT(ev_n[EV_GR_ENABLE] == 1);
        EXPECT(first_wr(PMU_LPQ_HEAD) > ev_at[EV_GR_ENABLE]);
        EXPECT(ev_at[EV_GR_INIT] > first_wr(PMU_LPQ_HEAD));
        /* takeover runs before anything else touches the hardware:
         * scheduler off -> engine reset -> preempt -> scheduler on */
        int i_off = shim_find_from(0, RL0 + GA10B_RL_SCHED_DISABLE, 1);
        int i_pre = first_wr(RL0 + GA10B_RL_PREEMPT);
        int i_on = shim_find_from(0, RL0 + GA10B_RL_SCHED_DISABLE, 0);
        EXPECT(i_off == 0);
        EXPECT(ev_n[EV_GR_RESET] >= 1 && ev_at[EV_GR_RESET] > i_off);
        EXPECT(i_pre >= ev_at[EV_GR_RESET] && i_on > i_pre);
        EXPECT(ev_at[EV_STAGE_INIT] > i_on);
        if (killed) {
            int i_dis = shim_find_from(0, CHRAM0, GA10B_CHRAM_DISABLE);
            int i_clr = shim_find_from(0, CHRAM0, GA10B_CHRAM_CLEAR);
            EXPECT(i_dis > i_off && i_dis < ev_at[EV_GR_RESET]);
            EXPECT(i_clr > i_pre && i_clr < i_on);
        }
        EXPECT(chram_in_use() == 1);              /* only the new channel */
        EXPECT(M->ctx_owner == shim_self_uuid);
    } else if (strcmp(step, "refused") == 0) {
        /* the owner is alive: refuse before touching the hardware */
        EXPECT(ga10b_stage_gr(fw) != 0);
        EXPECT(shim_nwr == 0);
        EXPECT(memcmp(&before, M, sizeof(before)) == 0);
        ga10b_pmem_hdr_t* h = ga10b_pmem_hdr();
        EXPECT(h != NULL && h->owner_uuid == shim_alive_uuid);
    } else if (strcmp(step, "lost") == 0) {
        /* RTOS running, its memory record gone: fail, leave the RTOS alone */
        EXPECT(ga10b_stage_gr(fw) != 0);
        EXPECT(M->boots == before.boots && M->bootstraps == before.bootstraps);
        EXPECT(shim_count_off(PMU_CPUCTL) == 0);
        EXPECT(shim_count_off(PMU_LPQ_HEAD) == 0);
        EXPECT(ev_n[EV_GR_INIT] == 0);
    } else {
        printf("FAIL: unknown step %s\n", step);
        return 1;
    }
    EXPECT(M->bad_phys == 0);
    EXPECT(M->rpc_errors == 0);
    EXPECT(M->stale_saves == 0);
    fflush(stdout);
    exit(fails != 0);                             /* exit handlers run */
}

/* ---- orchestrator ---- */
static const char* self;
static const char* fw_dir = "/Users/tony/work/gpu-fw/ga10b";
static char hw_path[64], pmem_path[64];

static int make_file(char* path, size_t size) {
    int fd = mkstemp(path);
    if (fd < 0 || ftruncate(fd, (off_t)size) != 0)
        return -1;
    close(fd);
    return 0;
}

static void run(const char* step, uint32_t uuid, uint32_t alive) {
    char u[16], a[16];
    snprintf(u, sizeof(u), "%u", uuid);
    snprintf(a, sizeof(a), "%u", alive);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        execl(self, self, step, hw_path, pmem_path, fw_dir, u, a, (char*)NULL);
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    printf("  %-12s uuid %u: %s\n", step, uuid, ok ? "ok" : "FAILED");
    if (!ok)
        fails++;
}

static void scenario(const char* name) {
    strcpy(hw_path, "/tmp/ga10b_hw_XXXXXX");
    strcpy(pmem_path, "/tmp/ga10b_pmem_XXXXXX");
    if (make_file(hw_path, 4096 + BAR0_BYTES) != 0 ||
        make_file(pmem_path, GA10B_PMEM_SIZE) != 0) {
        printf("FAIL: temp files\n");
        exit(1);
    }
    printf("%s\n", name);
}

static void scenario_end(void) {
    unlink(hw_path);
    unlink(pmem_path);
}

int main(int argc, char** argv) {
    if (argc == 7) {
        shim_self_pid = (uint32_t)atoi(argv[5]);
        shim_self_uuid = (uint32_t)atoi(argv[5]);
        shim_alive_uuid = (uint32_t)atoi(argv[6]);
        return generation(argv[1], argv[2], argv[3], argv[4]);
    }
    self = argv[0];
    if (argc > 1)
        fw_dir = argv[1];

    scenario("owners exit cleanly, three processes back to back");
    run("cold", 11, 0);
    run("next", 12, 0);
    run("next", 13, 0);
    scenario_end();

    scenario("owners are killed with a channel still scheduled");
    run("cold-killed", 21, 0);
    run("next-killed", 22, 0);
    scenario_end();

    scenario("second process while the owner is alive");
    run("cold-killed", 31, 0);
    run("refused", 32, 31);
    run("next-killed", 33, 0);
    scenario_end();

    scenario("RTOS running but the device-lifetime segment was lost");
    run("cold", 41, 0);
    int fd = open(pmem_path, O_WRONLY | O_TRUNC);
    if (fd < 0 || ftruncate(fd, GA10B_PMEM_SIZE) != 0)
        fails++;
    if (fd >= 0)
        close(fd);
    run("lost", 42, 0);
    scenario_end();

    printf("test_takeover: %s\n", fails ? "FAILURES" : "ALL PASS");
    return fails != 0;
}
