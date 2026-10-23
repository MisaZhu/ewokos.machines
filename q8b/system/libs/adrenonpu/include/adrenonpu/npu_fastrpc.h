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
 *         (nbufs&0xff) | (nattrs&0xff)<<8 | (nrets&0xff)<<16 | (attrs<<24)
 *     (FASTRPC_SCALAR macros, remote.h of the Hexagon SDK).
 *
 * EwokOS kernel transport ABI (to be implemented kernel-side; mirrors the
 * restricted GPU PAS ABI of machines/q8b/include/q8b_gpu_pas.h).  Until the
 * kernel grows a FastRPC service, frpc_call() receives the
 * Q8B_NPU_PAS_UNSUPPORTED verdict and the library declines the HTP path,
 * exactly like the Adreno zap flow declines on a kernel without SCM:
 *
 *   syscall3(SYS_PLATFORM_CTL, op, arg, size), op in [0x51385100, +0x100):
 *     QUERY          arg=0, size=0            -> Q8B_NPU_PAS_ABI if present
 *     SESSION_OPEN   arg=domain(3=cdsp), 0    -> session id (>=0)
 *     SESSION_CLOSE  arg=session, 0
 *     INVOKE         arg=payload PA, size     -> payload updated in place:
 *                        payload[0] = { session, retval } after the call
 *     MMAP           arg=(session<<32)|flags, size; buffer PA pre-staged
 *                    -> device-visible address (0 on failure)
 *     MUNMAP         arg=device addr, size
 *     PWR_VOTE       arg=(session<<32)|level, 0   (HAP_power DCVS v3 vote)
 *
 * The invoke payload built by frpc_invoke() follows the kernel driver's
 * expected layout (fastrpc.c parses it identically):
 *   [0]  uint64 { session, retval }   header, retval written by the kernel
 *   [8]  uint32 handle                remote object handle
 *   [12] uint32 sc                     scalar descriptor
 *   [16] int32 fds[nfds]               fd list (mmap'd buffers)
 *   [...] uint64 args[nargs]           payload-relative data offsets
 *   [...] buffer data                  each remote buffer, 8-byte aligned
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
#define Q8B_NPU_FRPC_SESSION  (Q8B_NPU_ABI_BASE + 0x10u)
#define Q8B_NPU_FRPC_CLOSE    (Q8B_NPU_ABI_BASE + 0x11u)
#define Q8B_NPU_FRPC_INVOKE   (Q8B_NPU_ABI_BASE + 0x12u)
#define Q8B_NPU_FRPC_MMAP     (Q8B_NPU_ABI_BASE + 0x13u)
#define Q8B_NPU_FRPC_MUNMAP   (Q8B_NPU_ABI_BASE + 0x14u)
#define Q8B_NPU_FRPC_PWR      (Q8B_NPU_ABI_BASE + 0x15u)

/* Verdicts, matching the Q8B_GPU_PAS_* code style. */
#define Q8B_NPU_PAS_ABI         0x51385101u
#define Q8B_NPU_PAS_BUSY        (-1101)
#define Q8B_NPU_PAS_UNSUPPORTED (-1102)
#define Q8B_NPU_PAS_INVALID     (-1103)
#define Q8B_NPU_PAS_FAILED      (-1104)

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

/* Remote buffer descriptor for frpc_invoke: either a locally allocated
   payload copy (pa == 0, data copied in before the call and back after for
   out buffers) or a registered dma buffer (pa != 0, sent by fd/phys, no
   copy). */
typedef struct {
	uint64_t pa;
	void    *data;
	uint32_t size;
	uint32_t fd;
	bool     is_out;
} frpc_rbuf_t;

#define FRPC_MAX_FDS   16
#define FRPC_MAX_RBUFS 32

int  frpc_call(uint32_t op, uint64_t arg, uint32_t size);
bool frpc_available(void);          /* kernel exposes the NPU FastRPC ABI */
int  frpc_session_open(uint32_t domain, uint32_t *session);
void frpc_session_close(uint32_t session);
uint32_t frpc_session_get(void);    /* active cdsp session, 0 when down */
int  frpc_power_vote(uint32_t session, uint32_t level);

/* remote_handle64_open: resolve a skel URI ("file://libqnn_htp_v68_skel.so")
   into a remote object handle.  Returns 0 on success. */
int frpc_open(uint32_t session, const char *uri, uint32_t *handle);
int frpc_close(uint32_t session, uint32_t handle);

/* remote_handle64_invoke with the Linux payload layout (file header).
   retval receives the remote function's own return code. */
int frpc_invoke(uint32_t session, uint32_t handle, uint32_t sc,
                const frpc_rbuf_t *rbufs, uint32_t nrbufs,
                const int32_t *fds, uint32_t nfds, int32_t *retval);

/* remote_alloc/mmap equivalent: stage a dma buffer for cDSP access and
   return its device-visible address (0 on failure). */
uint64_t frpc_mmap(uint32_t session, uint64_t pa, uint32_t size);
int      frpc_munmap(uint32_t session, uint64_t devaddr, uint32_t size);

/* Scalar descriptor packing (FASTRPC_SCALAR). */
static inline uint32_t frpc_scalar(uint32_t attrs, uint32_t nrets,
                                   uint32_t nattrs, uint32_t nbufs) {
	return (nbufs & 0xffu) | ((nattrs & 0xffu) << 8) |
	       ((nrets & 0xffu) << 16) | ((attrs & 0xffu) << 24);
}

#ifdef __cplusplus
}
#endif

#endif /* NPU_FASTRPC_H */
