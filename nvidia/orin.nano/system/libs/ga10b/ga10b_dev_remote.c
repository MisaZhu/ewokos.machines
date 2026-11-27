/*
 * ga10b_dev_remote.c - client half of the resident GPU daemon (gpud).
 * Exports the exact ga10b_dev.h surface of the in-process driver, so kuda's
 * backend_ga10b.c and ga10b_pgl.c link against this file (libga10br.a)
 * unchanged.  Everything that touches the GPU goes through /dev/gpu RPC;
 * device memory stays client-owned DMA (block 1), so the CPU memcpy paths
 * (uncached writes, cacheable-mirror reads) keep their measured throughput
 * without a single extra copy.
 *
 * Mapping model mirrors the local driver: dev_map keeps a per-process LRU
 * over the client's 64 fixed GPU VA slots (a cache hit needs no RPC - the
 * daemon VM keeps the mapping); dev_alloc maps a persistent bump range.
 *
 * ga10b_dev_launch_async queues launches in the payload shm and sends them
 * as one batch RPC: a transformer decode step is hundreds of small launches,
 * and the IPC round trip per launch costs more than most of them run for.
 * Every other call that could observe or disturb a queued launch drains the
 * queue first, so the device sees the same order as with synchronous calls.
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/shm.h>

#include <ewoksys/dma.h>
#include <ewoksys/klog.h>
#include <ewoksys/proc.h>
#include <ewoksys/proto.h>
#include <ewoksys/vdevice.h>

#include <ga10b/ga10b_dev.h>
#include <ga10b/ga10b_rpc.h>

typedef struct st_kmeta {
    char name[GA10B_RPC_NAME_MAX];
    int kid;
    int have_info;
    ga10b_rpc_kinfo_rep_t info;
    struct st_kmeta* next;
} kmeta_t;

struct ga10b_module {
    int32_t handle;
    kmeta_t* kernels;
    struct ga10b_module* next;
};

static struct {
    int state;                    /* 0 = not opened, 1 = ready, -1 = failed */
    int pid;                      /* gpud pid */
    uint64_t slot_base;
    int req_id;
    void* req_va;
    uint32_t req_size;
    struct { uint64_t phy; uint32_t size; uint32_t used; } map[GA10B_RPC_SLOTS];
    uint32_t tick;
    uint32_t ver;                 /* daemon protocol version */
    uint32_t batch_max;           /* launches the daemon takes per batch */
    uint32_t q_count;             /* queued launches in req_va */
    uint32_t q_bytes;             /* 8-aligned end of the last record */
    int q_err;                    /* a queued launch failed (sticky) */
    struct ga10b_module* mods;    /* stub list for teardown */
    pthread_mutex_t lock;
} r = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* caller holds r.lock; rep may be NULL */
static int rpc(int cmd, const void* req, uint32_t reqsz,
               void* rep, uint32_t repsz) {
    proto_t in, out;
    PF->init(&in);
    if (req != NULL && reqsz != 0)
        PF->add(&in, req, reqsz);
    PF->init(&out);
    int ret = dev_cntl_by_pid(r.pid, cmd, &in, &out);
    if (ret == 0 && rep != NULL) {
        int32_t sz = 0;
        void* d = proto_read(&out, &sz);
        if (d == NULL || sz < (int32_t)repsz)
            ret = GA10B_RPC_E_FAILED;
        else
            memcpy(rep, d, repsz);
    }
    PF->clear(&out);
    PF->clear(&in);
    return ret;
}

/* caller holds r.lock.  Runs the queued launches in one RPC; `flags` can fold
 * the L2 flush that would follow.  Returns the sticky launch status. */
static int queue_flush(uint32_t flags) {
    if (r.q_count == 0)
        return r.q_err;
    ga10b_rpc_batch_t req = { .count = r.q_count, .bytes = r.q_bytes,
                              .flags = flags, .start = 0 };
    int v3 = r.ver >= 3u;
    if (v3)
        req.flags |= GA10B_RPC_BATCH_PARTIAL;
    r.q_count = 0;
    r.q_bytes = 0;
    /* the daemon hands a long batch back when its time budget is spent:
     * send the rest again from where it stopped */
    for (;;) {
        ga10b_rpc_batch_rep_t rep;
        memset(&rep, 0, sizeof(rep));
        if (rpc(GA10B_RPC_LAUNCH_BATCH, &req,
                v3 ? sizeof(req) : GA10B_RPC_BATCH_V2_SIZE, &rep,
                v3 ? sizeof(rep) : GA10B_RPC_BATCH_REP_V2_SIZE) != 0 ||
            rep.status != 0 || rep.done > req.count) {
            r.q_err = -1;
            break;
        }
        if (rep.done == req.count)
            break;
        if (!v3 || rep.done == 0 || rep.next <= req.start ||
            rep.next > req.bytes) {
            r.q_err = -1;
            break;
        }
        req.count -= rep.done;
        req.start = rep.next;
    }
    return r.q_err;
}

/* keyed contig shm, fresh key per allocation (the g2dclient idiom) */
static int shm_new(uint32_t size, int* id, void** va) {
    static uint32_t seq;
    uint32_t pid = (uint32_t)getpid();
    for (int tries = 0; tries < 256; tries++) {
        uint32_t key = 0x47505500u ^ ((pid & 0xffffu) << 8) ^ (seq++ & 0xffu);
        int sid = shmget((key_t)key, (int)size,
                         0666 | IPC_CREAT | IPC_EXCL | IPC_CONTIG);
        if (sid <= 0)
            continue;
        void* p = shmat(sid, 0, 0);
        if (p == (void*)-1) {
            shmctl(sid, IPC_RMID, NULL);
            continue;
        }
        *id = sid;
        *va = p;
        return 0;
    }
    return -1;
}

static void shm_drop(int id, void* va) {
    if (va != NULL)
        shmdt(va);
    if (id > 0)
        shmctl(id, IPC_RMID, NULL);
}

static void module_stub_free(struct ga10b_module* m) {
    while (m->kernels != NULL) {
        kmeta_t* k = m->kernels;
        m->kernels = k->next;
        free(k);
    }
    free(m);
}

static void remote_release(void) {
    pthread_mutex_lock(&r.lock);
    if (r.state == 1)
        rpc(GA10B_RPC_CLOSE, NULL, 0, NULL, 0);
    while (r.mods != NULL) {
        struct ga10b_module* m = r.mods;
        r.mods = m->next;
        module_stub_free(m);
    }
    shm_drop(r.req_id, r.req_va);
    r.req_id = 0;
    r.req_va = NULL;
    r.q_count = 0;
    r.q_bytes = 0;
    r.q_err = 0;
    r.state = 0;
    pthread_mutex_unlock(&r.lock);
}

int ga10b_dev_open(const char* fw_dir) {
    pthread_mutex_lock(&r.lock);
    if (r.state != 0) {
        int rc = r.state == 1 ? 0 : -1;
        pthread_mutex_unlock(&r.lock);
        return rc;
    }
    r.state = -1;
    r.pid = dev_get_pid(GA10B_RPC_DEV);
    if (r.pid <= 0) {
        klog("ga10b-remote: %s not running\n", GA10B_RPC_DEV);
        goto out;
    }
    r.req_size = GA10B_RPC_REQ_SHM_SIZE;
    if (shm_new(r.req_size, &r.req_id, &r.req_va) != 0) {
        klog("ga10b-remote: payload shm alloc failed\n");
        goto out;
    }
    ga10b_rpc_open_t req;
    memset(&req, 0, sizeof(req));
    /* a multi_task client may open from a worker thread; the daemon keys the
     * session by the owning process, so stamp that process's uuid */
    int32_t owner = (int32_t)getpid();
    procinfo_t pinfo;
    if (proc_info(owner, &pinfo) == 0 &&
        pinfo.type == TASK_TYPE_THREAD && pinfo.father_pid > 0)
        owner = pinfo.father_pid;
    req.uuid = proc_get_uuid(owner);
    req.shm_id = r.req_id;
    req.shm_size = r.req_size;
    if (fw_dir != NULL)
        strncpy(req.fw_dir, fw_dir, sizeof(req.fw_dir) - 1);
    ga10b_rpc_open_rep_t rep;
    if (rpc(GA10B_RPC_OPEN, &req, sizeof(req), &rep, sizeof(rep)) != 0) {
        shm_drop(r.req_id, r.req_va);
        r.req_id = 0;
        r.req_va = NULL;
        goto out;
    }
    r.slot_base = rep.slot_base;
    /* an older daemon takes shorter batches in the version 2 layout */
    uint32_t ver = 0;
    if (rpc(GA10B_RPC_PING, NULL, 0, &ver, sizeof(ver)) != 0)
        ver = 2u;
    r.ver = ver;
    r.batch_max = ver >= 3u ? GA10B_RPC_BATCH_MAX : GA10B_RPC_BATCH_MAX_V2;
    memset(r.map, 0, sizeof(r.map));
    r.tick = 0;
    r.state = 1;
    atexit(remote_release);
out:;
    int rc = r.state == 1 ? 0 : -1;
    pthread_mutex_unlock(&r.lock);
    return rc;
}

int ga10b_dev_ready(void) {
    return r.state == 1;
}

uint64_t ga10b_dev_map(uint64_t phy, uint32_t size) {
    pthread_mutex_lock(&r.lock);
    uint64_t va = 0;
    if (r.state != 1 || phy == 0 || size == 0 || (phy & 0xfffu) != 0 ||
        size > GA10B_RPC_SLOT_SPAN)
        goto out;
    uint32_t victim = 0;
    for (uint32_t i = 0; i < GA10B_RPC_SLOTS; i++) {
        if (r.map[i].phy == phy && r.map[i].size >= size) {
            r.map[i].used = ++r.tick;
            va = r.slot_base + i * GA10B_RPC_SLOT_SPAN;
            goto out;   /* mapping persists in the daemon VM: no RPC */
        }
        if (r.map[i].used < r.map[victim].used)
            victim = i;
    }
    /* a queued launch may still address the victim slot's old mapping */
    (void)queue_flush(0);
    ga10b_rpc_map_t req;
    req.va = r.slot_base + victim * GA10B_RPC_SLOT_SPAN;
    req.phy = phy;
    req.size = size;
    req.old_size = r.map[victim].size;
    if (rpc(GA10B_RPC_MAP, &req, sizeof(req), NULL, 0) != 0)
        goto out;
    r.map[victim].phy = phy;
    r.map[victim].size = size;
    r.map[victim].used = ++r.tick;
    va = req.va;
out:
    pthread_mutex_unlock(&r.lock);
    return va;
}

int ga10b_dev_alloc(uint32_t size, void** va, uint64_t* gpu_va) {
    pthread_mutex_lock(&r.lock);
    int rc = -1;
    if (r.state != 1 || size == 0)
        goto out;
    size = (size + 4095u) & ~4095u;
    ewokos_addr_t v = dma_alloc(GA10B_RPC_DMA_BLOCK, size, false);
    if (v == 0)
        goto out;
    ewokos_addr_t phy = dma_phy_addr(GA10B_RPC_DMA_BLOCK, v);
    if (phy == 0) {
        dma_free(GA10B_RPC_DMA_BLOCK, v);
        goto out;
    }
    memset((void*)v, 0, size);
    ga10b_rpc_alloc_t req = { .phy = (uint64_t)phy, .size = size };
    ga10b_rpc_alloc_rep_t rep;
    if (rpc(GA10B_RPC_ALLOC, &req, sizeof(req), &rep, sizeof(rep)) != 0) {
        dma_free(GA10B_RPC_DMA_BLOCK, v);
        goto out;
    }
    *va = (void*)v;
    *gpu_va = rep.gpu_va;
    rc = 0;
out:
    pthread_mutex_unlock(&r.lock);
    return rc;
}

void ga10b_dev_free(void* va, uint64_t gpu_va, uint32_t size) {
    pthread_mutex_lock(&r.lock);
    if (r.state == 1 && va != NULL) {
        (void)queue_flush(0);
        if (gpu_va != 0) {
            ga10b_rpc_free_t req = { .gpu_va = gpu_va, .size = size };
            rpc(GA10B_RPC_FREE, &req, sizeof(req), NULL, 0);
        }
        dma_free(GA10B_RPC_DMA_BLOCK, (ewokos_addr_t)va);
    }
    pthread_mutex_unlock(&r.lock);
}

int ga10b_dev_copy(uint64_t dst_va, uint64_t src_va, uint32_t bytes) {
    pthread_mutex_lock(&r.lock);
    int rc = -1;
    if (r.state == 1 && bytes != 0) {
        (void)queue_flush(0);
        ga10b_rpc_copy_t req = { .dst_va = dst_va, .src_va = src_va,
                                 .bytes = bytes };
        rc = rpc(GA10B_RPC_COPY, &req, sizeof(req), NULL, 0) == 0 ? 0 : -1;
    }
    pthread_mutex_unlock(&r.lock);
    return rc;
}

int ga10b_dev_sync_to_gpu(void) {
    pthread_mutex_lock(&r.lock);
    ga10b_rpc_sync_t req = { .dir = GA10B_RPC_SYNC_TO_GPU };
    int rc = -1;
    if (r.state == 1) {
        (void)queue_flush(0);
        rc = rpc(GA10B_RPC_SYNC, &req, sizeof(req), NULL, 0);
    }
    pthread_mutex_unlock(&r.lock);
    return rc;
}

int ga10b_dev_sync_to_cpu(void) {
    pthread_mutex_lock(&r.lock);
    ga10b_rpc_sync_t req = { .dir = GA10B_RPC_SYNC_TO_CPU };
    int rc = -1;
    if (r.state == 1) {
        /* with launches queued the L2 flush rides their batch RPC */
        rc = r.q_count != 0 ? queue_flush(GA10B_RPC_BATCH_SYNC_TO_CPU)
                            : rpc(GA10B_RPC_SYNC, &req, sizeof(req), NULL, 0);
    }
    pthread_mutex_unlock(&r.lock);
    return rc;
}

ga10b_module_t* ga10b_dev_module(const void* cubin, uint32_t size) {
    pthread_mutex_lock(&r.lock);
    struct ga10b_module* m = NULL;
    if (r.state != 1 || cubin == NULL || size == 0 || size > (16u << 20))
        goto out;
    int sid = 0;
    void* image = NULL;
    if (shm_new((size + 4095u) & ~4095u, &sid, &image) != 0)
        goto out;
    memcpy(image, cubin, size);
    ga10b_rpc_module_t req = { .shm_id = sid, .size = size };
    ga10b_rpc_module_rep_t rep;
    int rc = rpc(GA10B_RPC_MODULE_LOAD, &req, sizeof(req), &rep, sizeof(rep));
    shm_drop(sid, image);
    if (rc != 0)
        goto out;
    m = calloc(1, sizeof(*m));
    if (m == NULL)
        goto out;
    m->handle = rep.handle;
    m->next = r.mods;
    r.mods = m;
out:
    pthread_mutex_unlock(&r.lock);
    return m;
}

static kmeta_t* module_kmeta(struct ga10b_module* m, int kid) {
    for (kmeta_t* k = m->kernels; k != NULL; k = k->next)
        if (k->kid == kid)
            return k;
    return NULL;
}

int ga10b_module_kernel(const ga10b_module_t* mm, const char* name) {
    struct ga10b_module* m = (struct ga10b_module*)mm;
    pthread_mutex_lock(&r.lock);
    int kid = -1;
    if (r.state != 1 || m == NULL || name == NULL)
        goto out;
    for (kmeta_t* k = m->kernels; k != NULL; k = k->next) {
        if (strcmp(k->name, name) == 0) {
            kid = k->kid;
            goto out;
        }
    }
    ga10b_rpc_kfind_t req;
    memset(&req, 0, sizeof(req));
    req.handle = m->handle;
    strncpy(req.name, name, sizeof(req.name) - 1);
    ga10b_rpc_kfind_rep_t rep;
    if (rpc(GA10B_RPC_KERNEL_FIND, &req, sizeof(req), &rep, sizeof(rep)) != 0)
        goto out;
    kid = rep.kid;
    if (kid >= 0) {
        kmeta_t* k = calloc(1, sizeof(*k));
        if (k != NULL) {
            strncpy(k->name, name, sizeof(k->name) - 1);
            k->kid = kid;
            k->next = m->kernels;
            m->kernels = k;
        }
    }
out:
    pthread_mutex_unlock(&r.lock);
    return kid;
}

/* caller holds r.lock */
static const ga10b_rpc_kinfo_rep_t* launch_info(struct ga10b_module* m,
                                                int kid) {
    kmeta_t* k = module_kmeta(m, kid);
    if (k == NULL) {
        k = calloc(1, sizeof(*k));
        if (k == NULL)
            return NULL;
        k->kid = kid;
        k->next = m->kernels;
        m->kernels = k;
    }
    if (!k->have_info) {
        ga10b_rpc_kinfo_t req = { .handle = m->handle, .kid = kid };
        if (rpc(GA10B_RPC_KERNEL_INFO, &req, sizeof(req),
                &k->info, sizeof(k->info)) != 0)
            return NULL;
        k->have_info = 1;
    }
    return &k->info;
}

int ga10b_dev_launch(ga10b_module_t* m, int kernel, const uint32_t grid[3],
                     const uint32_t block[3], void* const* args) {
    return ga10b_dev_launch_dyn(m, kernel, grid, block, args, 0);
}

int ga10b_dev_launch_dyn(ga10b_module_t* m, int kernel, const uint32_t grid[3],
                         const uint32_t block[3], void* const* args,
                         uint32_t dyn_smem) {
    pthread_mutex_lock(&r.lock);
    int rc = -1;
    if (r.state != 1 || m == NULL)
        goto out;
    const ga10b_rpc_kinfo_rep_t* ki = launch_info(m, kernel);
    if (ki == NULL || ki->param_total > r.req_size)
        goto out;
    (void)queue_flush(0);   /* keeps submission order; the shm is shared */
    uint8_t* blob = (uint8_t*)r.req_va;
    memset(blob, 0, ki->param_total);
    for (uint32_t i = 0; i < ki->nparams; i++) {
        if ((uint32_t)ki->off[i] + ki->sz[i] > ki->param_total)
            goto out;
        memcpy(blob + ki->off[i], args[i], ki->sz[i]);
    }
    ga10b_rpc_launch_t req;
    memset(&req, 0, sizeof(req));
    req.handle = m->handle;
    req.kid = kernel;
    memcpy(req.grid, grid, sizeof(req.grid));
    memcpy(req.block, block, sizeof(req.block));
    req.dyn_smem = dyn_smem;
    req.blob_off = 0;
    req.blob_len = ki->param_total;
    rc = rpc(GA10B_RPC_LAUNCH, &req, sizeof(req), NULL, 0) == 0 ? 0 : -1;
out:
    pthread_mutex_unlock(&r.lock);
    return rc;
}

int ga10b_dev_launch_async(ga10b_module_t* m, int kernel,
                           const uint32_t grid[3], const uint32_t block[3],
                           void* const* args, uint32_t dyn_smem) {
    pthread_mutex_lock(&r.lock);
    int rc = -1;
    if (r.state != 1 || m == NULL || r.q_err != 0)
        goto out;
    const ga10b_rpc_kinfo_rep_t* ki = launch_info(m, kernel);
    if (ki == NULL)
        goto out;
    ga10b_rpc_launch_t rec;
    uint32_t need = (uint32_t)sizeof(rec) + ki->param_total;
    if (need > r.req_size)
        goto out;
    for (uint32_t i = 0; i < ki->nparams; i++)
        if ((uint32_t)ki->off[i] + ki->sz[i] > ki->param_total)
            goto out;
    if (need > r.req_size - r.q_bytes && queue_flush(0) != 0)
        goto out;
    memset(&rec, 0, sizeof(rec));
    rec.handle = m->handle;
    rec.kid = kernel;
    memcpy(rec.grid, grid, sizeof(rec.grid));
    memcpy(rec.block, block, sizeof(rec.block));
    rec.dyn_smem = dyn_smem;
    rec.blob_off = r.q_bytes + (uint32_t)sizeof(rec);
    rec.blob_len = ki->param_total;
    uint8_t* blob = (uint8_t*)r.req_va + rec.blob_off;
    memcpy((uint8_t*)r.req_va + r.q_bytes, &rec, sizeof(rec));
    memset(blob, 0, ki->param_total);
    for (uint32_t i = 0; i < ki->nparams; i++)
        memcpy(blob + ki->off[i], args[i], ki->sz[i]);
    r.q_bytes = (rec.blob_off + rec.blob_len + 7u) & ~7u;
    r.q_count++;
    rc = r.q_count >= r.batch_max ? queue_flush(0) : 0;
out:
    pthread_mutex_unlock(&r.lock);
    return rc;
}

int ga10b_dev_flush(void) {
    pthread_mutex_lock(&r.lock);
    int rc = r.state == 1 ? queue_flush(0) : -1;
    pthread_mutex_unlock(&r.lock);
    return rc;
}

int ga10b_dev_launch_compute(ga10b_module_t* m, int kernel,
                             const uint32_t grid[3], const uint32_t block[3],
                             const uint32_t* unif, uint32_t unif_dwords,
                             const uint64_t* ssbo_va, uint32_t nssbo,
                             const uint32_t* texdesc, uint32_t texdesc_dwords) {
    pthread_mutex_lock(&r.lock);
    int rc = -1;
    if (r.state != 1 || m == NULL)
        goto out;
    uint32_t ub = unif_dwords * 4u, sb = nssbo * 8u, tb = texdesc_dwords * 4u;
    if (ub + sb + tb > r.req_size)
        goto out;
    (void)queue_flush(0);
    uint8_t* p = (uint8_t*)r.req_va;
    if (ub != 0)
        memcpy(p, unif, ub);
    if (sb != 0)
        memcpy(p + ub, ssbo_va, sb);
    if (tb != 0)
        memcpy(p + ub + sb, texdesc, tb);
    ga10b_rpc_launchc_t req;
    memset(&req, 0, sizeof(req));
    req.handle = m->handle;
    req.kid = kernel;
    memcpy(req.grid, grid, sizeof(req.grid));
    memcpy(req.block, block, sizeof(req.block));
    req.unif_dwords = unif_dwords;
    req.nssbo = nssbo;
    req.texdesc_dwords = texdesc_dwords;
    req.off = 0;
    rc = rpc(GA10B_RPC_LAUNCH_COMPUTE, &req, sizeof(req), NULL, 0) == 0 ? 0 : -1;
out:
    pthread_mutex_unlock(&r.lock);
    return rc;
}