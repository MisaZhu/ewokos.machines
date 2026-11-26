/*
 * ga10b_rpc.h - wire protocol between libga10b's remote client implementation
 * (ga10b_dev_remote.c) and the resident GPU daemon (drivers/gpud).
 *
 * Transport: dev_cntl_by_pid() on /dev/gpu, one fixed-size request struct per
 * command (all < 512B inline proto).  Variable payloads (packed kernel params,
 * launch_compute blobs) ride a per-client contiguous shm established at OPEN;
 * cubin images ride a one-shot shm passed by id in MODULE_LOAD.
 *
 * The daemon serializes every request (single-task vdevice): one GPU owner,
 * one VM, one compute channel.  Each client owns a 4GB GPU VA window of 64
 * fixed 64MB slots (the ga10b_dev_map LRU contract) plus bump-allocated
 * persistent ranges below GA10B_RPC_PERSIST_CAP.
 */
#ifndef GA10B_RPC_H
#define GA10B_RPC_H

#include <stdint.h>

#define GA10B_RPC_DEV          "/dev/gpu"
#define GA10B_RPC_VERSION      3u  /* 3: batch start offset, partial batches,
                                      256-launch cap (PING tells the client) */

/* dev_cntl command ids (driver-private low space) */
#define GA10B_RPC_PING           1
#define GA10B_RPC_OPEN           2
#define GA10B_RPC_CLOSE          3
#define GA10B_RPC_MAP            4   /* fixed-VA slot (re)map */
#define GA10B_RPC_ALLOC          5   /* persistent map of client dma */
#define GA10B_RPC_FREE           6
#define GA10B_RPC_COPY           7
#define GA10B_RPC_SYNC           8
#define GA10B_RPC_MODULE_LOAD    9
#define GA10B_RPC_KERNEL_FIND   10
#define GA10B_RPC_KERNEL_INFO   11
#define GA10B_RPC_LAUNCH        12
#define GA10B_RPC_LAUNCH_COMPUTE 13
#define GA10B_RPC_LAUNCH_BATCH  14   /* queued launches in the payload shm */

/* handler status codes (negative dev_cntl return passes through to client) */
#define GA10B_RPC_E_FAILED      (-1)
#define GA10B_RPC_E_NOSESSION   (-2)
#define GA10B_RPC_E_INVAL       (-3)
#define GA10B_RPC_E_NOMEM       (-4)
#define GA10B_RPC_E_NODEV       (-5)

#define GA10B_RPC_MAX_CLIENTS   16u
#define GA10B_RPC_SLOTS         64u
#define GA10B_RPC_SLOT_SPAN     0x04000000ULL   /* 64MB */
#define GA10B_RPC_WINDOW_BASE   0x400000000ULL  /* 16GB: above persistent VA */
#define GA10B_RPC_WINDOW_SPAN   (GA10B_RPC_SLOTS * GA10B_RPC_SLOT_SPAN)
/* persistent bump mappings must stay below the first client window */
#define GA10B_RPC_PERSIST_CAP   GA10B_RPC_WINDOW_BASE
#define GA10B_RPC_REQ_SHM_SIZE  (1u << 20)      /* per-client payload shm */
#define GA10B_RPC_DMA_BLOCK     1               /* GPU high pool (shared) */
#define GA10B_RPC_NAME_MAX      128             /* KUBIN_NAME_MAX */
#define GA10B_RPC_MAX_PARAMS    32              /* KUBIN_MAX_PARAMS */

typedef struct {
    uint32_t uuid;                  /* client proc uuid (pid-reuse guard) */
    int32_t  shm_id;                /* client-created contig payload shm */
    uint32_t shm_size;
    char     fw_dir[128];           /* "" = driver default */
} ga10b_rpc_open_t;

typedef struct {
    uint64_t slot_base;             /* this client's map window base */
    uint32_t client_idx;
} ga10b_rpc_open_rep_t;

typedef struct {
    uint64_t va;                    /* inside the client's window */
    uint64_t phy;
    uint32_t size;
    uint32_t old_size;              /* previous mapping at va (0 = fresh) */
} ga10b_rpc_map_t;

typedef struct {
    uint64_t phy;                   /* client-owned dma buffer */
    uint32_t size;
} ga10b_rpc_alloc_t;

typedef struct {
    uint64_t gpu_va;
} ga10b_rpc_alloc_rep_t;

typedef struct {
    uint64_t gpu_va;
    uint32_t size;
} ga10b_rpc_free_t;

typedef struct {
    uint64_t dst_va;
    uint64_t src_va;
    uint32_t bytes;
} ga10b_rpc_copy_t;

#define GA10B_RPC_SYNC_TO_GPU   0u  /* L2 invalidate */
#define GA10B_RPC_SYNC_TO_CPU   1u  /* L2 flush */
typedef struct {
    uint32_t dir;
} ga10b_rpc_sync_t;

typedef struct {
    int32_t  shm_id;                /* one-shot contig shm holding the cubin */
    uint32_t size;
} ga10b_rpc_module_t;

typedef struct {
    int32_t handle;
} ga10b_rpc_module_rep_t;

typedef struct {
    int32_t handle;
    char    name[GA10B_RPC_NAME_MAX];
} ga10b_rpc_kfind_t;

typedef struct {
    int32_t kid;
} ga10b_rpc_kfind_rep_t;

typedef struct {
    int32_t handle;
    int32_t kid;
} ga10b_rpc_kinfo_t;

typedef struct {
    uint32_t nparams;
    uint32_t param_total;
    uint16_t off[GA10B_RPC_MAX_PARAMS];   /* cbank0 param-window offsets */
    uint16_t sz[GA10B_RPC_MAX_PARAMS];
} ga10b_rpc_kinfo_rep_t;

typedef struct {
    int32_t  handle;
    int32_t  kid;
    uint32_t grid[3];
    uint32_t block[3];
    uint32_t dyn_smem;
    uint32_t blob_off;              /* packed params in the client payload shm */
    uint32_t blob_len;
} ga10b_rpc_launch_t;

/* Batched launches: the payload shm holds `count` records back to back from
 * offset 0, each a ga10b_rpc_launch_t immediately followed by its param blob
 * (blob_off = record offset + sizeof record), the next record 8-aligned.
 * The daemon runs them in order and stops at the first failure.
 *
 * What bounds how long one client holds the single-task daemon is time, not
 * count: with GA10B_RPC_BATCH_PARTIAL the daemon returns once
 * GA10B_RPC_BATCH_BUDGET_US has passed, reporting how many launches retired
 * and where the next record starts, and the client sends the rest again from
 * there (`start`).  A decode step of a small model fits one round trip; a
 * long batch still lets other clients in every budget.
 *
 * Version 2 peers: 12-byte request (no `start`), 8-byte reply, 32 launches. */
#define GA10B_RPC_BATCH_MAX     256u
#define GA10B_RPC_BATCH_MAX_V2  32u
#define GA10B_RPC_BATCH_BUDGET_US 20000u
#define GA10B_RPC_BATCH_SYNC_TO_CPU 1u  /* L2 flush after the last launch */
#define GA10B_RPC_BATCH_PARTIAL     2u  /* client resumes a partial batch */
typedef struct {
    uint32_t count;
    uint32_t bytes;                 /* payload shm bytes the records span */
    uint32_t flags;
    uint32_t start;                 /* payload offset of the first record */
} ga10b_rpc_batch_t;
#define GA10B_RPC_BATCH_V2_SIZE 12u

typedef struct {
    uint32_t done;                  /* launches that retired */
    int32_t  status;                /* 0 or the GA10B_RPC_E_* of record `done` */
    uint32_t next;                  /* payload offset of record `done` */
} ga10b_rpc_batch_rep_t;
#define GA10B_RPC_BATCH_REP_V2_SIZE 8u

/* payload shm layout at off: unif dwords, then ssbo VAs, then texdesc dwords */
typedef struct {
    int32_t  handle;
    int32_t  kid;
    uint32_t grid[3];
    uint32_t block[3];
    uint32_t unif_dwords;
    uint32_t nssbo;
    uint32_t texdesc_dwords;
    uint32_t off;
} ga10b_rpc_launchc_t;

#endif