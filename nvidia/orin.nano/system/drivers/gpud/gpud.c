/*
 * gpud -- resident GA10B GPU daemon: the single GPU owner, serving many
 * client processes over /dev/gpu (see libs/ga10b/include/ga10b/ga10b_rpc.h).
 *
 * libga10b's remote implementation (libga10br) exports the same ga10b_dev.h
 * surface the in-process driver does, so kuda apps and g2dd's bsp_g3d become
 * clients without source changes.  The daemon runs the secure bring-up once
 * (lazily, on the first OPEN) and serializes every request: one VM, one
 * compute channel, launches are synchronous.  Each client owns a 4GB GPU VA
 * slot window plus persistent bump mappings; client death is detected by the
 * loop_step uuid sweep and tears the session's mappings/modules down.
 *
 * Idle at boot: no GPU MMIO happens until a client opens the device, so the
 * ga10bd bring-up CLI keeps working on a fresh boot until the first client.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/shm.h>

#include <ewoksys/kernel_tic.h>
#include <ewoksys/klog.h>
#include <ewoksys/proc.h>
#include <ewoksys/proto.h>
#include <ewoksys/vdevice.h>

#include "ga10b.h"
#include "ga10b_devd.h"
#include <ga10b/ga10b_rpc.h>
#include <bsp/bsp_bpmp.h>

typedef struct st_alloc {
    uint64_t va;
    uint32_t size;
    struct st_alloc* next;
} gpud_alloc_t;

typedef struct st_mod {
    int32_t handle;
    ga10b_module_t* m;
    struct st_mod* next;
} gpud_mod_t;

typedef struct {
    uint8_t used;
    uint32_t pid;
    uint32_t uuid;
    uint32_t idx;
    uint64_t slot_base;
    void* req_va;
    uint32_t req_size;
    uint32_t slot_size[GA10B_RPC_SLOTS];
    gpud_alloc_t* allocs;
    gpud_mod_t* mods;
} gpud_sess_t;

static gpud_sess_t _sess[GA10B_RPC_MAX_CLIENTS];
static int _dev_up;
static int32_t _next_handle = 1;

static gpud_sess_t* sess_find(uint32_t pid) {
    for (uint32_t i = 0; i < GA10B_RPC_MAX_CLIENTS; i++)
        if (_sess[i].used && _sess[i].pid == pid)
            return &_sess[i];
    return NULL;
}

static void sess_cleanup(gpud_sess_t* s) {
    for (uint32_t i = 0; i < GA10B_RPC_SLOTS; i++) {
        if (s->slot_size[i] != 0)
            ga10b_devd_unmap(s->slot_base + i * GA10B_RPC_SLOT_SPAN,
                             s->slot_size[i]);
    }
    while (s->allocs != NULL) {
        gpud_alloc_t* a = s->allocs;
        s->allocs = a->next;
        ga10b_devd_unmap(a->va, a->size);
        free(a);
    }
    while (s->mods != NULL) {
        gpud_mod_t* m = s->mods;
        s->mods = m->next;
        ga10b_dev_module_free(m->m);
        free(m);
    }
    if (s->req_va != NULL)
        shmdt(s->req_va);
    klog("gpud: client pid %u released (idx %u)\n", s->pid, s->idx);
    memset(s, 0, sizeof(*s));
}

/* pid-slot reuse guard: a session whose owner is gone (or whose pid now
 * belongs to a different process) is torn down before anything else runs */
static gpud_sess_t* sess_live(uint32_t pid) {
    gpud_sess_t* s = sess_find(pid);
    if (s == NULL)
        return NULL;
    if (!ga10b_os_alive(s->pid, s->uuid)) {
        sess_cleanup(s);
        return NULL;
    }
    return s;
}

static ga10b_module_t* sess_module(gpud_sess_t* s, int32_t handle) {
    for (gpud_mod_t* m = s->mods; m != NULL; m = m->next)
        if (m->handle == handle)
            return m->m;
    return NULL;
}

static int do_open(uint32_t pid, const ga10b_rpc_open_t* req,
                   ga10b_rpc_open_rep_t* rep) {
    gpud_sess_t* s = sess_live(pid);
    if (s != NULL) { /* idempotent re-open */
        rep->slot_base = s->slot_base;
        rep->client_idx = s->idx;
        return 0;
    }
    if (req->uuid == 0 || req->shm_id <= 0 ||
        req->shm_size < 4096 || req->shm_size > (4u << 20))
        return GA10B_RPC_E_INVAL;
    if (!_dev_up) {
        const char* fw = req->fw_dir[0] != 0 ? req->fw_dir : NULL;
        if (ga10b_dev_open(fw) != 0) {
            klog("gpud: GPU bring-up failed (driver klog has the stage)\n");
            return GA10B_RPC_E_NODEV;
        }
        _dev_up = 1;
    }
    uint32_t idx = 0;
    while (idx < GA10B_RPC_MAX_CLIENTS && _sess[idx].used)
        idx++;
    if (idx == GA10B_RPC_MAX_CLIENTS)
        return GA10B_RPC_E_NOMEM;
    void* req_va = shmat(req->shm_id, 0, 0);
    if (req_va == (void*)-1)
        return GA10B_RPC_E_INVAL;
    s = &_sess[idx];
    memset(s, 0, sizeof(*s));
    s->used = 1;
    s->pid = pid;
    s->uuid = req->uuid;
    s->idx = idx;
    s->slot_base = GA10B_RPC_WINDOW_BASE + idx * GA10B_RPC_WINDOW_SPAN;
    s->req_va = req_va;
    s->req_size = req->shm_size;
    rep->slot_base = s->slot_base;
    rep->client_idx = idx;
    klog("gpud: client pid %u opened (idx %u, window %llx)\n", pid, idx,
         (unsigned long long)s->slot_base);
    return 0;
}

static int do_map(gpud_sess_t* s, const ga10b_rpc_map_t* req) {
    if (req->va < s->slot_base ||
        req->va >= s->slot_base + GA10B_RPC_WINDOW_SPAN ||
        ((req->va - s->slot_base) % GA10B_RPC_SLOT_SPAN) != 0 ||
        req->size == 0 || req->size > GA10B_RPC_SLOT_SPAN ||
        req->old_size > GA10B_RPC_SLOT_SPAN ||
        req->phy == 0 || (req->phy & 0xfffu) != 0)
        return GA10B_RPC_E_INVAL;
    if (ga10b_devd_map_at(req->va, req->phy, req->size, req->old_size) != 0)
        return GA10B_RPC_E_FAILED;
    uint32_t i = (uint32_t)((req->va - s->slot_base) / GA10B_RPC_SLOT_SPAN);
    s->slot_size[i] = req->size;
    return 0;
}

static int do_alloc(gpud_sess_t* s, const ga10b_rpc_alloc_t* req,
                    ga10b_rpc_alloc_rep_t* rep) {
    uint64_t va = ga10b_devd_map(req->phy, req->size);
    if (va == 0)
        return GA10B_RPC_E_NOMEM;
    gpud_alloc_t* a = calloc(1, sizeof(*a));
    if (a == NULL) {
        ga10b_devd_unmap(va, req->size);
        return GA10B_RPC_E_NOMEM;
    }
    a->va = va;
    a->size = (req->size + 4095u) & ~4095u;
    a->next = s->allocs;
    s->allocs = a;
    rep->gpu_va = va;
    return 0;
}

static int do_free(gpud_sess_t* s, const ga10b_rpc_free_t* req) {
    gpud_alloc_t** pp = &s->allocs;
    while (*pp != NULL && (*pp)->va != req->gpu_va)
        pp = &(*pp)->next;
    if (*pp == NULL)
        return GA10B_RPC_E_INVAL;
    gpud_alloc_t* a = *pp;
    *pp = a->next;
    ga10b_devd_unmap(a->va, a->size);
    free(a);
    return 0;
}

static int do_module_load(gpud_sess_t* s, const ga10b_rpc_module_t* req,
                          ga10b_rpc_module_rep_t* rep) {
    if (req->shm_id <= 0 || req->size == 0 || req->size > (16u << 20))
        return GA10B_RPC_E_INVAL;
    void* image = shmat(req->shm_id, 0, 0);
    if (image == (void*)-1)
        return GA10B_RPC_E_INVAL;
    ga10b_module_t* m = ga10b_dev_module(image, req->size);
    shmdt(image);
    if (m == NULL)
        return GA10B_RPC_E_FAILED;
    gpud_mod_t* e = calloc(1, sizeof(*e));
    if (e == NULL) {
        ga10b_dev_module_free(m);
        return GA10B_RPC_E_NOMEM;
    }
    e->handle = _next_handle++;
    e->m = m;
    e->next = s->mods;
    s->mods = e;
    rep->handle = e->handle;
    return 0;
}

/* runtime switch (devcmd /dev/gpu pipeline on|off): batched launches are
 * queued on the channel and collected per chunk instead of one at a time */
static int _pipeline = 1;
#define GPUD_QUEUE_CHUNK 64u    /* launches queued between two drains */

/* Launch profile, off by default (devcmd /dev/gpu prof on|off|reset|dump):
 * synchronous launches use CPU wait time; queued launches use adjacent GPU
 * release timestamps.  A queued delta includes the preceding WFI/dispatch
 * gap as well as the named kernel, not pure shader execution time. */
#define GPUD_PROF_MAX 48u
#define GPUD_PROF_HIST 12u
typedef struct {
    char name[GA10B_RPC_NAME_MAX];
    uint32_t n, queued;
    uint64_t ns, min, max;
    uint32_t hist[GPUD_PROF_HIST];
} gpud_prof_t;
static gpud_prof_t _prof[GPUD_PROF_MAX];
static uint32_t _prof_n;
static int _prof_on;
static uint64_t _prof_ts_origin, _prof_ts_prev;
static uint32_t _prof_ts_valid, _prof_ts_bad;
/* submit_ns: CPU time building/submitting launches; drain_ns: retirement wait */
static struct {
    uint32_t batches, launches;
    uint64_t ns, submit_ns, drain_ns;
} _prof_batch;

static gpud_prof_t* prof_add(const char* name, uint64_t ns, int timed) {
    if (name == NULL)
        name = "?";
    gpud_prof_t* e = NULL;
    for (uint32_t i = 0; i < _prof_n && e == NULL; i++)
        if (strncmp(_prof[i].name, name, sizeof(_prof[i].name) - 1u) == 0)
            e = &_prof[i];
    if (e == NULL) {
        if (_prof_n == GPUD_PROF_MAX)
            return NULL;
        e = &_prof[_prof_n++];
        memset(e, 0, sizeof(*e));
        strncpy(e->name, name, sizeof(e->name) - 1u);
        e->min = ~0ull;
    }
    if (!timed) {
        e->queued++;
        return e;
    }
    e->n++;
    e->ns += ns;
    if (ns < e->min)
        e->min = ns;
    if (ns > e->max)
        e->max = ns;
    static const uint32_t edge_us[GPUD_PROF_HIST - 1u] =
        { 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096 };
    uint32_t b = 0;
    while (b < GPUD_PROF_HIST - 1u && ns >= (uint64_t)edge_us[b] * 1000u)
        b++;
    e->hist[b]++;
    return e;
}

static void prof_reset(void) {
    _prof_n = 0;
    _prof_ts_origin = _prof_ts_prev = 0;
    _prof_ts_valid = _prof_ts_bad = 0;
    memset(&_prof_batch, 0, sizeof(_prof_batch));
}

static void prof_dump(void) {
    klog("gpud prof: %u batches %u launches %u us (submit %u us, drain wait "
         "%u us)\n", _prof_batch.batches, _prof_batch.launches,
         (uint32_t)(_prof_batch.ns / 1000u),
         (uint32_t)(_prof_batch.submit_ns / 1000u),
         (uint32_t)(_prof_batch.drain_ns / 1000u));
    klog("gpud prof: GPU release span %u us; queued ns = release-to-release "
         "(includes WFI/gaps), timestamp errors %u\n",
         _prof_ts_valid ? (uint32_t)((_prof_ts_prev - _prof_ts_origin) / 1000u) : 0u,
         _prof_ts_bad);
    for (uint32_t i = 0; i < _prof_n; i++) {
        const gpud_prof_t* e = &_prof[i];
        klog("gpud prof: %s n %u total %u us avg %u ns min %u max %u "
             "queued %u\n", e->name, e->n, (uint32_t)(e->ns / 1000u),
             e->n != 0 ? (uint32_t)(e->ns / e->n) : 0u,
             e->n != 0 ? (uint32_t)e->min : 0u, (uint32_t)e->max, e->queued);
        klog("gpud prof: %s hist us <4:%u <8:%u <16:%u <32:%u <64:%u "
             "<128:%u <256:%u <512:%u <1024:%u <2048:%u <4096:%u >=:%u\n",
             e->name, e->hist[0], e->hist[1], e->hist[2], e->hist[3],
             e->hist[4], e->hist[5], e->hist[6], e->hist[7], e->hist[8],
             e->hist[9], e->hist[10], e->hist[11]);
    }
}

/* The BPMP only follows bus activity: after idle the memory clock needs about
 * half a second of load to climb back from its lowest rate, and launches run
 * several times slower until it does.  Hold the top rate while GPU work keeps
 * arriving; loop_step drops it after GPUD_MEMCLK_IDLE_NS without any. */
#define GPUD_MEMCLK_IDLE_NS 1000000000ull
static struct {
    int on;
    int held;
    int failed;
    uint64_t last;             /* end of the latest GPU work */
} _memclk = { 1, 0, 0, 0 };

static void memclk_hold(void) {
    if (!_memclk.on || _memclk.held || _memclk.failed)
        return;
    if (bsp_bpmp_init() != 0 || bsp_bpmp_gpu_memclk_hold(1) != 0) {
        klog("gpud: memory clock floor unavailable\n");
        _memclk.failed = 1;
        return;
    }
    _memclk.held = 1;
}

static void memclk_drop(void) {
    if (!_memclk.held)
        return;
    bsp_bpmp_gpu_memclk_hold(0);
    _memclk.held = 0;
}

/* queue != 0: 1 when the launch cannot be queued right now */
static int do_launch(gpud_sess_t* s, const ga10b_rpc_launch_t* req, int queue,
                     gpud_prof_t** queued_prof) {
    if (queued_prof != NULL)
        *queued_prof = NULL;
    ga10b_module_t* m = sess_module(s, req->handle);
    if (m == NULL || req->blob_len == 0 ||
        req->blob_off > s->req_size ||
        req->blob_len > s->req_size - req->blob_off)
        return GA10B_RPC_E_INVAL;
    ga10b_rpc_kinfo_rep_t ki;
    if (ga10b_devd_kernel_info(m, req->kid, &ki) != 0 ||
        ki.param_total > req->blob_len)
        return GA10B_RPC_E_INVAL;
    /* the client packed each param at its cbank0 offset; hand the driver
     * pointers back into that blob (cubin_pack_params re-packs in place) */
    void* args[GA10B_RPC_MAX_PARAMS];
    uint8_t* blob = (uint8_t*)s->req_va + req->blob_off;
    for (uint32_t i = 0; i < ki.nparams; i++) {
        if ((uint32_t)ki.off[i] + ki.sz[i] > req->blob_len)
            return GA10B_RPC_E_INVAL;
        args[i] = blob + ki.off[i];
    }
    uint64_t t0 = 0, t1 = 0;
    if (_prof_on)
        kernel_tic_nsec(&t0);
    int rc = queue ? ga10b_dev_launch_queue(m, req->kid, req->grid, req->block,
                                            args, req->dyn_smem)
                   : ga10b_dev_launch_dyn(m, req->kid, req->grid, req->block,
                                          args, req->dyn_smem);
    if (_prof_on && rc == 0) {
        kernel_tic_nsec(&t1);
        gpud_prof_t* e = prof_add(ga10b_devd_kernel_name(m, req->kid),
                                  t1 - t0, !queue);
        if (queue) {
            _prof_batch.submit_ns += t1 - t0;
            if (queued_prof != NULL)
                *queued_prof = e;
        }
    }
    return rc < 0 ? GA10B_RPC_E_FAILED : rc;
}

/* collect the queued launches of a batch; next[i] = resume offset after the
 * i-th of them.  Nonzero when some did not retire (reported in rep). */
static int batch_drain(ga10b_rpc_batch_rep_t* rep, const uint32_t* next,
                       gpud_prof_t* const* qprof, uint32_t* queued) {
    uint32_t n = *queued, retired = 0;
    *queued = 0;
    if (n == 0)
        return 0;
    uint64_t t0 = 0, t1 = 0;
    if (_prof_on)
        kernel_tic_nsec(&t0);
    int rc = ga10b_dev_launch_drain(&retired);
    if (_prof_on) {
        kernel_tic_nsec(&t1);
        _prof_batch.drain_ns += t1 - t0;
    }
    if (retired > n)
        retired = n;
    if (_prof_on) {
        for (uint32_t i = 0; i < retired; i++) {
            uint64_t ts = 0;
            if (qprof[i] == NULL ||
                ga10b_dev_queue_timestamp(i, &ts) != 0 ||
                (_prof_ts_valid && ts < _prof_ts_prev)) {
                _prof_ts_bad++;
                _prof_ts_valid = 0;
                continue;
            }
            if (_prof_ts_valid)
                prof_add(qprof[i]->name, ts - _prof_ts_prev, 1);
            else
                _prof_ts_origin = ts;
            _prof_ts_prev = ts;
            _prof_ts_valid = 1;
        }
    }
    rep->done += retired;
    if (retired != 0)
        rep->next = next[retired - 1u];
    if (rc != 0 || retired != n) {
        rep->status = GA10B_RPC_E_FAILED;
        return -1;
    }
    return 0;
}

/* One IPC round trip for a run of launches.  A malformed batch header is
 * refused outright; a bad or failing record stops the run and is reported
 * through the reply, so the client knows how many launches retired.  A
 * client that can resume (PARTIAL) is also sent back once the time budget
 * is spent, so a long batch cannot starve the other clients. */
static int batch_run(gpud_sess_t* s, const ga10b_rpc_batch_t* req,
                     ga10b_rpc_batch_rep_t* rep) {
    rep->done = 0;
    rep->status = 0;
    rep->next = req->start;
    if (req->count == 0 || req->count > GA10B_RPC_BATCH_MAX ||
        req->bytes > s->req_size || req->start > req->bytes ||
        (req->start & 7u) != 0)
        return GA10B_RPC_E_INVAL;
    uint64_t t0 = 0, now = 0;
    kernel_tic_nsec(&t0);
    uint32_t off = req->start;
    uint32_t qnext[GPUD_QUEUE_CHUNK], qn = 0;
    gpud_prof_t* qprof[GPUD_QUEUE_CHUNK];
    for (uint32_t i = 0; i < req->count; i++) {
        /* validate a private copy: the client can still write the shm */
        ga10b_rpc_launch_t rec;
        gpud_prof_t* qe = NULL;
        int rc = GA10B_RPC_E_INVAL, queued = 0;
        if (off <= req->bytes && sizeof(rec) <= req->bytes - off) {
            memcpy(&rec, (uint8_t*)s->req_va + off, sizeof(rec));
            if (rec.blob_off == off + sizeof(rec) &&
                rec.blob_len <= req->bytes - rec.blob_off) {
                rc = do_launch(s, &rec, _pipeline, &qe);
                if (rc > 0) {
                    /* not queueable: collect what is queued and retry; a
                     * launch that still does not fit runs synchronously */
                    if (batch_drain(rep, qnext, qprof, &qn) != 0)
                        return 0;
                    if (rep->done != 0 &&
                        (req->flags & GA10B_RPC_BATCH_PARTIAL) != 0) {
                        kernel_tic_nsec(&now);
                        if (now - t0 > (uint64_t)GA10B_RPC_BATCH_BUDGET_US * 1000u)
                            return 0;
                    }
                    rc = do_launch(s, &rec, 1, &qe);
                    if (rc > 0)
                        rc = do_launch(s, &rec, 0, NULL);
                    else
                        queued = rc == 0;
                } else {
                    queued = _pipeline && rc == 0;
                }
            }
        }
        if (rc != 0) {
            if (batch_drain(rep, qnext, qprof, &qn) == 0)
                rep->status = rc;
            return 0;
        }
        off = (rec.blob_off + rec.blob_len + 7u) & ~7u;
        if (queued) {
            qnext[qn] = off;
            qprof[qn++] = qe;
            if (qn < GPUD_QUEUE_CHUNK && i + 1 < req->count)
                continue;
            if (batch_drain(rep, qnext, qprof, &qn) != 0)
                return 0;
        } else {
            rep->done++;
            rep->next = off;
        }
        if ((req->flags & GA10B_RPC_BATCH_PARTIAL) != 0 && i + 1 < req->count) {
            kernel_tic_nsec(&now);
            if (now - t0 > (uint64_t)GA10B_RPC_BATCH_BUDGET_US * 1000u)
                return 0;
        }
    }
    if ((req->flags & GA10B_RPC_BATCH_SYNC_TO_CPU) != 0) {
        if (_prof_on && qn != 0 && batch_drain(rep, qnext, qprof, &qn) != 0)
            return 0;
        if (_prof_on)
            kernel_tic_nsec(&t0);
        if (ga10b_dev_sync_to_cpu() != 0)
            rep->status = GA10B_RPC_E_FAILED;
        if (_prof_on) {
            kernel_tic_nsec(&now);
            prof_add("<sync_to_cpu>", now - t0, 1);
        }
    }
    return 0;
}

static int do_launch_batch(gpud_sess_t* s, const ga10b_rpc_batch_t* req,
                           ga10b_rpc_batch_rep_t* rep) {
    if (!_prof_on)
        return batch_run(s, req, rep);
    uint64_t t0 = 0, t1 = 0;
    kernel_tic_nsec(&t0);
    int rc = batch_run(s, req, rep);
    kernel_tic_nsec(&t1);
    if (rc == 0) {
        _prof_batch.batches++;
        _prof_batch.launches += rep->done;
        _prof_batch.ns += t1 - t0;
    }
    return rc;
}

static int do_launch_compute(gpud_sess_t* s, const ga10b_rpc_launchc_t* req) {
    ga10b_module_t* m = sess_module(s, req->handle);
    uint32_t bytes = req->unif_dwords * 4u + req->nssbo * 8u +
                     req->texdesc_dwords * 4u;
    if (m == NULL || req->off > s->req_size || bytes > s->req_size - req->off)
        return GA10B_RPC_E_INVAL;
    uint8_t* p = (uint8_t*)s->req_va + req->off;
    const uint32_t* unif = (const uint32_t*)p;
    const uint64_t* ssbo = (const uint64_t*)(p + req->unif_dwords * 4u);
    const uint32_t* texdesc =
        (const uint32_t*)(p + req->unif_dwords * 4u + req->nssbo * 8u);
    if (ga10b_dev_launch_compute(m, req->kid, req->grid, req->block,
                                 unif, req->unif_dwords, ssbo, req->nssbo,
                                 texdesc, req->texdesc_dwords) != 0)
        return GA10B_RPC_E_FAILED;
    return 0;
}

static int gpud_request(vdevice_t* dev, int from_pid, int cmd,
                        proto_t* in, proto_t* ret, void* p) {
    (void)dev;
    (void)p;
    /* multi_task clients (g2dd) issue RPCs from kernel worker threads, each
     * with its own pid slot: key the session by the owning process so every
     * worker of one client shares its VA window and allocations */
    uint32_t pid = (uint32_t)from_pid;
    procinfo_t pinfo;
    if (proc_info((int32_t)pid, &pinfo) == 0 &&
        pinfo.type == TASK_TYPE_THREAD && pinfo.father_pid > 0)
        pid = (uint32_t)pinfo.father_pid;
    int32_t sz = 0;
    const void* req = proto_read(in, &sz);

    if (cmd == GA10B_RPC_PING) {
        PF->addi(ret, (int32_t)GA10B_RPC_VERSION);
        return 0;
    }
    if (cmd == GA10B_RPC_OPEN) {
        if (req == NULL || sz != (int32_t)sizeof(ga10b_rpc_open_t))
            return GA10B_RPC_E_INVAL;
        ga10b_rpc_open_rep_t rep;
        int rc = do_open(pid, (const ga10b_rpc_open_t*)req, &rep);
        if (rc == 0)
            PF->add(ret, &rep, sizeof(rep));
        return rc;
    }

    gpud_sess_t* s = sess_live(pid);
    if (s == NULL)
        return GA10B_RPC_E_NOSESSION;

    switch (cmd) {
    case GA10B_RPC_CLOSE:
        sess_cleanup(s);
        return 0;
    case GA10B_RPC_MAP:
        if (req == NULL || sz != (int32_t)sizeof(ga10b_rpc_map_t))
            return GA10B_RPC_E_INVAL;
        return do_map(s, (const ga10b_rpc_map_t*)req);
    case GA10B_RPC_ALLOC: {
        if (req == NULL || sz != (int32_t)sizeof(ga10b_rpc_alloc_t))
            return GA10B_RPC_E_INVAL;
        ga10b_rpc_alloc_rep_t rep;
        int rc = do_alloc(s, (const ga10b_rpc_alloc_t*)req, &rep);
        if (rc == 0)
            PF->add(ret, &rep, sizeof(rep));
        return rc;
    }
    case GA10B_RPC_FREE:
        if (req == NULL || sz != (int32_t)sizeof(ga10b_rpc_free_t))
            return GA10B_RPC_E_INVAL;
        return do_free(s, (const ga10b_rpc_free_t*)req);
    case GA10B_RPC_COPY: {
        if (req == NULL || sz != (int32_t)sizeof(ga10b_rpc_copy_t))
            return GA10B_RPC_E_INVAL;
        const ga10b_rpc_copy_t* r = (const ga10b_rpc_copy_t*)req;
        if (ga10b_dev_copy(r->dst_va, r->src_va, r->bytes) != 0)
            return GA10B_RPC_E_FAILED;
        return 0;
    }
    case GA10B_RPC_SYNC: {
        if (req == NULL || sz != (int32_t)sizeof(ga10b_rpc_sync_t))
            return GA10B_RPC_E_INVAL;
        const ga10b_rpc_sync_t* r = (const ga10b_rpc_sync_t*)req;
        int rc = r->dir == GA10B_RPC_SYNC_TO_GPU ?
                 ga10b_dev_sync_to_gpu() : ga10b_dev_sync_to_cpu();
        return rc == 0 ? 0 : GA10B_RPC_E_FAILED;
    }
    case GA10B_RPC_MODULE_LOAD: {
        if (req == NULL || sz != (int32_t)sizeof(ga10b_rpc_module_t))
            return GA10B_RPC_E_INVAL;
        ga10b_rpc_module_rep_t rep;
        int rc = do_module_load(s, (const ga10b_rpc_module_t*)req, &rep);
        if (rc == 0)
            PF->add(ret, &rep, sizeof(rep));
        return rc;
    }
    case GA10B_RPC_KERNEL_FIND: {
        if (req == NULL || sz != (int32_t)sizeof(ga10b_rpc_kfind_t))
            return GA10B_RPC_E_INVAL;
        const ga10b_rpc_kfind_t* r = (const ga10b_rpc_kfind_t*)req;
        ga10b_module_t* m = sess_module(s, r->handle);
        if (m == NULL)
            return GA10B_RPC_E_INVAL;
        ga10b_rpc_kfind_rep_t rep;
        rep.kid = ga10b_module_kernel(m, r->name);
        PF->add(ret, &rep, sizeof(rep));
        return 0;
    }
    case GA10B_RPC_KERNEL_INFO: {
        if (req == NULL || sz != (int32_t)sizeof(ga10b_rpc_kinfo_t))
            return GA10B_RPC_E_INVAL;
        const ga10b_rpc_kinfo_t* r = (const ga10b_rpc_kinfo_t*)req;
        ga10b_module_t* m = sess_module(s, r->handle);
        if (m == NULL)
            return GA10B_RPC_E_INVAL;
        ga10b_rpc_kinfo_rep_t rep;
        if (ga10b_devd_kernel_info(m, r->kid, &rep) != 0)
            return GA10B_RPC_E_INVAL;
        PF->add(ret, &rep, sizeof(rep));
        return 0;
    }
    case GA10B_RPC_LAUNCH:
        if (req == NULL || sz != (int32_t)sizeof(ga10b_rpc_launch_t))
            return GA10B_RPC_E_INVAL;
        return do_launch(s, (const ga10b_rpc_launch_t*)req, 0, NULL);
    case GA10B_RPC_LAUNCH_BATCH: {
        /* a version 2 client sends the request without `start` */
        ga10b_rpc_batch_t b;
        memset(&b, 0, sizeof(b));
        if (req == NULL || (sz != (int32_t)sizeof(b) &&
                            sz != (int32_t)GA10B_RPC_BATCH_V2_SIZE))
            return GA10B_RPC_E_INVAL;
        memcpy(&b, req, (size_t)sz);
        if (sz == (int32_t)GA10B_RPC_BATCH_V2_SIZE &&
            (b.count > GA10B_RPC_BATCH_MAX_V2 ||
             (b.flags & GA10B_RPC_BATCH_PARTIAL) != 0))
            return GA10B_RPC_E_INVAL;
        ga10b_rpc_batch_rep_t rep;
        int rc = do_launch_batch(s, &b, &rep);
        if (rc == 0)
            PF->add(ret, &rep, sizeof(rep));
        return rc;
    }
    case GA10B_RPC_LAUNCH_COMPUTE:
        if (req == NULL || sz != (int32_t)sizeof(ga10b_rpc_launchc_t))
            return GA10B_RPC_E_INVAL;
        return do_launch_compute(s, (const ga10b_rpc_launchc_t*)req);
    default:
        return GA10B_RPC_E_INVAL;
    }
}

static int gpud_dev_cntl(vdevice_t* dev, int from_pid, int cmd,
                         proto_t* in, proto_t* ret, void* p) {
    int work = cmd == GA10B_RPC_LAUNCH || cmd == GA10B_RPC_LAUNCH_BATCH ||
               cmd == GA10B_RPC_LAUNCH_COMPUTE || cmd == GA10B_RPC_COPY;
    if (work)
        memclk_hold();
    int rc = gpud_request(dev, from_pid, cmd, in, ret, p);
    if (work)
        kernel_tic_nsec(&_memclk.last);
    return rc;
}

static int gpud_loop_step(vdevice_t* dev, void* p) {
    (void)dev;
    (void)p;
    if (_memclk.held) {
        uint64_t now = 0;
        kernel_tic_nsec(&now);
        if (now - _memclk.last > GPUD_MEMCLK_IDLE_NS)
            memclk_drop();
    }
    for (uint32_t i = 0; i < GA10B_RPC_MAX_CLIENTS; i++) {
        gpud_sess_t* s = &_sess[i];
        if (s->used && !ga10b_os_alive(s->pid, s->uuid)) {
            klog("gpud: client pid %u died, reclaiming session\n", s->pid);
            sess_cleanup(s);
        }
    }
    /* vdevice spins loop_step with no idle sleep of its own; requests
     * arrive IPC_NON_BLOCK and interrupt this sleep (the netd pattern) */
    usleep(100000);
    return 0;
}

static char* gpud_cmd(vdevice_t* dev, int from_pid, int argc, char** argv,
                      void* p) {
    (void)dev;
    (void)from_pid;
    (void)p;
    static char out[768];
    if (argc == 2 && strcmp(argv[0], "pipeline") == 0 &&
        (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0)) {
        _pipeline = argv[1][1] == 'n';
        snprintf(out, sizeof(out), "gpud: pipeline=%d\n", _pipeline);
        return out;
    }
    if (argc == 2 && strcmp(argv[0], "memclk") == 0 &&
        (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0)) {
        _memclk.on = argv[1][1] == 'n';
        _memclk.failed = 0;
        if (!_memclk.on)
            memclk_drop();
        snprintf(out, sizeof(out), "gpud: memclk=%d\n", _memclk.on);
        return out;
    }
    if (argc == 2 && strcmp(argv[0], "prof") == 0) {
        if (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "off") == 0) {
            _prof_on = argv[1][1] == 'n';
            _prof_ts_valid = 0;
            ga10b_dev_queue_timestamps(_prof_on);
        } else if (strcmp(argv[1], "reset") == 0)
            prof_reset();
        else if (strcmp(argv[1], "dump") == 0)
            prof_dump();
        snprintf(out, sizeof(out),
                 "gpud: prof=%d kernels %u batches %u launches %u us %u "
                 "ts_bad %u\n", _prof_on, _prof_n, _prof_batch.batches,
                 _prof_batch.launches, (uint32_t)(_prof_batch.ns / 1000u),
                 _prof_ts_bad);
        return out;
    }
    if (argc < 1 || strcmp(argv[0], "stat") != 0) {
        snprintf(out, sizeof(out), "usage: devcmd /dev/gpu stat | "
                 "pipeline on|off | memclk on|off | prof on|off|reset|dump\n");
        return out;
    }
    uint32_t tables = 0, ranges = 0;
    uint64_t va_next = 0;
    ga10b_devd_mm_stat(&tables, &ranges, &va_next);
    int off = snprintf(out, sizeof(out),
                       "gpud: dev_up=%d pipeline=%d prof=%d tables=%u "
                       "free_ranges=%u va_next=%x%08x memclk=%s clients:\n",
                       _dev_up, _pipeline, _prof_on, tables, ranges,
                       (uint32_t)(va_next >> 32), (uint32_t)va_next,
                       !_memclk.on ? "off" : _memclk.failed ? "failed" :
                       _memclk.held ? "held" : "idle");
    for (uint32_t i = 0; i < GA10B_RPC_MAX_CLIENTS && off < (int)sizeof(out); i++) {
        gpud_sess_t* s = &_sess[i];
        if (!s->used)
            continue;
        uint32_t nslots = 0, nallocs = 0, nmods = 0;
        for (uint32_t j = 0; j < GA10B_RPC_SLOTS; j++)
            if (s->slot_size[j] != 0)
                nslots++;
        for (gpud_alloc_t* a = s->allocs; a != NULL; a = a->next)
            nallocs++;
        for (gpud_mod_t* m = s->mods; m != NULL; m = m->next)
            nmods++;
        off += snprintf(out + off, sizeof(out) - off,
                        "  idx %u pid %u slots %u allocs %u mods %u\n",
                        s->idx, s->pid, nslots, nallocs, nmods);
    }
    return out;
}

int main(int argc, char** argv) {
    const char* mnt = (argc > 1) ? argv[1] : GA10B_RPC_DEV;
    vdevice_t dev;
    memset(&dev, 0, sizeof(dev));
    strcpy(dev.desc, "gpu");
    dev.dev_cntl = gpud_dev_cntl;
    dev.loop_step = gpud_loop_step;
    dev.cmd = gpud_cmd;
    klog("gpud: resident GA10B service at %s (bring-up on first open)\n", mnt);
    /* single-task: every request runs to completion before the next one,
     * which IS the M1 GPU serialization (one channel, synchronous launches) */
    if (device_run(&dev, mnt, FS_TYPE_CHAR, 0666, false) != 0)
        return -1;
    return 0;
}