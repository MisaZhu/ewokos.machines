/*
 * test_gpud.c -- host dry-run of the resident GPU daemon (gpud) and the
 * libga10br remote client, wired back-to-back in ONE process:
 *
 *   ga10b_dev_remote.c  --RPC(proto)-->  gpud.c  -->  driver stubs
 *
 * The real proto.c serialization is used; shm/dma/proc/vdevice are host
 * shims (test/host_gpud), and every ga10b driver entry the daemon calls is
 * a recording stub.  This tests the daemon logic only -- no GPU model:
 *   - RPC struct sizes fit the 512-byte inline proto buffer
 *   - open / idempotent re-open / per-client VA windows
 *   - map validation, LRU eviction, cache hits without RPC, remap old_size
 *   - alloc/free ownership, module load/find/info caching
 *   - launch param packing consistency (client packs at cbank0 offsets,
 *     daemon rebuilds args[] into the same blob) and compute payload layout
 *   - queued launches: one RPC per batch, submission order, automatic drain
 *     at the cap and before every call that could observe a queued launch,
 *     failure reporting, malformed batch records
 *   - memory-clock floor: one request per busy period, dropped when idle
 *   - session death sweep, pid-reuse guard, teardown completeness
 *
 * Build (from libs/ga10b):
 *   cc -I. -Iinclude -I../bsp/include -Itest/host_gpud -Itest/host \
 *     -I$EWOK/system/basic/libc/libewoksys/include \
 *     -I$EWOK/system/basic/libc/libewoksys/ewoksys/include \
 *     -I$EWOK/kernel/kernel/include \
 *     -Wall -Wextra -o /tmp/test_gpud test/test_gpud.c \
 *     $EWOK/system/basic/libc/libewoksys/ewoksys/src/proto.c
 * Run: /tmp/test_gpud
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/shm.h>

#include <ewoksys/proc.h>
#include <ewoksys/proto.h>
#include <ga10b/ga10b_dev.h>
#include <ga10b/ga10b_rpc.h>

static int fails;
#define EXPECT(cond) do { \
    if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } \
} while (0)

/* ---------------- host shims ---------------- */

static int g_quiet = 1;
int klog(const char* fmt, ...) {
    if (g_quiet) return 0;
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("%s", buf);
    return n;
}

/* dma: malloc-backed, fake phys well above 4GB */
#define DMA_MAX 256
static struct { void* va; uint64_t phy; uint32_t size; } g_dma[DMA_MAX];
static int g_dma_live;
static uint64_t g_dma_phy_next = 0x200000000ULL;

ewokos_addr_t dma_alloc(uint32_t size) {
    for (int i = 0; i < DMA_MAX; i++) {
        if (g_dma[i].va != NULL) continue;
        void* p = malloc(size);
        if (p == NULL) return 0;
        g_dma[i].va = p;
        g_dma[i].phy = g_dma_phy_next;
        g_dma[i].size = size;
        g_dma_phy_next += (size + 0xfffffULL) & ~0xfffffULL;
        g_dma_live++;
        return (ewokos_addr_t)p;
    }
    return 0;
}
void dma_free(ewokos_addr_t va) {
    for (int i = 0; i < DMA_MAX; i++) {
        if (g_dma[i].va == (void*)va) {
            free(g_dma[i].va);
            memset(&g_dma[i], 0, sizeof(g_dma[i]));
            g_dma_live--;
            return;
        }
    }
    EXPECT(!"dma_free of unknown va");
}
ewokos_addr_t dma_phy_addr(ewokos_addr_t va) {
    for (int i = 0; i < DMA_MAX; i++)
        if (g_dma[i].va == (void*)va) return g_dma[i].phy;
    return 0;
}

/* shm: one shared buffer per segment, attach-counted RMID */
#define SHM_MAX 64
static struct {
    key_t key; void* buf; uint32_t size; int attaches; int rmid;
} g_shm[SHM_MAX];
static int g_shm_live;

static void shm_reap(int i) {
    if (g_shm[i].rmid && g_shm[i].attaches == 0 && g_shm[i].buf != NULL) {
        free(g_shm[i].buf);
        memset(&g_shm[i], 0, sizeof(g_shm[i]));
        g_shm_live--;
    }
}
int shmget(key_t key, int size, int flag) {
    if (key != IPC_PRIVATE) {
        for (int i = 0; i < SHM_MAX; i++)
            if (g_shm[i].buf != NULL && g_shm[i].key == key)
                return (flag & IPC_EXCL) ? -1 : i + 1;
    }
    for (int i = 0; i < SHM_MAX; i++) {
        if (g_shm[i].buf != NULL) continue;
        g_shm[i].buf = calloc(1, size);
        if (g_shm[i].buf == NULL) return -1;
        g_shm[i].key = key;
        g_shm[i].size = size;
        g_shm_live++;
        return i + 1;
    }
    return -1;
}
void* shmat(int shmid, const void* addr, int flag) {
    (void)addr; (void)flag;
    int i = shmid - 1;
    if (shmid <= 0 || i >= SHM_MAX || g_shm[i].buf == NULL) return (void*)-1;
    g_shm[i].attaches++;
    return g_shm[i].buf;
}
int shmdt(void* p) {
    for (int i = 0; i < SHM_MAX; i++) {
        if (g_shm[i].buf == p) {
            g_shm[i].attaches--;
            shm_reap(i);
            return 0;
        }
    }
    return -1;
}
int shmctl(int shmid, int cmd, void* buf) {
    (void)buf;
    int i = shmid - 1;
    if (shmid <= 0 || i >= SHM_MAX || g_shm[i].buf == NULL) return -1;
    if (cmd == IPC_RMID) { g_shm[i].rmid = 1; shm_reap(i); return 0; }
    return -1;
}

/* proc identity: one live client uuid the test controls */
#define UUID_A 0xaaaa1111u
static uint32_t g_client_uuid = UUID_A;
uint32_t proc_get_uuid(int32_t pid) { (void)pid; return g_client_uuid; }
static int32_t g_worker_pid;   /* nonzero: this pid is a thread of getpid() */
int proc_info(int32_t pid, procinfo_t* info) {
    if (g_worker_pid != 0 && pid == g_worker_pid) {
        info->type = TASK_TYPE_THREAD;
        info->father_pid = (int32_t)getpid();
    } else {
        info->type = TASK_TYPE_PROC;
        info->father_pid = 0;
    }
    return 0;
}

/* liveness table the test controls */
#define ALIVE_MAX 8
static struct { uint32_t pid, uuid; int live; } g_alive[ALIVE_MAX];
static void alive_set(uint32_t pid, uint32_t uuid, int live) {
    for (int i = 0; i < ALIVE_MAX; i++) {
        if (g_alive[i].pid == pid || g_alive[i].pid == 0) {
            g_alive[i].pid = pid; g_alive[i].uuid = uuid;
            g_alive[i].live = live;
            return;
        }
    }
    EXPECT(!"alive table full");
}

/* ---------------- driver stubs (daemon side) ---------------- */

static int g_drv_open_calls;
static int g_map_at_calls, g_unmap_calls, g_modfree_calls;
static int g_map_at_after, g_unmap_after;
static int launches_seen(void);
static int g_copy_calls, g_sync_gpu_calls, g_sync_cpu_calls;
static struct { uint64_t va, phy; uint32_t size, old_size; } g_last_map_at;
static uint64_t g_persist_bump = 0x10000000ULL;
static int g_persist_live;

typedef struct {
    char name[GA10B_RPC_NAME_MAX];
    ga10b_rpc_kinfo_rep_t info;
} fake_kernel_t;
typedef struct {
    int nkernels;
    fake_kernel_t ks[4];
    uint8_t image[64];
    uint32_t image_size;
} fake_mod_t;

static fake_mod_t* fake_mod_new(const void* image, uint32_t size) {
    fake_mod_t* m = calloc(1, sizeof(*m));
    if (m == NULL) return NULL;
    m->image_size = size < sizeof(m->image) ? size : sizeof(m->image);
    memcpy(m->image, image, m->image_size);
    m->nkernels = 2;
    strcpy(m->ks[0].name, "vecadd");
    m->ks[0].info.nparams = 3;
    m->ks[0].info.param_total = 0x54;
    m->ks[0].info.off[0] = 0x40; m->ks[0].info.sz[0] = 8;
    m->ks[0].info.off[1] = 0x48; m->ks[0].info.sz[1] = 8;
    m->ks[0].info.off[2] = 0x50; m->ks[0].info.sz[2] = 4;
    strcpy(m->ks[1].name, "scale");
    m->ks[1].info.nparams = 1;
    m->ks[1].info.param_total = 0x44;
    m->ks[1].info.off[0] = 0x40; m->ks[1].info.sz[0] = 4;
    return m;
}

static int drv_open(const char* fw_dir) { (void)fw_dir; g_drv_open_calls++; return 0; }
int ga10b_os_alive(uint32_t pid, uint32_t uuid) {
    for (int i = 0; i < ALIVE_MAX; i++)
        if (g_alive[i].pid == pid)
            return g_alive[i].live && g_alive[i].uuid == uuid;
    return 0;
}
uint64_t ga10b_devd_map(uint64_t phy, uint32_t size) {
    if (phy == 0 || (phy & 0xfff) != 0 || size == 0) return 0;
    uint64_t va = g_persist_bump;
    uint64_t next = va + ((size + 4095u) & ~4095u);
    if (next > GA10B_RPC_PERSIST_CAP) return 0;
    g_persist_bump = next;
    g_persist_live++;
    return va;
}
void ga10b_devd_unmap(uint64_t va, uint32_t size) {
    (void)va; (void)size;
    g_unmap_calls++;
    g_unmap_after = launches_seen();
    if (va < GA10B_RPC_WINDOW_BASE && g_persist_live > 0)
        g_persist_live--;
}
int ga10b_devd_map_at(uint64_t va, uint64_t phy, uint32_t size,
                      uint32_t old_size) {
    g_map_at_calls++;
    g_map_at_after = launches_seen();
    g_last_map_at.va = va; g_last_map_at.phy = phy;
    g_last_map_at.size = size; g_last_map_at.old_size = old_size;
    return 0;
}
static ga10b_module_t* drv_module(const void* cubin, uint32_t size) {
    return (ga10b_module_t*)fake_mod_new(cubin, size);
}
void ga10b_dev_module_free(ga10b_module_t* m) {
    g_modfree_calls++;
    free(m);
}
static int drv_module_kernel(const ga10b_module_t* mm, const char* name) {
    fake_mod_t* m = (fake_mod_t*)mm;
    for (int i = 0; i < m->nkernels; i++)
        if (strcmp(m->ks[i].name, name) == 0) return i;
    return -1;
}
void ga10b_devd_mm_stat(uint32_t* tables, uint32_t* free_ranges,
                        uint64_t* va_next) {
    *tables = 7; *free_ranges = 3; *va_next = 0x412345000ULL;
}
const char* ga10b_devd_kernel_name(const ga10b_module_t* mm, int kid) {
    const fake_mod_t* m = (const fake_mod_t*)mm;
    return (kid < 0 || kid >= m->nkernels) ? NULL : m->ks[kid].name;
}
int ga10b_devd_kernel_info(ga10b_module_t* mm, int kid,
                           ga10b_rpc_kinfo_rep_t* out) {
    fake_mod_t* m = (fake_mod_t*)mm;
    if (kid < 0 || kid >= m->nkernels) return -1;
    *out = m->ks[kid].info;
    return 0;
}

/* launch capture: what the driver would actually read */
static struct {
    int called;
    uint32_t grid[3], block[3], dyn_smem;
    uint8_t args[GA10B_RPC_MAX_PARAMS][64];
    uint32_t nargs;
} g_launch;
static int launches_seen(void) { return g_launch.called; }
/* per-launch history (order + params) and failure injection */
#define HIST_MAX 512
static struct { int kid; uint32_t grid0, dyn_smem, arg0; } g_hist[HIST_MAX];
static int g_hist_n;
static int g_launch_fail_on;   /* nonzero: that g_launch.called value fails */
uint64_t g_fake_nsec;          /* the daemon's clock (host_gpud kernel_tic.h) */
/* BPMP memory-clock floor of the GPU client */
static int g_memclk_calls, g_memclk_state, g_memclk_fail;
int bsp_bpmp_init(void) { return 0; }
int bsp_bpmp_gpu_memclk_hold(int hold) {
    g_memclk_calls++;
    if (g_memclk_fail) return -1;
    g_memclk_state = hold;
    return 0;
}
static uint64_t g_launch_ns;   /* fake run time of one launch */
static int drv_launch_dyn(ga10b_module_t* mm, int kid, const uint32_t grid[3],
                          const uint32_t block[3], void* const* args,
                          uint32_t dyn_smem) {
    fake_mod_t* m = (fake_mod_t*)mm;
    if (kid < 0 || kid >= m->nkernels) return -1;
    const ga10b_rpc_kinfo_rep_t* ki = &m->ks[kid].info;
    g_launch.called++;
    g_fake_nsec += g_launch_ns;
    if (g_launch_fail_on != 0 && g_launch.called == g_launch_fail_on)
        return -1;
    if (g_hist_n < HIST_MAX) {
        g_hist[g_hist_n].kid = kid;
        g_hist[g_hist_n].grid0 = grid[0];
        g_hist[g_hist_n].dyn_smem = dyn_smem;
        memcpy(&g_hist[g_hist_n].arg0, args[0], 4);
        g_hist_n++;
    }
    memcpy(g_launch.grid, grid, sizeof(g_launch.grid));
    memcpy(g_launch.block, block, sizeof(g_launch.block));
    g_launch.dyn_smem = dyn_smem;
    g_launch.nargs = ki->nparams;
    for (uint32_t i = 0; i < ki->nparams; i++)
        memcpy(g_launch.args[i], args[i], ki->sz[i]);
    return 0;
}
/* queued launches: recorded when queued, retired by the drain */
#define STUB_QUEUE_SLOTS 8
static int g_q_pending, g_q_calls, g_drain_calls;
static int g_queue_timestamps;
static uint64_t g_gpu_ts;
static int g_q_refuse_kid = -1;   /* this kernel never fits a queue slot */
static int g_drain_short;         /* nonzero: next drain retires that many less */
static int g_busy_violations;     /* device touched with launches queued */
static int drv_launch_queue(ga10b_module_t* mm, int kid, const uint32_t grid[3],
                            const uint32_t block[3], void* const* args,
                            uint32_t dyn_smem) {
    if (kid == g_q_refuse_kid || g_q_pending == STUB_QUEUE_SLOTS)
        return 1;
    g_q_calls++;
    int rc = drv_launch_dyn(mm, kid, grid, block, args, dyn_smem);
    g_fake_nsec -= g_launch_ns;   /* a queued launch runs while it is drained */
    if (rc != 0)
        return -1;
    g_q_pending++;
    return 0;
}
static int drv_launch_drain(uint32_t* retired) {
    g_drain_calls++;
    int n = g_q_pending, lost = g_drain_short < n ? g_drain_short : n;
    g_fake_nsec += (uint64_t)n * g_launch_ns;
    g_q_pending = 0;
    g_drain_short = 0;
    *retired = (uint32_t)(n - lost);
    return lost != 0 ? -1 : 0;
}
static void drv_queue_timestamps(int enable) {
    g_queue_timestamps = enable;
}
static int drv_queue_timestamp(uint32_t slot, uint64_t* timestamp) {
    (void)slot;
    if (!g_queue_timestamps)
        return -1;
    g_gpu_ts += g_launch_ns != 0 ? g_launch_ns : 1000u;
    *timestamp = g_gpu_ts;
    return 0;
}
static int drv_launch_sync(ga10b_module_t* mm, int kid, const uint32_t grid[3],
                           const uint32_t block[3], void* const* args,
                           uint32_t dyn_smem) {
    if (g_q_pending != 0)
        g_busy_violations++;
    return drv_launch_dyn(mm, kid, grid, block, args, dyn_smem);
}
static struct {
    int called;
    uint32_t unif[64], nunif;
    uint64_t ssbo[8]; uint32_t nssbo;
    uint32_t tex[64], ntex;
    int launches_before;
} g_launchc;
static int drv_launch_compute(ga10b_module_t* mm, int kid,
                              const uint32_t grid[3], const uint32_t block[3],
                              const uint32_t* unif, uint32_t unif_dwords,
                              const uint64_t* ssbo, uint32_t nssbo,
                              const uint32_t* tex, uint32_t tex_dwords) {
    (void)mm; (void)kid; (void)grid; (void)block;
    g_launchc.called++;
    g_launchc.launches_before = g_launch.called;
    g_launchc.nunif = unif_dwords;
    memcpy(g_launchc.unif, unif, unif_dwords * 4);
    g_launchc.nssbo = nssbo;
    memcpy(g_launchc.ssbo, ssbo, nssbo * 8);
    g_launchc.ntex = tex_dwords;
    memcpy(g_launchc.tex, tex, tex_dwords * 4);
    return 0;
}
/* g_launch.called as seen by the last copy / sync / unmap / map_at: proves
 * the queued launches ran first */
static int g_copy_after, g_sync_gpu_after, g_sync_cpu_after;
static int drv_copy(uint64_t d, uint64_t s, uint32_t n) {
    (void)d; (void)s; (void)n; g_copy_calls++;
    g_copy_after = g_launch.called;
    return 0;
}
static int drv_sync_to_gpu(void) {
    g_sync_gpu_calls++; g_sync_gpu_after = g_launch.called; return 0;
}
static int drv_sync_to_cpu(void) {
    if (g_q_pending != 0)
        g_busy_violations++;
    g_sync_cpu_calls++; g_sync_cpu_after = g_launch.called; return 0;
}

/* ---------------- the code under test ---------------- */

/* the daemon's driver calls bind to the recording stubs above, NOT to the
 * remote client definitions included right after it */
#define main                      gpud_main_unused
#define ga10b_dev_open            drv_open
#define ga10b_dev_module          drv_module
#define ga10b_module_kernel       drv_module_kernel
#define ga10b_dev_launch_dyn      drv_launch_sync
#define ga10b_dev_launch_queue    drv_launch_queue
#define ga10b_dev_launch_drain    drv_launch_drain
#define ga10b_dev_queue_timestamps drv_queue_timestamps
#define ga10b_dev_queue_timestamp drv_queue_timestamp
#define ga10b_dev_launch_compute  drv_launch_compute
#define ga10b_dev_copy            drv_copy
#define ga10b_dev_sync_to_gpu     drv_sync_to_gpu
#define ga10b_dev_sync_to_cpu     drv_sync_to_cpu
#include "../../../drivers/gpud/gpud.c"
#undef main
#undef ga10b_dev_open
#undef ga10b_dev_module
#undef ga10b_module_kernel
#undef ga10b_dev_launch_dyn
#undef ga10b_dev_launch_queue
#undef ga10b_dev_launch_drain
#undef ga10b_dev_queue_timestamps
#undef ga10b_dev_queue_timestamp
#undef ga10b_dev_launch_compute
#undef ga10b_dev_copy
#undef ga10b_dev_sync_to_gpu
#undef ga10b_dev_sync_to_cpu

#include "../ga10b_dev_remote.c"

/* vdevice loopback: the remote client's RPC lands in the daemon handler */
#define GPUD_PID 777
int device_run(vdevice_t* dev, const char* mnt, int type, int mode, bool mt) {
    (void)dev; (void)mnt; (void)type; (void)mode; (void)mt;
    return 0;
}
int dev_get_pid(const char* fname) {
    return strcmp(fname, GA10B_RPC_DEV) == 0 ? GPUD_PID : -1;
}
static int g_rpc_calls;
int dev_cntl_by_pid(int pid, int cmd, proto_t* in, proto_t* out) {
    EXPECT(pid == GPUD_PID);
    g_rpc_calls++;
    return gpud_dev_cntl(NULL, (int)getpid(), cmd, in, out, NULL);
}

/* direct daemon call helper for second-client / malformed-request tests */
static int daemon_call(int from_pid, int cmd, const void* req, uint32_t sz,
                       void* rep, uint32_t repsz) {
    proto_t in, out;
    PF->init(&in);
    if (req != NULL) PF->add(&in, req, sz);
    PF->init(&out);
    int rc = gpud_dev_cntl(NULL, from_pid, cmd, &in, &out, NULL);
    if (rc == 0 && rep != NULL) {
        int32_t got = 0;
        void* d = proto_read(&out, &got);
        EXPECT(d != NULL && got >= (int32_t)repsz);
        if (d != NULL && got >= (int32_t)repsz) memcpy(rep, d, repsz);
    }
    PF->clear(&in);
    PF->clear(&out);
    return rc;
}

/* ---------------- tests ---------------- */

static void test_proto_sizes(void) {
    /* every wire struct must fit the inline proto buffer with headroom */
    EXPECT(sizeof(ga10b_rpc_open_t) <= 480);
    EXPECT(sizeof(ga10b_rpc_open_rep_t) <= 480);
    EXPECT(sizeof(ga10b_rpc_map_t) <= 480);
    EXPECT(sizeof(ga10b_rpc_alloc_t) <= 480);
    EXPECT(sizeof(ga10b_rpc_alloc_rep_t) <= 480);
    EXPECT(sizeof(ga10b_rpc_free_t) <= 480);
    EXPECT(sizeof(ga10b_rpc_copy_t) <= 480);
    EXPECT(sizeof(ga10b_rpc_sync_t) <= 480);
    EXPECT(sizeof(ga10b_rpc_module_t) <= 480);
    EXPECT(sizeof(ga10b_rpc_kfind_t) <= 480);
    EXPECT(sizeof(ga10b_rpc_kinfo_t) <= 480);
    EXPECT(sizeof(ga10b_rpc_kinfo_rep_t) <= 480);
    EXPECT(sizeof(ga10b_rpc_launch_t) <= 480);
    EXPECT(sizeof(ga10b_rpc_launchc_t) <= 480);
    EXPECT(sizeof(ga10b_rpc_batch_t) <= 480);
    EXPECT(sizeof(ga10b_rpc_batch_rep_t) <= 480);
    /* a full batch of maximal records fits the payload shm */
    EXPECT(GA10B_RPC_BATCH_MAX * (sizeof(ga10b_rpc_launch_t) + 0x10000u)
           > GA10B_RPC_REQ_SHM_SIZE);   /* so the byte-limit path is real */
    /* window layout invariants */
    EXPECT(GA10B_RPC_WINDOW_BASE == GA10B_RPC_PERSIST_CAP);
    EXPECT(GA10B_RPC_WINDOW_SPAN ==
           (uint64_t)GA10B_RPC_SLOTS * GA10B_RPC_SLOT_SPAN);
}

static void test_ping_open(void) {
    int32_t ver = 0;
    EXPECT(daemon_call(1, GA10B_RPC_PING, NULL, 0, &ver, sizeof(ver)) == 0);
    EXPECT(ver == GA10B_RPC_VERSION);

    alive_set((uint32_t)getpid(), UUID_A, 1);
    EXPECT(ga10b_dev_open(NULL) == 0);
    EXPECT(g_drv_open_calls == 1);
    EXPECT(ga10b_dev_ready() == 1);
    EXPECT(r.slot_base == GA10B_RPC_WINDOW_BASE);
    /* idempotent: no second bring-up, same window */
    EXPECT(ga10b_dev_open(NULL) == 0);
    EXPECT(g_drv_open_calls == 1);

    /* malformed open (direct): bad uuid / shm */
    ga10b_rpc_open_t bad;
    memset(&bad, 0, sizeof(bad));
    bad.shm_id = 1; bad.shm_size = 4096;
    EXPECT(daemon_call(42, GA10B_RPC_OPEN, &bad, sizeof(bad), NULL, 0)
           == GA10B_RPC_E_INVAL);
    /* wrong wire size is rejected */
    EXPECT(daemon_call((int)getpid(), GA10B_RPC_MAP, &bad, sizeof(bad) - 1,
                       NULL, 0) == GA10B_RPC_E_INVAL);
}

static void test_map_lru(void) {
    uint64_t phy = 0x100000;
    uint64_t va = ga10b_dev_map(phy, 4096);
    EXPECT(va == r.slot_base);
    EXPECT(g_map_at_calls == 1);
    EXPECT(g_last_map_at.phy == phy && g_last_map_at.size == 4096 &&
           g_last_map_at.old_size == 0);
    /* cache hit: same phy, fits -> no RPC */
    EXPECT(ga10b_dev_map(phy, 4096) == va);
    EXPECT(g_map_at_calls == 1);
    /* same phy but larger: mirrors the local driver -- a fresh LRU slot
     * (the first empty one), NOT an in-place grow */
    EXPECT(ga10b_dev_map(phy, 8192) == r.slot_base + GA10B_RPC_SLOT_SPAN);
    EXPECT(g_map_at_calls == 2);
    EXPECT(g_last_map_at.old_size == 0 && g_last_map_at.size == 8192);
    /* invalid inputs */
    EXPECT(ga10b_dev_map(0, 4096) == 0);
    EXPECT(ga10b_dev_map(0x100001, 4096) == 0);       /* unaligned phy */
    EXPECT(ga10b_dev_map(phy, GA10B_RPC_SLOT_SPAN + 1) == 0);

    /* fill the remaining 62 slots, then evict the LRU (slot 0) */
    for (uint32_t i = 2; i < GA10B_RPC_SLOTS; i++)
        EXPECT(ga10b_dev_map(0x200000 + i * 0x1000, 4096) ==
               r.slot_base + i * GA10B_RPC_SLOT_SPAN);
    int before = g_map_at_calls;
    uint64_t ev = ga10b_dev_map(0x900000, 4096);
    EXPECT(ev == r.slot_base);                        /* slot 0 was LRU */
    EXPECT(g_map_at_calls == before + 1);
    EXPECT(g_last_map_at.old_size == 4096);
    /* daemon-side window guard (direct): foreign window / unaligned slot */
    ga10b_rpc_map_t m = { .va = GA10B_RPC_WINDOW_BASE + GA10B_RPC_WINDOW_SPAN,
                          .phy = 0x100000, .size = 4096, .old_size = 0 };
    EXPECT(daemon_call((int)getpid(), GA10B_RPC_MAP, &m, sizeof(m), NULL, 0)
           == GA10B_RPC_E_INVAL);
    m.va = r.slot_base + 4096;                        /* not slot-aligned */
    EXPECT(daemon_call((int)getpid(), GA10B_RPC_MAP, &m, sizeof(m), NULL, 0)
           == GA10B_RPC_E_INVAL);
}

static void test_alloc_free(void) {
    void* va = NULL; uint64_t gpu_va = 0;
    EXPECT(ga10b_dev_alloc(1000, &va, &gpu_va) == 0);
    EXPECT(va != NULL && gpu_va != 0);
    EXPECT(gpu_va < GA10B_RPC_PERSIST_CAP);           /* persistent range */
    EXPECT(((uint8_t*)va)[0] == 0 && ((uint8_t*)va)[999] == 0);
    /* foreign free is refused: daemon FREE only owns the session's list */
    ga10b_rpc_free_t f = { .gpu_va = gpu_va + 0x100000, .size = 4096 };
    EXPECT(daemon_call((int)getpid(), GA10B_RPC_FREE, &f, sizeof(f), NULL, 0)
           == GA10B_RPC_E_INVAL);
    int unmaps = g_unmap_calls;
    ga10b_dev_free(va, gpu_va, 1000);
    EXPECT(g_unmap_calls == unmaps + 1);
    EXPECT(g_persist_live == 0);
}

static ga10b_module_t* g_mod;
static void test_module_launch(void) {
    uint8_t cubin[64];
    for (uint32_t i = 0; i < sizeof(cubin); i++) cubin[i] = (uint8_t)(i * 7);
    g_mod = ga10b_dev_module(cubin, sizeof(cubin));
    EXPECT(g_mod != NULL);
    /* the daemon received the exact image through the one-shot shm */
    fake_mod_t* fm = (fake_mod_t*)_sess[0].mods->m;
    EXPECT(fm->image_size == sizeof(cubin));
    EXPECT(memcmp(fm->image, cubin, sizeof(cubin)) == 0);

    int kid = ga10b_module_kernel(g_mod, "vecadd");
    EXPECT(kid == 0);
    /* name cache: second lookup must not hit the wire -- count via kid reuse */
    EXPECT(ga10b_module_kernel(g_mod, "vecadd") == kid);
    EXPECT(ga10b_module_kernel(g_mod, "nope") == -1);

    /* param packing roundtrip: 8B + 8B + 4B at cbank0 0x40/0x48/0x50 */
    uint64_t a = 0x1122334455667788ULL, b = 0x99aabbccddeeff00ULL;
    uint32_t c = 0xcafebabe;
    void* args[3] = { &a, &b, &c };
    uint32_t grid[3] = { 2, 3, 4 }, block[3] = { 32, 4, 1 };
    memset(&g_launch, 0, sizeof(g_launch));
    EXPECT(ga10b_dev_launch_dyn(g_mod, kid, grid, block, args, 512) == 0);
    EXPECT(g_launch.called == 1);
    EXPECT(memcmp(g_launch.grid, grid, sizeof(grid)) == 0);
    EXPECT(memcmp(g_launch.block, block, sizeof(block)) == 0);
    EXPECT(g_launch.dyn_smem == 512);
    EXPECT(g_launch.nargs == 3);
    EXPECT(memcmp(g_launch.args[0], &a, 8) == 0);
    EXPECT(memcmp(g_launch.args[1], &b, 8) == 0);
    EXPECT(memcmp(g_launch.args[2], &c, 4) == 0);
    /* the blob in the shared req shm holds the params at their offsets */
    uint8_t* blob = (uint8_t*)r.req_va;
    EXPECT(memcmp(blob + 0x40, &a, 8) == 0);
    EXPECT(memcmp(blob + 0x48, &b, 8) == 0);
    EXPECT(memcmp(blob + 0x50, &c, 4) == 0);

    /* compute payload: unif || ssbo || texdesc */
    uint32_t unif[4] = { 1, 2, 3, 4 };
    uint64_t ssbo[2] = { 0x400000000ULL, 0x404000000ULL };
    uint32_t tex[8] = { 10, 11, 12, 13, 14, 15, 16, 17 };
    memset(&g_launchc, 0, sizeof(g_launchc));
    EXPECT(ga10b_dev_launch_compute(g_mod, kid, grid, block,
                                    unif, 4, ssbo, 2, tex, 8) == 0);
    EXPECT(g_launchc.called == 1);
    EXPECT(g_launchc.nunif == 4 &&
           memcmp(g_launchc.unif, unif, sizeof(unif)) == 0);
    EXPECT(g_launchc.nssbo == 2 &&
           memcmp(g_launchc.ssbo, ssbo, sizeof(ssbo)) == 0);
    EXPECT(g_launchc.ntex == 8 &&
           memcmp(g_launchc.tex, tex, sizeof(tex)) == 0);

    /* copy / sync ride the wire */
    EXPECT(ga10b_dev_copy(0x400000000ULL, 0x400001000ULL, 4096) == 0);
    EXPECT(g_copy_calls == 1);
    EXPECT(ga10b_dev_sync_to_gpu() == 0 && g_sync_gpu_calls == 1);
    EXPECT(ga10b_dev_sync_to_cpu() == 0 && g_sync_cpu_calls == 1);

    /* daemon bounds: launch blob outside the req shm */
    ga10b_rpc_launch_t L;
    memset(&L, 0, sizeof(L));
    L.handle = g_mod->handle; L.kid = kid;
    L.blob_off = r.req_size; L.blob_len = 4;
    EXPECT(daemon_call((int)getpid(), GA10B_RPC_LAUNCH, &L, sizeof(L),
                       NULL, 0) == GA10B_RPC_E_INVAL);
}

/* queue one launch tagged by grid[0] */
static int q1(int kid, uint32_t tag, uint32_t dyn) {
    uint64_t a = 0x1000 + tag, b = 0x2000 + tag;
    uint32_t c = tag;
    void* args[3] = { &a, &b, &c };
    uint32_t grid[3] = { tag, 1, 1 }, block[3] = { 64, 1, 1 };
    return ga10b_dev_launch_async(g_mod, kid, grid, block, args, dyn);
}

static void test_launch_batch(void) {
    int kadd = ga10b_module_kernel(g_mod, "vecadd");
    int kscale = ga10b_module_kernel(g_mod, "scale");
    EXPECT(kadd == 0 && kscale == 1);
    /* warm the kernel-info cache so RPC counts below are launches only */
    EXPECT(q1(kadd, 1, 0) == 0 && q1(kscale, 1, 0) == 0);
    EXPECT(ga10b_dev_flush() == 0);

    /* queued launches do not reach the daemon until the flush: one RPC,
     * submission order, per-launch params intact */
    memset(&g_launch, 0, sizeof(g_launch));
    g_hist_n = 0;
    int rpcs = g_rpc_calls;
    for (uint32_t i = 0; i < 5; i++)
        EXPECT(q1((i & 1) ? kscale : kadd, 10 + i, i * 16) == 0);
    EXPECT(g_launch.called == 0 && g_rpc_calls == rpcs && r.q_count == 5);
    EXPECT(ga10b_dev_flush() == 0);
    EXPECT(g_rpc_calls == rpcs + 1 && g_launch.called == 5 && r.q_count == 0);
    EXPECT(g_hist_n == 5);
    for (uint32_t i = 0; i < 5; i++) {
        EXPECT(g_hist[i].kid == ((i & 1) ? kscale : kadd));
        EXPECT(g_hist[i].grid0 == 10 + i && g_hist[i].dyn_smem == i * 16);
        EXPECT(g_hist[i].arg0 == 0x1000 + 10 + i);
    }
    /* the last record's full param set (8B + 8B + 4B) survived the queue */
    uint64_t la = 0x1000 + 14, lb = 0x2000 + 14;
    uint32_t lc = 14;
    EXPECT(g_launch.nargs == 3 && memcmp(g_launch.args[0], &la, 8) == 0 &&
           memcmp(g_launch.args[1], &lb, 8) == 0 &&
           memcmp(g_launch.args[2], &lc, 4) == 0);
    /* nothing queued: flush is free */
    EXPECT(ga10b_dev_flush() == 0 && g_rpc_calls == rpcs + 1);

    /* the cap drains by itself; the remainder stays queued */
    memset(&g_launch, 0, sizeof(g_launch));
    g_hist_n = 0;
    rpcs = g_rpc_calls;
    for (uint32_t i = 0; i < GA10B_RPC_BATCH_MAX + 3; i++)
        EXPECT(q1(kadd, 100 + i, 0) == 0);
    EXPECT(g_launch.called == (int)GA10B_RPC_BATCH_MAX);
    EXPECT(g_rpc_calls == rpcs + 1 && r.q_count == 3);
    /* sync_to_cpu rides the batch: launches first, then the L2 flush, in a
     * single round trip */
    int sc = g_sync_cpu_calls;
    EXPECT(ga10b_dev_sync_to_cpu() == 0);
    EXPECT(g_rpc_calls == rpcs + 2 && r.q_count == 0);
    EXPECT(g_sync_cpu_calls == sc + 1);
    EXPECT(g_sync_cpu_after == (int)GA10B_RPC_BATCH_MAX + 3);
    for (uint32_t i = 0; i < GA10B_RPC_BATCH_MAX + 3; i++)
        EXPECT(g_hist[i].grid0 == 100 + i);

    /* time budget: the daemon hands a long batch back and the client
     * resumes it where it stopped - same order, nothing lost or repeated,
     * and the L2 flush still comes once, after the last launch.  3 ms
     * launches against the 20 ms budget: 7 launches per round trip. */
    memset(&g_launch, 0, sizeof(g_launch));
    g_hist_n = 0;
    g_launch_ns = 3000000ull;
    rpcs = g_rpc_calls;
    sc = g_sync_cpu_calls;
    for (uint32_t i = 0; i < 40; i++)
        EXPECT(q1((i & 1) ? kscale : kadd, 300 + i, i) == 0);
    EXPECT(g_rpc_calls == rpcs && r.q_count == 40);
    EXPECT(ga10b_dev_sync_to_cpu() == 0);
    EXPECT(g_rpc_calls == rpcs + 6 && g_launch.called == 40 && r.q_count == 0);
    EXPECT(g_sync_cpu_calls == sc + 1 && g_sync_cpu_after == 40);
    EXPECT(g_hist_n == 40);
    for (uint32_t i = 0; i < 40; i++)
        EXPECT(g_hist[i].grid0 == 300 + i && g_hist[i].dyn_smem == i &&
               g_hist[i].kid == ((i & 1) ? kscale : kadd));
    /* a failure in a later round is still reported */
    memset(&g_launch, 0, sizeof(g_launch));
    g_launch_fail_on = 10;
    for (uint32_t i = 0; i < 20; i++)
        EXPECT(q1(kadd, 400 + i, 0) == 0);
    EXPECT(ga10b_dev_flush() != 0 && g_launch.called == 10);
    g_launch_fail_on = 0;
    r.q_err = 0;
    g_launch_ns = 0;

    /* a version 2 daemon: 12-byte request, 8-byte reply, 32 per batch */
    uint32_t ver = r.ver, bmax = r.batch_max;
    EXPECT(ver == GA10B_RPC_VERSION && bmax == GA10B_RPC_BATCH_MAX);
    r.ver = 2;
    r.batch_max = GA10B_RPC_BATCH_MAX_V2;
    memset(&g_launch, 0, sizeof(g_launch));
    g_hist_n = 0;
    rpcs = g_rpc_calls;
    for (uint32_t i = 0; i < GA10B_RPC_BATCH_MAX_V2 + 2; i++)
        EXPECT(q1(kadd, 500 + i, 0) == 0);
    EXPECT(g_rpc_calls == rpcs + 1 && r.q_count == 2 &&
           g_launch.called == (int)GA10B_RPC_BATCH_MAX_V2);
    EXPECT(ga10b_dev_flush() == 0 && g_rpc_calls == rpcs + 2 &&
           g_hist_n == (int)GA10B_RPC_BATCH_MAX_V2 + 2);
    r.ver = ver;
    r.batch_max = bmax;

    /* every call that could observe a queued launch drains it first */
    memset(&g_launch, 0, sizeof(g_launch));
    EXPECT(q1(kadd, 1, 0) == 0);
    EXPECT(ga10b_dev_sync_to_gpu() == 0 && g_sync_gpu_after == 1);
    EXPECT(q1(kadd, 2, 0) == 0);
    EXPECT(ga10b_dev_copy(0x400000000ULL, 0x400001000ULL, 4096) == 0);
    EXPECT(g_copy_after == 2);
    /* a synchronous launch runs after the queued ones */
    g_hist_n = 0;
    EXPECT(q1(kadd, 7, 0) == 0 && q1(kadd, 8, 0) == 0);
    uint64_t a = 1, b = 2;
    uint32_t c = 3;
    void* args[3] = { &a, &b, &c };
    uint32_t grid[3] = { 9, 1, 1 }, block[3] = { 64, 1, 1 };
    EXPECT(ga10b_dev_launch_dyn(g_mod, kadd, grid, block, args, 0) == 0);
    EXPECT(g_hist_n == 3 && g_hist[0].grid0 == 7 && g_hist[1].grid0 == 8 &&
           g_hist[2].grid0 == 9);
    EXPECT(g_launch.called == 5);
    EXPECT(q1(kadd, 3, 0) == 0);
    uint32_t unif[1] = { 1 };
    EXPECT(ga10b_dev_launch_compute(g_mod, kadd, grid, block, unif, 1,
                                    NULL, 0, NULL, 0) == 0);
    EXPECT(g_launchc.launches_before == 6);
    /* slot map: a cache hit keeps the queue, an eviction drains it */
    uint64_t hit_phy = 0xa00000;
    EXPECT(ga10b_dev_map(hit_phy, 4096) != 0);
    EXPECT(q1(kadd, 4, 0) == 0);
    EXPECT(ga10b_dev_map(hit_phy, 4096) != 0 && r.q_count == 1);
    EXPECT(ga10b_dev_map(0xb00000, 4096) != 0 && r.q_count == 0);
    EXPECT(g_map_at_after == 7);
    /* free: the queued launch may use the buffer */
    void* av = NULL;
    uint64_t ag = 0;
    EXPECT(ga10b_dev_alloc(4096, &av, &ag) == 0);
    EXPECT(q1(kadd, 5, 0) == 0);
    ga10b_dev_free(av, ag, 4096);
    EXPECT(g_unmap_after == 8 && r.q_count == 0);

    /* a launch failing mid-batch: the rest never runs, the flush reports
     * it, and later queued launches are refused */
    memset(&g_launch, 0, sizeof(g_launch));
    g_launch_fail_on = 2;
    for (uint32_t i = 0; i < 4; i++)
        EXPECT(q1(kadd, 50 + i, 0) == 0);
    EXPECT(ga10b_dev_flush() != 0);
    EXPECT(g_launch.called == 2);
    g_launch_fail_on = 0;
    EXPECT(q1(kadd, 60, 0) != 0 && r.q_count == 0);
    EXPECT(ga10b_dev_flush() != 0);
    r.q_err = 0;   /* a fresh open clears it; keep the session for the tests below */

    /* malformed batches straight at the daemon */
    int me = (int)getpid();
    ga10b_rpc_batch_t B = { .count = 0, .bytes = 0, .flags = 0, .start = 0 };
    ga10b_rpc_batch_rep_t R;
    EXPECT(daemon_call(me, GA10B_RPC_LAUNCH_BATCH, &B, sizeof(B), NULL, 0)
           == GA10B_RPC_E_INVAL);
    B.count = GA10B_RPC_BATCH_MAX + 1;
    EXPECT(daemon_call(me, GA10B_RPC_LAUNCH_BATCH, &B, sizeof(B), NULL, 0)
           == GA10B_RPC_E_INVAL);
    B.count = 1; B.bytes = r.req_size + 1;
    EXPECT(daemon_call(me, GA10B_RPC_LAUNCH_BATCH, &B, sizeof(B), NULL, 0)
           == GA10B_RPC_E_INVAL);
    EXPECT(daemon_call(me, GA10B_RPC_LAUNCH_BATCH, &B, sizeof(B) - 1, NULL, 0)
           == GA10B_RPC_E_INVAL);
    /* start past the records, or not on a record boundary */
    B.count = 1; B.bytes = 64; B.start = 72;
    EXPECT(daemon_call(me, GA10B_RPC_LAUNCH_BATCH, &B, sizeof(B), NULL, 0)
           == GA10B_RPC_E_INVAL);
    B.start = 4;
    EXPECT(daemon_call(me, GA10B_RPC_LAUNCH_BATCH, &B, sizeof(B), NULL, 0)
           == GA10B_RPC_E_INVAL);
    /* the version 2 layout keeps its own limits */
    B.start = 0; B.count = GA10B_RPC_BATCH_MAX_V2 + 1; B.bytes = 0;
    EXPECT(daemon_call(me, GA10B_RPC_LAUNCH_BATCH, &B, GA10B_RPC_BATCH_V2_SIZE,
                       NULL, 0) == GA10B_RPC_E_INVAL);
    B.count = 1; B.flags = GA10B_RPC_BATCH_PARTIAL;
    EXPECT(daemon_call(me, GA10B_RPC_LAUNCH_BATCH, &B, GA10B_RPC_BATCH_V2_SIZE,
                       NULL, 0) == GA10B_RPC_E_INVAL);
    B.flags = 0;
    B.count = 1; B.bytes = r.req_size + 1;

    uint8_t* shm = (uint8_t*)r.req_va;
    ga10b_rpc_launch_t L;
    uint32_t rec_bytes = (uint32_t)sizeof(L) + 0x54u;
    memset(&L, 0, sizeof(L));
    L.handle = g_mod->handle; L.kid = kadd;
    L.grid[0] = L.grid[1] = L.grid[2] = 1;
    L.block[0] = L.block[1] = L.block[2] = 1;
    L.blob_off = (uint32_t)sizeof(L); L.blob_len = 0x54;
    memset(shm, 0, 4096);
    memcpy(shm, &L, sizeof(L));
    memset(&g_launch, 0, sizeof(g_launch));
    /* well-formed single record: accepted */
    B.count = 1; B.bytes = rec_bytes;
    EXPECT(daemon_call(me, GA10B_RPC_LAUNCH_BATCH, &B, sizeof(B),
                       &R, sizeof(R)) == 0);
    EXPECT(R.done == 1 && R.status == 0 && g_launch.called == 1);
    /* record cut short by `bytes` */
    B.bytes = (uint32_t)sizeof(L) - 4;
    EXPECT(daemon_call(me, GA10B_RPC_LAUNCH_BATCH, &B, sizeof(B),
                       &R, sizeof(R)) == 0);
    EXPECT(R.done == 0 && R.status == GA10B_RPC_E_INVAL);
    /* blob reaching past `bytes` */
    B.bytes = rec_bytes - 1;
    EXPECT(daemon_call(me, GA10B_RPC_LAUNCH_BATCH, &B, sizeof(B),
                       &R, sizeof(R)) == 0);
    EXPECT(R.done == 0 && R.status == GA10B_RPC_E_INVAL);
    /* blob_off not right behind the record (would alias another record) */
    L.blob_off = 0;
    memcpy(shm, &L, sizeof(L));
    B.bytes = rec_bytes;
    EXPECT(daemon_call(me, GA10B_RPC_LAUNCH_BATCH, &B, sizeof(B),
                       &R, sizeof(R)) == 0);
    EXPECT(R.done == 0 && R.status == GA10B_RPC_E_INVAL);
    /* good record then a foreign module handle: first runs, second refused */
    L.blob_off = (uint32_t)sizeof(L);
    memcpy(shm, &L, sizeof(L));
    uint32_t off2 = (rec_bytes + 7u) & ~7u;
    L.handle = 12345;
    L.blob_off = off2 + (uint32_t)sizeof(L);
    memcpy(shm + off2, &L, sizeof(L));
    B.count = 2; B.bytes = off2 + rec_bytes;
    memset(&g_launch, 0, sizeof(g_launch));
    EXPECT(daemon_call(me, GA10B_RPC_LAUNCH_BATCH, &B, sizeof(B),
                       &R, sizeof(R)) == 0);
    EXPECT(R.done == 1 && R.status == GA10B_RPC_E_INVAL &&
           g_launch.called == 1);
    EXPECT(g_sync_cpu_calls == sc + 1);   /* no sync without the flag */
}

/* the same batches with the daemon queueing on the channel: the stub queue
 * holds STUB_QUEUE_SLOTS launches, so every batch below crosses a drain */
static void test_launch_pipeline(void) {
    int kadd = ga10b_module_kernel(g_mod, "vecadd");
    int kscale = ga10b_module_kernel(g_mod, "scale");
    char* on[2] = { "pipeline", "on" };
    char* off[2] = { "pipeline", "off" };
    char* st[1] = { "stat" };
    EXPECT(_pipeline == 0);
    EXPECT(strstr(gpud_cmd(NULL, 0, 1, st, NULL), "pipeline=0") != NULL);
    EXPECT(strstr(gpud_cmd(NULL, 0, 1, st, NULL),
                  "tables=7 free_ranges=3 va_next=412345000 ") != NULL);
    EXPECT(strstr(gpud_cmd(NULL, 0, 2, on, NULL), "pipeline=1") != NULL);
    EXPECT(_pipeline == 1);
    EXPECT(strstr(gpud_cmd(NULL, 0, 1, st, NULL), "pipeline=1") != NULL);
    EXPECT(q1(kadd, 1, 0) == 0 && q1(kscale, 1, 0) == 0);
    EXPECT(ga10b_dev_flush() == 0);

    /* order and params survive, one RPC, the L2 flush comes after the last
     * drain, and nothing touches the device while launches are queued */
    memset(&g_launch, 0, sizeof(g_launch));
    g_hist_n = 0;
    g_q_calls = g_drain_calls = g_busy_violations = 0;
    int rpcs = g_rpc_calls, sc = g_sync_cpu_calls;
    for (uint32_t i = 0; i < 40; i++)
        EXPECT(q1((i & 1) ? kscale : kadd, 700 + i, i) == 0);
    EXPECT(ga10b_dev_sync_to_cpu() == 0);
    EXPECT(g_rpc_calls == rpcs + 1 && g_launch.called == 40 && g_hist_n == 40);
    EXPECT(g_q_calls == 40 && g_drain_calls == 5 && g_q_pending == 0);
    EXPECT(g_sync_cpu_calls == sc + 1 && g_sync_cpu_after == 40);
    EXPECT(g_busy_violations == 0);
    for (uint32_t i = 0; i < 40; i++)
        EXPECT(g_hist[i].grid0 == 700 + i && g_hist[i].dyn_smem == i &&
               g_hist[i].kid == ((i & 1) ? kscale : kadd) &&
               g_hist[i].arg0 == 0x1000 + 700 + i);

    /* a kernel that never fits a slot runs synchronously, in its place,
     * after the launches queued before it were collected */
    memset(&g_launch, 0, sizeof(g_launch));
    g_hist_n = 0;
    g_q_calls = g_drain_calls = g_busy_violations = 0;
    g_q_refuse_kid = kscale;
    for (uint32_t i = 0; i < 12; i++)
        EXPECT(q1((i % 3) == 2 ? kscale : kadd, 800 + i, 0) == 0);
    EXPECT(ga10b_dev_flush() == 0);
    EXPECT(g_launch.called == 12 && g_hist_n == 12 && g_q_calls == 8);
    EXPECT(g_busy_violations == 0 && g_q_pending == 0);
    for (uint32_t i = 0; i < 12; i++)
        EXPECT(g_hist[i].grid0 == 800 + i);
    g_q_refuse_kid = -1;

    /* time budget: checked where the queue is collected, the client resumes
     * (3 ms launches, 8 per drain: 24 ms, so 8 launches per round trip) */
    memset(&g_launch, 0, sizeof(g_launch));
    g_hist_n = 0;
    g_busy_violations = 0;
    g_launch_ns = 3000000ull;
    rpcs = g_rpc_calls;
    sc = g_sync_cpu_calls;
    for (uint32_t i = 0; i < 40; i++)
        EXPECT(q1(kadd, 900 + i, 0) == 0);
    EXPECT(ga10b_dev_sync_to_cpu() == 0);
    EXPECT(g_rpc_calls == rpcs + 5 && g_launch.called == 40 && g_hist_n == 40);
    EXPECT(g_sync_cpu_calls == sc + 1 && g_sync_cpu_after == 40);
    EXPECT(g_busy_violations == 0);
    for (uint32_t i = 0; i < 40; i++)
        EXPECT(g_hist[i].grid0 == 900 + i);
    g_launch_ns = 0;

    /* a launch refused when it is queued: the earlier ones are collected,
     * the rest never runs, the flush reports it */
    memset(&g_launch, 0, sizeof(g_launch));
    g_launch_fail_on = 3;
    for (uint32_t i = 0; i < 6; i++)
        EXPECT(q1(kadd, 50 + i, 0) == 0);
    EXPECT(ga10b_dev_flush() != 0);
    EXPECT(g_launch.called == 3 && g_q_pending == 0);
    g_launch_fail_on = 0;
    r.q_err = 0;

    /* queued launches that never retire: the reply counts the ones that
     * did and points behind the last of them; no L2 flush after a failure */
    int me = (int)getpid();
    uint8_t* shm = (uint8_t*)r.req_va;
    ga10b_rpc_launch_t L;
    uint32_t rec_bytes = (uint32_t)sizeof(L) + 0x54u;
    uint32_t step = (rec_bytes + 7u) & ~7u;
    memset(shm, 0, 4096);
    for (uint32_t i = 0; i < 5; i++) {
        memset(&L, 0, sizeof(L));
        L.handle = g_mod->handle; L.kid = kadd;
        L.grid[0] = L.grid[1] = L.grid[2] = 1;
        L.block[0] = L.block[1] = L.block[2] = 1;
        L.blob_off = i * step + (uint32_t)sizeof(L); L.blob_len = 0x54;
        memcpy(shm + i * step, &L, sizeof(L));
    }
    ga10b_rpc_batch_t B = { .count = 5, .bytes = 4 * step + rec_bytes,
                            .flags = GA10B_RPC_BATCH_SYNC_TO_CPU, .start = 0 };
    ga10b_rpc_batch_rep_t R;
    memset(&g_launch, 0, sizeof(g_launch));
    sc = g_sync_cpu_calls;
    g_drain_short = 2;
    EXPECT(daemon_call(me, GA10B_RPC_LAUNCH_BATCH, &B, sizeof(B),
                       &R, sizeof(R)) == 0);
    EXPECT(R.done == 3 && R.status == GA10B_RPC_E_FAILED && R.next == 3 * step);
    EXPECT(g_sync_cpu_calls == sc && g_q_pending == 0);
    /* all of them retire: done and next cover the whole batch */
    EXPECT(daemon_call(me, GA10B_RPC_LAUNCH_BATCH, &B, sizeof(B),
                       &R, sizeof(R)) == 0);
    EXPECT(R.done == 5 && R.status == 0 && R.next == 5 * step);
    EXPECT(g_sync_cpu_calls == sc + 1);
    /* a bad record behind queued launches: those still run and count */
    L.handle = 12345; L.blob_off = 2 * step + (uint32_t)sizeof(L);
    memcpy(shm + 2 * step, &L, sizeof(L));
    memset(&g_launch, 0, sizeof(g_launch));
    EXPECT(daemon_call(me, GA10B_RPC_LAUNCH_BATCH, &B, sizeof(B),
                       &R, sizeof(R)) == 0);
    EXPECT(R.done == 2 && R.status == GA10B_RPC_E_INVAL && R.next == 2 * step);
    EXPECT(g_launch.called == 2 && g_q_pending == 0);
    EXPECT(g_busy_violations == 0);

    /* the single-launch RPC stays synchronous */
    g_q_calls = 0;
    uint64_t a = 1, b = 2;
    uint32_t c = 3;
    void* args[3] = { &a, &b, &c };
    uint32_t grid[3] = { 9, 1, 1 }, block[3] = { 64, 1, 1 };
    EXPECT(ga10b_dev_launch_dyn(g_mod, kadd, grid, block, args, 0) == 0);
    EXPECT(g_q_calls == 0);

    EXPECT(strstr(gpud_cmd(NULL, 0, 2, off, NULL), "pipeline=0") != NULL);
    EXPECT(_pipeline == 0);
    g_q_calls = 0;
    EXPECT(q1(kadd, 1, 0) == 0 && ga10b_dev_flush() == 0 && g_q_calls == 0);
}

static const gpud_prof_t* prof_find(const char* name) {
    for (uint32_t i = 0; i < _prof_n; i++)
        if (strcmp(_prof[i].name, name) == 0)
            return &_prof[i];
    return NULL;
}

/* the daemon's launch profile: CPU time for synchronous launches, adjacent
 * GPU releases for queued ones, batch totals, nothing recorded when off */
static void test_prof(void) {
    int kadd = ga10b_module_kernel(g_mod, "vecadd");
    int kscale = ga10b_module_kernel(g_mod, "scale");
    char* on[2] = { "prof", "on" };
    char* off[2] = { "prof", "off" };
    char* rst[2] = { "prof", "reset" };
    char* dump[2] = { "prof", "dump" };
    char* pon[2] = { "pipeline", "on" };
    char* poff[2] = { "pipeline", "off" };
    EXPECT(_prof_on == 0 && _prof_n == 0 && _prof_batch.batches == 0);
    EXPECT(q1(kadd, 1, 0) == 0 && ga10b_dev_flush() == 0);
    EXPECT(_prof_n == 0 && _prof_batch.batches == 0);

    EXPECT(strstr(gpud_cmd(NULL, 0, 2, on, NULL), "prof=1") != NULL);
    EXPECT(g_queue_timestamps == 1);
    g_launch_ns = 3000;
    for (uint32_t i = 0; i < 6; i++)
        EXPECT(q1((i % 3) == 2 ? kscale : kadd, 1 + i, 0) == 0);
    EXPECT(ga10b_dev_sync_to_cpu() == 0);
    const gpud_prof_t* a = prof_find("vecadd");
    const gpud_prof_t* b = prof_find("scale");
    const gpud_prof_t* f = prof_find("<sync_to_cpu>");
    EXPECT(a != NULL && b != NULL && f != NULL && _prof_n == 3);
    if (a != NULL && b != NULL && f != NULL) {
        EXPECT(a->n == 4 && a->ns == 12000 && a->min == 3000 && a->max == 3000);
        EXPECT(b->n == 2 && b->ns == 6000 && a->queued == 0 && b->queued == 0);
        EXPECT(f->n == 1);
    }
    EXPECT(_prof_batch.batches == 1 && _prof_batch.launches == 6 &&
           _prof_batch.ns == 18000 && _prof_batch.submit_ns == 0);
    EXPECT(strstr(gpud_cmd(NULL, 0, 2, dump, NULL), "launches 6") != NULL);

    /* queued launches use GPU release deltas; the first only sets a baseline */
    EXPECT(_prof_batch.drain_ns == 0);
    gpud_cmd(NULL, 0, 2, pon, NULL);
    for (uint32_t i = 0; i < 5; i++)
        EXPECT(q1(kadd, 1 + i, 0) == 0);
    EXPECT(ga10b_dev_flush() == 0);
    a = prof_find("vecadd");
    EXPECT(a != NULL && a->n == 8 && a->queued == 5 && a->ns == 24000 &&
           a->min == 3000 && a->max == 3000 && _prof_ts_bad == 0);
    EXPECT(_prof_batch.batches == 2 && _prof_batch.launches == 11 &&
           _prof_batch.ns == 33000 && _prof_batch.drain_ns == 15000);
    gpud_cmd(NULL, 0, 2, poff, NULL);

    /* a failing launch is not recorded */
    g_launch_fail_on = g_launch.called + 2;
    EXPECT(q1(kscale, 1, 0) == 0 && q1(kscale, 2, 0) == 0);
    EXPECT(ga10b_dev_flush() != 0);
    g_launch_fail_on = 0;
    r.q_err = 0;
    b = prof_find("scale");
    EXPECT(b != NULL && b->n == 3);
    EXPECT(_prof_batch.batches == 3 && _prof_batch.launches == 12);

    EXPECT(strstr(gpud_cmd(NULL, 0, 2, rst, NULL), "kernels 0") != NULL);
    EXPECT(_prof_n == 0 && _prof_batch.batches == 0 && _prof_on == 1);
    EXPECT(strstr(gpud_cmd(NULL, 0, 2, off, NULL), "prof=0") != NULL);
    EXPECT(g_queue_timestamps == 0);
    EXPECT(q1(kadd, 1, 0) == 0 && ga10b_dev_flush() == 0 && _prof_n == 0);
    g_launch_ns = 0;
}

static void test_memclk(void) {
    int kadd = ga10b_module_kernel(g_mod, "vecadd");
    char* st[1] = { "stat" };
    char* on[2] = { "memclk", "on" };
    char* off[2] = { "memclk", "off" };
    /* every launch so far shared one request for the floor */
    EXPECT(_memclk.held == 1 && g_memclk_state == 1 && g_memclk_calls == 1);
    EXPECT(strstr(gpud_cmd(NULL, 0, 1, st, NULL), " memclk=held ") != NULL);

    /* kept through short pauses, dropped once the GPU has been idle */
    g_fake_nsec = _memclk.last + GPUD_MEMCLK_IDLE_NS;
    EXPECT(gpud_loop_step(NULL, NULL) == 0);
    EXPECT(_memclk.held == 1 && g_memclk_calls == 1);
    g_fake_nsec++;
    EXPECT(gpud_loop_step(NULL, NULL) == 0);
    EXPECT(_memclk.held == 0 && g_memclk_state == 0 && g_memclk_calls == 2);
    EXPECT(strstr(gpud_cmd(NULL, 0, 1, st, NULL), " memclk=idle ") != NULL);
    EXPECT(gpud_loop_step(NULL, NULL) == 0 && g_memclk_calls == 2);

    /* only GPU work asks for it again; the idle time counts from its end */
    EXPECT(ga10b_dev_sync_to_cpu() == 0 && g_memclk_calls == 2);
    g_launch_ns = 5000;
    EXPECT(q1(kadd, 1, 0) == 0 && ga10b_dev_flush() == 0);
    EXPECT(_memclk.held == 1 && g_memclk_state == 1 && g_memclk_calls == 3);
    EXPECT(_memclk.last == g_fake_nsec);
    g_launch_ns = 0;

    /* runtime switch */
    EXPECT(strstr(gpud_cmd(NULL, 0, 2, off, NULL), "memclk=0") != NULL);
    EXPECT(_memclk.held == 0 && g_memclk_state == 0 && g_memclk_calls == 4);
    EXPECT(q1(kadd, 1, 0) == 0 && ga10b_dev_flush() == 0 && g_memclk_calls == 4);
    EXPECT(strstr(gpud_cmd(NULL, 0, 1, st, NULL), " memclk=off ") != NULL);
    EXPECT(strstr(gpud_cmd(NULL, 0, 2, on, NULL), "memclk=1") != NULL);

    /* a BPMP that refuses is asked once, launches go on without the floor */
    g_memclk_fail = 1;
    EXPECT(q1(kadd, 1, 0) == 0 && ga10b_dev_flush() == 0);
    EXPECT(_memclk.held == 0 && _memclk.failed == 1 && g_memclk_calls == 5);
    EXPECT(q1(kadd, 1, 0) == 0 && ga10b_dev_flush() == 0 && g_memclk_calls == 5);
    EXPECT(strstr(gpud_cmd(NULL, 0, 1, st, NULL), " memclk=failed ") != NULL);
    g_memclk_fail = 0;
    gpud_cmd(NULL, 0, 2, on, NULL);
    EXPECT(q1(kadd, 1, 0) == 0 && ga10b_dev_flush() == 0);
    EXPECT(_memclk.held == 1 && g_memclk_calls == 6);
}

static void test_thread_client(void) {
    /* a multi_task client's worker thread (own pid slot) must land in the
     * owning process's session, not be refused E_NOSESSION */
    g_worker_pid = 999;
    ga10b_rpc_map_t m = { .va = r.slot_base + 2 * GA10B_RPC_SLOT_SPAN,
                          .phy = 0x800000, .size = 4096, .old_size = 0 };
    EXPECT(daemon_call(999, GA10B_RPC_MAP, &m, sizeof(m), NULL, 0) == 0);
    ga10b_rpc_alloc_t al = { .phy = 0x810000, .size = 4096 };
    ga10b_rpc_alloc_rep_t ar;
    EXPECT(daemon_call(999, GA10B_RPC_ALLOC, &al, sizeof(al),
                       &ar, sizeof(ar)) == 0);
    ga10b_rpc_free_t f = { .gpu_va = ar.gpu_va, .size = 4096 };
    EXPECT(daemon_call(999, GA10B_RPC_FREE, &f, sizeof(f), NULL, 0) == 0);
    g_worker_pid = 0;
}

static void test_second_client_isolation(void) {
    uint32_t pid2 = 888;
    alive_set(pid2, 0xbbbb2222u, 1);
    int sid = shmget(0x1234, 4096, 0666 | IPC_CREAT | IPC_EXCL | SHM_NOCACHE);
    EXPECT(sid > 0);
    ga10b_rpc_open_t o;
    memset(&o, 0, sizeof(o));
    o.uuid = 0xbbbb2222u; o.shm_id = sid; o.shm_size = 4096;
    ga10b_rpc_open_rep_t rep;
    EXPECT(daemon_call((int)pid2, GA10B_RPC_OPEN, &o, sizeof(o),
                       &rep, sizeof(rep)) == 0);
    EXPECT(rep.client_idx == 1);
    EXPECT(rep.slot_base == GA10B_RPC_WINDOW_BASE + GA10B_RPC_WINDOW_SPAN);

    /* client 2 may not map into client 1's window */
    ga10b_rpc_map_t m = { .va = r.slot_base, .phy = 0x300000,
                          .size = 4096, .old_size = 0 };
    EXPECT(daemon_call((int)pid2, GA10B_RPC_MAP, &m, sizeof(m), NULL, 0)
           == GA10B_RPC_E_INVAL);
    /* client 2 allocs, client 1 may not free them */
    ga10b_rpc_alloc_t al = { .phy = 0x300000, .size = 4096 };
    ga10b_rpc_alloc_rep_t ar;
    EXPECT(daemon_call((int)pid2, GA10B_RPC_ALLOC, &al, sizeof(al),
                       &ar, sizeof(ar)) == 0);
    ga10b_rpc_free_t f = { .gpu_va = ar.gpu_va, .size = 4096 };
    EXPECT(daemon_call((int)getpid(), GA10B_RPC_FREE, &f, sizeof(f), NULL, 0)
           == GA10B_RPC_E_INVAL);
    EXPECT(daemon_call((int)pid2, GA10B_RPC_FREE, &f, sizeof(f), NULL, 0)
           == 0);
    /* client 1 may not launch client 2's module handle (none loaded) */
    ga10b_rpc_kfind_t kf;
    memset(&kf, 0, sizeof(kf));
    kf.handle = 999; strcpy(kf.name, "vecadd");
    EXPECT(daemon_call((int)pid2, GA10B_RPC_KERNEL_FIND, &kf, sizeof(kf),
                       NULL, 0) == GA10B_RPC_E_INVAL);
    /* tidy client 2 */
    EXPECT(daemon_call((int)pid2, GA10B_RPC_CLOSE, NULL, 0, NULL, 0) == 0);
    shmdt(shmat(sid, 0, 0));
    shmctl(sid, IPC_RMID, NULL);
}

static void test_death_sweep(void) {
    /* map two slots + one persistent alloc + one module live in session 0 */
    EXPECT(ga10b_dev_map(0x500000, 4096) != 0);
    void* av = NULL; uint64_t ag = 0;
    EXPECT(ga10b_dev_alloc(4096, &av, &ag) == 0);

    int unmaps = g_unmap_calls, frees = g_modfree_calls;
    alive_set((uint32_t)getpid(), UUID_A, 0);         /* owner dies */
    /* next RPC sees the dead session: refused AND reclaimed */
    EXPECT(ga10b_dev_map(0x600000, 4096) == 0);
    EXPECT(g_unmap_calls > unmaps);
    EXPECT(g_modfree_calls == frees + 1);
    EXPECT(g_persist_live == 0);
    EXPECT(_sess[0].used == 0);
    /* the dead process's client-side DMA buffer is reclaimed by its exit */
    dma_free((ewokos_addr_t)av);
    /* loop_step sweep is idempotent on the empty table */
    EXPECT(gpud_loop_step(NULL, NULL) == 0);

    /* pid reuse: same pid, different uuid is a dead session too */
    g_client_uuid = 0xcccc3333u;
    alive_set((uint32_t)getpid(), 0xcccc3333u, 1);
    /* remote still thinks it is open; force a fresh open cycle */
    remote_release();
    EXPECT(ga10b_dev_open(NULL) == 0);
    EXPECT(r.slot_base == GA10B_RPC_WINDOW_BASE);     /* idx 0 reused */
    ga10b_rpc_map_t m = { .va = r.slot_base, .phy = 0x700000,
                          .size = 4096, .old_size = 0 };
    EXPECT(daemon_call((int)getpid(), GA10B_RPC_MAP, &m, sizeof(m), NULL, 0)
           == 0);
    /* now a process with the SAME pid but the OLD uuid asks: refused */
    g_alive[0].uuid = UUID_A;                         /* uuid mismatch */
    EXPECT(daemon_call((int)getpid(), GA10B_RPC_MAP, &m, sizeof(m), NULL, 0)
           == GA10B_RPC_E_NOSESSION);
    g_alive[0].uuid = 0xcccc3333u;                    /* restore */
}

static void test_teardown(void) {
    remote_release();                                 /* client exits */
    EXPECT(_sess[0].used == 0);
    EXPECT(g_persist_live == 0);
    EXPECT(g_dma_live == 0);
    EXPECT(g_shm_live == 0);
    EXPECT(ga10b_dev_ready() == 0);
}

int main(void) {
    /* queued launches are the default; the synchronous-path tests come first */
    char* st[1] = { "stat" };
    char* poff[2] = { "pipeline", "off" };
    EXPECT(strstr(gpud_cmd(NULL, 0, 1, st, NULL), "pipeline=1") != NULL);
    gpud_cmd(NULL, 0, 2, poff, NULL);
    test_proto_sizes();
    test_ping_open();
    test_map_lru();
    test_alloc_free();
    test_thread_client();
    test_module_launch();
    test_launch_batch();
    test_launch_pipeline();
    test_prof();
    test_memclk();
    test_second_client_isolation();
    test_death_sweep();
    test_teardown();
    if (fails == 0)
        printf("test_gpud: all passed\n");
    else
        printf("test_gpud: %d FAILURES\n", fails);
    return fails == 0 ? 0 : 1;
}