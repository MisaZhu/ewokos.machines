/*
 * npu_fastrpc.h - FastRPC client transport for the SC8280XP cDSP (Q8B).
 *
 * Reconstruction sources (the vendor libcdsprpc.so is closed; this is the
 * protocol as implemented by the open Linux side):
 *   - drivers/misc/fastrpc.c (v6.6): the FASTRPC_IOCTL_* request structs
 *     below are copied field-for-field from include/uapi/linux/fastrpc.h;
 *     the invoke payload layout follows fastrpc_create_invoke_buf().
 *   - libcdsprpc semantics: remote_handle64_open(uri, &handle) /
 *     remote_handle64_invoke(handle, sc, fds, fdslen, args, argslen,
 *     &ret) / remote_handle64_close(handle), with the scalar descriptor
 *     word packed as
 *         (method&0x1f)<<24 | (inbufs&0xff)<<16 | (outbufs&0xff)<<8 |
 *         (inhandles&0x0f)<<4 | (outhandles&0x0f)
 *     (the REMOTE_SCALARS layout used by Linux fastrpc.c).
 *
 * EwokOS kernel transport ABI mirrors the restricted GPU PAS ABI.  PAS and
 * the synchronous GLINK/FastRPC service are separate probes so a caller can
 * distinguish an authenticated DSP from one that still lacks an apps_smmu
 * mapping:
 *
 *   syscall3(SYS_PLATFORM_CTL, op, arg, size), op in [0x51385100, +0x100):
 *     PAS_QUERY      arg=0, size=0            -> Q8B_NPU_PAS_ABI if present
 *     PAS_STATUS     arg=0, size=0            -> SMP2P/PAS state bits
 *     FRPC_QUERY     arg=0, size=0            -> Q8B_NPU_FRPC_ABI if present
 *     METADATA_QUERY arg=0, size=0            -> 4096 if page format supported
 *     POINTER_QUERY  arg=0, size=0            -> 1 if AP pointer format supported
 *     SESSION_OPEN   arg=domain(3=cdsp), 0    -> session id (>=0)
 *     SESSION_CLOSE  arg=session, 0
 *     INVOKE         arg=payload PA, size     -> payload updated in place:
 *                        payload[0] = { session, retval } after the call
 *     MMAP           arg=(session<<32)|PFN, size; buffer PA pre-staged
 *                    -> consolidated device-visible address (SID<<32|IOVA)
 *     MUNMAP         arg=consolidated device addr, size
 *     PWR_VOTE       arg=(session<<32)|level, 0   (HAP_power DCVS v3 vote)
 *
 * The invoke payload built by frpc_invoke() has a private 16-byte prefix
 * (session, retval, handle, scalar descriptor), padded to 4096 bytes when
 * METADATA_QUERY confirms support. Bit31 of the private session word selects
 * that format; kernels still accept the legacy unpadded prefix. The same remote
 * buffer/invoke/page metadata and inline data that Linux fastrpc.c sends at
 * the address in struct fastrpc_msg.  Inline data begins at the 128-byte
 * FASTRPC_ALIGN boundary relative to that metadata address, just as in the
 * upstream driver; the private prefix is not part of the DSP view.  The
 * kernel converts the prefix into that wire message and writes the response
 * retval back into the prefix. POINTER_QUERY independently negotiates bit30:
 * pv then carries AP DMA virtual pointers for staged/data-backed arguments,
 * while page descriptors always carry SID|IOVA. Explicit DSP addresses and
 * PA-only arguments retain their remote/IOVA pointers with paired backing.
 */
#ifndef NPU_FASTRPC_H
#define NPU_FASTRPC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- kernel platform_control ABI (see file header) --- */
#define Q8B_NPU_ABI_BASE      0x51385100u
#define Q8B_NPU_PAS_QUERY     (Q8B_NPU_ABI_BASE + 0x00u)
#define Q8B_NPU_PAS_INIT      (Q8B_NPU_ABI_BASE + 0x01u)
#define Q8B_NPU_PAS_MEM       (Q8B_NPU_ABI_BASE + 0x02u)
#define Q8B_NPU_PAS_STAGE     (Q8B_NPU_ABI_BASE + 0x03u)
#define Q8B_NPU_PAS_AUTH      (Q8B_NPU_ABI_BASE + 0x04u)
#define Q8B_NPU_PAS_BOOT      (Q8B_NPU_ABI_BASE + 0x05u)
#define Q8B_NPU_PAS_STOP      (Q8B_NPU_ABI_BASE + 0x06u)
#define Q8B_NPU_PAS_STATUS    (Q8B_NPU_ABI_BASE + 0x07u)
#define Q8B_NPU_FRPC_QUERY    (Q8B_NPU_ABI_BASE + 0x0fu)
#define Q8B_NPU_FRPC_SESSION  (Q8B_NPU_ABI_BASE + 0x10u)
#define Q8B_NPU_FRPC_CLOSE    (Q8B_NPU_ABI_BASE + 0x11u)
#define Q8B_NPU_FRPC_INVOKE   (Q8B_NPU_ABI_BASE + 0x12u)
#define Q8B_NPU_FRPC_MMAP     (Q8B_NPU_ABI_BASE + 0x13u)
#define Q8B_NPU_FRPC_MUNMAP   (Q8B_NPU_ABI_BASE + 0x14u)
#define Q8B_NPU_FRPC_PWR      (Q8B_NPU_ABI_BASE + 0x15u)

/* Nonblocking calls retain owned DMA until POLL consumes the response or
 * PAS_STOP has successfully quiesced the DSP.  Required for reverse RPC. */
#define Q8B_NPU_FRPC_SUBMIT (Q8B_NPU_ABI_BASE + 0x18u) /* payload PA, size */
#define Q8B_NPU_FRPC_POLL   (Q8B_NPU_ABI_BASE + 0x19u) /* same payload PA, size */
#define Q8B_NPU_FRPC_SUBMIT_LISTENER (Q8B_NPU_ABI_BASE + 0x1au)
#define Q8B_NPU_FRPC_METADATA_QUERY (Q8B_NPU_ABI_BASE + 0x1bu)
#define Q8B_NPU_FRPC_METADATA_PAGE 4096u
#define Q8B_NPU_FRPC_PACKET_PAGE (1u << 31)
#define Q8B_NPU_FRPC_POINTER_QUERY (Q8B_NPU_ABI_BASE + 0x1cu)
/* Root-only, read-only allocation snapshot: arg=exact PA, size=0;
 * return (pins<<32)|owner, negative if no pinned allocation. */
#define Q8B_NPU_FRPC_DMA_QUERY (Q8B_NPU_ABI_BASE + 0x1du)
#define Q8B_NPU_FRPC_POINTER_HOST 1u
#define Q8B_NPU_FRPC_PACKET_HOST (1u << 30)

/* Verdicts, matching the Q8B_GPU_PAS_* code style. */
#define Q8B_NPU_PAS_ABI         0x51385101u
#define Q8B_NPU_PAS_BUSY        (-1101)
#define Q8B_NPU_PAS_UNSUPPORTED (-1102)
#define Q8B_NPU_PAS_INVALID     (-1103)
#define Q8B_NPU_PAS_FAILED      (-1104)
#define Q8B_NPU_FRPC_ABI        (Q8B_NPU_ABI_BASE + 0x03u)

/* FastRPC domains (fastrpc.c enum fastrpc_proc_attr / misc device minors). */
#define FRPC_DOMAIN_ADSP  0
#define FRPC_DOMAIN_MDSP  1
#define FRPC_DOMAIN_SLPI  2
#define FRPC_DOMAIN_CDSP  3

/* --- upstream uapi/linux/fastrpc.h request structs (verbatim layout) --- */
struct fastrpc_invoke {
	uint32_t handle;
	uint32_t sc;
	uint64_t invokebuf;
};

struct fastrpc_invoke_fd {
	int32_t fd;
	uint32_t flags;
};

struct fastrpc_req_caps {
	uint32_t domain;
	uint32_t attribute_id;
	uint32_t capability;
};

struct fastrpc_mmap {
	int32_t  fd;
	uint32_t flags;
	uint64_t vaddrout;
	uint64_t size;
};

struct fastrpc_munmap {
	uint64_t vaddr;
	uint64_t size;
};

struct fastrpc_init_attrs {
	int32_t  filelen;
	uint32_t file;
	int32_t  memlen;
	uint64_t siglen;
};

/* Attribute words inside the scalar descriptor (remote.h FASTRPC_*). */
#define FRPC_ATTR_BYVALUE      0u
#define FRPC_ATTR_NOPAYLOAD    4u
#define FRPC_ATTR_NOSIGNAL     5u
#define FRPC_ATTR_PTRHANDLE    8u
#define FRPC_ATTR_RESERVED     9u

/* FastRPC framework root handle and init method ids from Linux
 * drivers/misc/fastrpc.c.  INIT_ATTACH selects a root/static PD; a dynamic
 * user PD uses INIT_CREATE directly before opening user handles. */
#define FRPC_INIT_HANDLE       1u
#define FRPC_CLIENT_ID         1
#define FRPC_RMID_INIT_ATTACH  0u
#define FRPC_RMID_INIT_RELEASE 1u
#define FRPC_RMID_INIT_MMAP    4u
#define FRPC_RMID_INIT_MUNMAP  5u
#define FRPC_RMID_INIT_CREATE  6u
#define FRPC_RMID_INIT_CREATE_ATTR 7u
#define FRPC_MODE_UNSIGNED_MODULE 8u
#define FRPC_RMID_INIT_MEM_MAP 10u
#define FRPC_RMID_INIT_MEM_UNMAP 11u
#define FRPC_MMAP_REMOTE_HEAP_ADDR 8u
#define FRPC_MMAP_ADD_PAGES         0x1000u

/* Root-object request payloads used by the upstream FastRPC service.  Q8B
 * keeps the private FRPC_MMAP operation for AP SMMU ownership and layers the
 * standard INIT_MMAP/INIT_MUNMAP calls on top of these exact layouts. */
struct frpc_mmap_req_msg {
	int32_t  client_id;
	uint32_t flags;
	uint64_t vaddr;
	int32_t  num;
};

struct frpc_mmap_rsp_msg {
	uint64_t vaddr;
};

struct frpc_munmap_req_msg {
	int32_t  client_id;
	uint64_t vaddr;
	uint64_t size;
};

struct frpc_mem_map_req_msg {
	int32_t  client_id;
	int32_t  fd;
	int32_t  offset;
	uint32_t flags;
	uint64_t vaddrin;
	int32_t  num;
	int32_t  data_len;
};

struct frpc_mem_unmap_req_msg {
	int32_t  client_id;
	int32_t  fd;
	uint64_t vaddrin;
	uint64_t len;
};

/* The first argument of INIT_CREATE is the private process descriptor built
 * by drivers/misc/fastrpc.c.  Keep its native 32/32/32/32/32 layout; the
 * actual file pointer is omitted from the compact client by setting filelen
 * to zero, while the fourth argument still carries the initial heap page. */
struct frpc_init_create_msg {
	int32_t  client_id;
	uint32_t namelen;
	uint32_t filelen;
	uint32_t pageslen;
	uint32_t attrs;
	uint32_t siglen;
};

/* Remote buffer descriptor for frpc_invoke: either a locally allocated
   payload copy (pa == 0, data copied in before the call and back after for
   out buffers) or a registered DMA buffer (pa != 0).  For DMA buffers pa is
   the page-aligned mapping base and offset selects the byte range used by
   this argument. data, when provided with pa, is the allocation base.
   The negotiated host-pointer format carries data+offset; PA-only buffers
   use IOVA+offset. Page descriptors always select the matching DMA pages. */
typedef struct {
	uint64_t pa;
	/* Optional DSP-side address returned by INIT_MMAP/INIT_MEM_MAP.  The
	 * compact bring-up path leaves this zero; pa always remains physical. */
	uint64_t dspaddr;
	void    *data;
	uint32_t size;
	uint32_t offset;
	int32_t  fd;
	bool     is_out;
} frpc_rbuf_t;

/* One buffer can have two distinct addresses: ap_devaddr is the
 * SID|IOVA address consumed by the AP SMMU, while dsp_vaddr is the virtual
 * address allocated by the DSP root INIT_MMAP method. */
typedef struct {
	uint64_t ap_devaddr;
	uint64_t dsp_vaddr;
	uint32_t size;
	uint32_t ap_size;
	unsigned epoch;
	/* The root operation may have executed without a usable reply. Only
	 * successful secure stop can retire a mapping in this state. */
	int uncertain;
} frpc_remote_map_t;

#define FRPC_MAX_FDS   16
#define FRPC_MAX_RBUFS 32

int  frpc_call(uint32_t op, uint64_t arg, uint32_t size);
bool frpc_available(void);          /* kernel exposes the NPU FastRPC ABI */
int  frpc_session_open(uint32_t domain, uint32_t *session);
/* attrs: 0 (default) or FRPC_MODE_UNSIGNED_MODULE. */
int  frpc_session_open_attrs(uint32_t domain, uint32_t attrs, uint32_t *session);
/* Optional board-matched FastRPC shell ELF, at most 2 MiB. */
int  frpc_session_open_image(uint32_t domain, uint32_t attrs,
        const void *image, uint32_t image_size, uint32_t *session);
/* Release USER_PD while the reverse-RPC listener can still answer callbacks.
 * Does not free the initial heap or stop the transport. */
int  frpc_session_release(uint32_t session);
/* Returns 0 only when closed or already retired. On failed cleanup, retains
 * outstanding ownership and blocks new opens. Keep the session ID to retry. */
int  frpc_session_close(uint32_t session);
uint32_t frpc_session_get(void);    /* active cdsp session, 0 when down */
int  frpc_power_vote(uint32_t session, uint32_t level);

/* remote_handle64_open: resolve a skel URI (normally
   "file://libQnnHtpV68Skel.so") into a remote object handle.  Returns 0 on
   success.  The current graph marshaller still uses the small bring-up ABI
   in npu_htp.c; an official QAIRT skel can therefore open successfully but
   will decline those private method ids until the SDK's generated stubs are
   installed. */
int frpc_open(uint32_t session, const char *uri, uint32_t *handle);
int frpc_close(uint32_t session, uint32_t handle);

/* remote_handle64_invoke with the Linux payload layout (file header).
 * retval receives the remote result only after successful transport cleanup.
 * On failed secure stop, caller-owned DMA must remain live until successful
 * frpc_transport_abort(); internal packet retention does not pin caller DMA
 * against an explicit free or process exit. */
int frpc_invoke(uint32_t session, uint32_t handle, uint32_t sc,
                const frpc_rbuf_t *rbufs, uint32_t nrbufs,
                const int32_t *fds, uint32_t nfds, int32_t *retval);

/* Nonblocking transport for the single-threaded reverse-RPC dispatcher.
 * A non-NULL request retains its DMA and caller buffers on BUSY or error,
 * including a consumed reply whose unmap/secure stop failed. Keep buffers
 * live until poll clears the pointer. Unmap failures return an error even
 * after safe reclamation; they never copy staged output or overwrite retval.
 * abort stops the DSP; then poll consumes requests without copying output.
 * Synchronous/internal deferred packets are reclaimed by successful abort.
 * Failed stop blocks new requests/opens until transport recovery. */
typedef struct frpc_request frpc_request_t;
int frpc_request_start(uint32_t session, uint32_t handle, uint32_t sc,
                const frpc_rbuf_t *rbufs, uint32_t nrbufs,
                const int32_t *fds, uint32_t nfds, frpc_request_t **request);
int frpc_request_poll(frpc_request_t **request, int32_t *retval);
int frpc_transport_abort(void);
void frpc_set_progress(void (*progress)(void));

/* remote_alloc/mmap equivalent: stage a dma buffer for cDSP access and
   return its device-visible address (0 on failure).  The kernel owns a
   reference for every successful call, including calls that reuse an
   existing PA->IOVA entry; pair each call with one frpc_munmap(). */
uint64_t frpc_mmap(uint32_t session, uint64_t pa, uint32_t size);
int      frpc_munmap(uint32_t session, uint64_t devaddr, uint32_t size);

/* Capture with a successful AP mmap. A different epoch proves its SMMU
 * reference was retired by successful CLOSE/secure stop; do not send a
 * stale raw IOVA to MUNMAP after a later session may have reused it.
 * Failed cleanup never advances this process-local transport epoch. */
unsigned frpc_mapping_epoch(void);

/* Standard FastRPC root mapping.  These helpers intentionally sit on top of
 * the private AP SMMU mapping: INIT_MMAP receives the consolidated page
 * address, returns the DSP virtual address, and the private reference stays
 * live until INIT_MUNMAP has completed. Use a fresh record for each map.
 * Even a failed map may return a nonempty record when secure stop failed:
 * keep its backing DMA alive and retry unmap or frpc_transport_abort().
 * An empty record means its references are retired, including after stop.
 * Unmap records remember the DSP acknowledgment so an AP cleanup retry
 * never repeats a successful root unmap. Records survive abort/session
 * close and can be retired without touching addresses from a newer epoch.
 * These userspace records do not pin DMA against free/process exit. */
int frpc_remote_mmap(uint32_t session, uint64_t pa, uint32_t size,
			     frpc_remote_map_t *mapping);
int frpc_remote_mmap_flags(uint32_t session, uint64_t pa, uint32_t size,
                uint32_t flags, frpc_remote_map_t *mapping);
int frpc_remote_munmap(uint32_t session, frpc_remote_map_t *mapping);

/* dma-buf form used once an fd importer supplies the physical backing.  pa
 * is still passed explicitly so the kernel can validate the AP ownership
 * before the DSP root sees the fd/offset metadata. */
int frpc_remote_mem_map(uint32_t session, int32_t fd, uint64_t pa,
			       uint32_t offset, uint32_t size, uint32_t flags,
			       frpc_remote_map_t *mapping);
int frpc_remote_mem_unmap(uint32_t session, int32_t fd,
				frpc_remote_map_t *mapping);

/* Scalar descriptor packing (REMOTE_SCALARS / FASTRPC_SCALARS).  This
 * compact client only emits buffer arguments; handle counts remain zero. */
static inline uint32_t frpc_scalar(uint32_t method, uint32_t inbufs,
                                   uint32_t outbufs) {
	return ((method & 0x1fu) << 24) | ((inbufs & 0xffu) << 16) |
	       ((outbufs & 0xffu) << 8);
}

#ifdef __cplusplus
}
#endif

#endif /* NPU_FASTRPC_H */
