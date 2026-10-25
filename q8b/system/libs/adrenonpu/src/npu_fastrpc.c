/*
 * npu_fastrpc.c - FastRPC client transport for the SC8280XP cDSP.
 *
 * This is the client half of the reconstruction described in npu_fastrpc.h:
 * the invoke payload follows drivers/misc/fastrpc.c (private Q8B prefix,
 * remote arguments, invoke/page lists, fd/crc lists and 128-byte-aligned
 * inline data), and the scalar descriptor is packed
 * with the FASTRPC_SCALAR macro semantics.  Everything rides the restricted
 * Q8B kernel ABI on syscall3(SYS_PLATFORM_CTL, op, arg, size), the same door
 * libadreno uses for the GPU PAS.
 *
 * The kernel exposes a serialized GLINK request/response service.  It keeps
 * the userspace side deliberately small: one synchronous invoke at a time,
 * Buffer mappings are accepted only after the kernel has initialized and
 * read back the apps_smmu context bank.
 */
#include <adrenonpu/qnn_core_internal.h>
#include <adrenonpu/npu_cdsp.h>
#include <ewoksys/syscall.h>
#include <ewoksys/proc.h>
#include <syscalls.h>
#include <stdlib.h>
#include <unistd.h>
#include <ewoksys/kernel_tic.h>
#include <string.h>

#define FRPC_RETRY_MAX 3u
#define FRPC_RETRY_NS  10000000ull   /* keep a failed GLINK probe bounded */
#define FRPC_MAX_CRCS  64u          /* FASTRPC_MAX_CRCLIST in Linux */
#define FRPC_ALIGN     128u         /* FASTRPC_ALIGN in drivers/misc/fastrpc.c */
#define FRPC_INIT_MEM_SIZE (2u * 1024u * 1024u) /* mainline INIT_FILELEN_MAX */

/* -1 unknown, 0 absent, 1 present. */
static int g_frpc_avail = -1;
static uint32_t g_meta_offset = 16u;
static int g_host_pointers;
static uint32_t g_session;
static npu_buf_t g_init_mem;
static uint64_t g_init_devaddr;
/* Retained even when CREATE or PAS_STOP fails. A subsequent open must not
 * overwrite the only ownership record for a heap still visible to DSP. */
static uint32_t g_init_session;
static int g_init_released;

/* Fine-counter sleep may be unavailable on a board.  Its -1 return is a
 * request to use the scheduler sleep, not evidence that time elapsed. */
static void frpc_pause(uint64_t ns) {
	if (proc_nsleep_precise(ns) != 0)
		usleep((unsigned)((ns + 999u) / 1000u));
}

static unsigned g_transport_epoch;
unsigned frpc_mapping_epoch(void) { return g_transport_epoch; }
static int g_transport_stopped;
static int g_transport_unsafe;
static void (*g_progress)(void);
/* Virtual listener thread while the single AP thread services callbacks.
 * Root mmap/munmap calls from here must not queue behind the application
 * request that is waiting for this very callback. */
static int g_in_progress;

struct frpc_request {
	npu_buf_t pay;
	uint64_t payload_iova, pointer_base;
	uint64_t mapped_iova[FRPC_MAX_RBUFS];
	uint32_t mapped_size[FRPC_MAX_RBUFS];
	frpc_rbuf_t rbufs[FRPC_MAX_RBUFS];
	uint32_t session, nrbufs, inbufs, meta_offset;
	unsigned epoch;
	int completed, cleanup_error;
	int32_t retval;
	struct frpc_request *deferred_next;
};

/* Requests with no public owner (failed construction or synchronous invoke)
 * stay reachable when secure stop fails. Only a successful stop drains them. */
static frpc_request_t *g_deferred_requests;

void frpc_set_progress(void (*progress)(void)) { g_progress = progress; }

int frpc_transport_abort(void)
{
	int rc = frpc_call(Q8B_NPU_PAS_STOP, 0, 0);
	if (rc == 0) {
		g_transport_epoch++;
		g_transport_stopped = 1;
		g_transport_unsafe = 0;
		g_session = 0;
		while (g_deferred_requests) {
			frpc_request_t *req = g_deferred_requests;
			g_deferred_requests = req->deferred_next;
			npu_buf_free(&req->pay);
			free(req);
		}
	} else {
		g_transport_unsafe = 1;
	}
	return rc;
}

struct frpc_phy_page {
	uint64_t addr;
	uint64_t size;
};

static int frpc_init_create(uint32_t session, uint32_t attrs,
        const void *image, uint32_t image_size)
{
	struct frpc_init_create_msg in;
	struct frpc_phy_page page;
	char name[] = "ewokos-qnn";
	frpc_rbuf_t rbufs[4];
	uint32_t sc;
	int32_t rv = 0;
	int rc;

	/* Linux allocates a private initial heap before INIT_CREATE.  Keep the
	 * same ordering so the DSP can install its user-PD page mapping before
	 * remote_handle64_open() resolves a skel. */
	memset(&g_init_mem, 0, sizeof(g_init_mem));
	uint32_t heap_size = image_size * 4u;
	if (heap_size < FRPC_INIT_MEM_SIZE) heap_size = FRPC_INIT_MEM_SIZE;
	heap_size = (heap_size + 0xfffffu) & ~0xfffffu;
	if (npu_buf_alloc(&g_init_mem, heap_size) != 0)
		return Q8B_NPU_PAS_FAILED;
	g_init_devaddr = frpc_mmap(session, g_init_mem.pa, g_init_mem.size);
	if (!g_init_devaddr) {
		npu_buf_free(&g_init_mem);
		memset(&g_init_mem, 0, sizeof(g_init_mem));
		return Q8B_NPU_PAS_UNSUPPORTED;
	}

	memset(&in, 0, sizeof(in));
	in.client_id = FRPC_CLIENT_ID;
	in.namelen = (uint32_t)sizeof(name);
	in.filelen = image_size;
	in.pageslen = 1;
	in.attrs = attrs;
	memset(&page, 0, sizeof(page));
	page.addr = g_init_devaddr;
	page.size = g_init_mem.size;
	memset(rbufs, 0, sizeof(rbufs));
	rbufs[0].data = &in;
	rbufs[0].size = sizeof(in);
	rbufs[0].fd = -1;
	rbufs[1].data = name;
	rbufs[1].size = sizeof(name);
	rbufs[1].fd = -1;
	rbufs[2].data = (void *)image;
	rbufs[2].size = image_size;
	rbufs[2].fd = -1;
	rbufs[3].data = &page;
	rbufs[3].size = sizeof(page);
	rbufs[3].fd = -1;
	sc = frpc_scalar(attrs ? FRPC_RMID_INIT_CREATE_ATTR :
	                 FRPC_RMID_INIT_CREATE, 4u, 0u);
	rc = frpc_invoke(session, FRPC_INIT_HANDLE, sc, rbufs, 4,
	                 NULL, 0, &rv);
	if (rc != 0 || rv != 0) {
		qnn_log(QNN_LOG_LVL_WARN,
		        "fastrpc: INIT_CREATE failed sid=%u rc=%d rv=%d\n",
		        session, rc, rv);
		/* The caller performs RELEASE (or a successful PAS_STOP) before
		 * reclaiming the heap, including a partially created USER_PD. */
		return rc != 0 ? rc : Q8B_NPU_PAS_FAILED;
	}
	qnn_log(QNN_LOG_LVL_INFO, "fastrpc: USER_PD created attrs=%u shell=%u heap=%u\n",
	        attrs, image_size, g_init_mem.size);
	return 0;
}

int frpc_call(uint32_t op, uint64_t arg, uint32_t size)
{
	int ret;
	unsigned retry = 0;

	do {
		ret = (int)syscall3(SYS_PLATFORM_CTL, (ewokos_addr_t)op,
		                    (ewokos_addr_t)arg, (ewokos_addr_t)size);
		if (ret != Q8B_NPU_PAS_BUSY)
			break;
		frpc_pause(FRPC_RETRY_NS);
	} while (++retry <= FRPC_RETRY_MAX);
	return ret;
}

/* Platform-control returns an ewokos_addr_t.  Keep the full 64-bit value for
 * device addresses: the Linux FastRPC ABI prepends the compute context-bank
 * SID in bits 63:32, so truncating syscall3() here turns a valid address into
 * a physical pointer on the DSP. */
static uint64_t frpc_call64(uint32_t op, uint64_t arg, uint32_t size)
{
	uint64_t ret;
	unsigned retry = 0;

	do {
		ret = (uint64_t)syscall3(SYS_PLATFORM_CTL, (ewokos_addr_t)op,
		                         (ewokos_addr_t)arg,
		                         (ewokos_addr_t)size);
		if ((int64_t)ret != Q8B_NPU_PAS_BUSY)
			break;
		frpc_pause(FRPC_RETRY_NS);
	} while (++retry <= FRPC_RETRY_MAX);
	return ret;
}

bool frpc_available(void)
{
	int ret;

	if (g_frpc_avail >= 0)
		return g_frpc_avail == 1;
	/* PAS support does not imply that the GLINK/FastRPC service exists.  The
	 * kernel exposes a separate probe so a successful PAS bring-up never makes
	 * the QNN backend attempt an unimplemented transport. */
	ret = frpc_call(Q8B_NPU_FRPC_QUERY, 0, 0);
	g_frpc_avail = ((uint32_t)ret == Q8B_NPU_FRPC_ABI) ? 1 : 0;
	if (g_frpc_avail && frpc_call(Q8B_NPU_FRPC_METADATA_QUERY, 0, 0) ==
	        (int)Q8B_NPU_FRPC_METADATA_PAGE)
		g_meta_offset = Q8B_NPU_FRPC_METADATA_PAGE;
	if (g_frpc_avail && frpc_call(Q8B_NPU_FRPC_POINTER_QUERY, 0, 0) ==
	        (int)Q8B_NPU_FRPC_POINTER_HOST)
		g_host_pointers = 1;
	if (g_frpc_avail == 0)
		qnn_log(QNN_LOG_LVL_INFO,
		        "fastrpc: kernel NPU transport unavailable (ret=%d)\n",
		        ret);
	return g_frpc_avail == 1;
}

int frpc_session_open(uint32_t domain, uint32_t *session)
{
	return frpc_session_open_attrs(domain, 0, session);
}

int frpc_session_open_attrs(uint32_t domain, uint32_t attrs, uint32_t *session)
{
	return frpc_session_open_image(domain, attrs, NULL, 0, session);
}

int frpc_session_open_image(uint32_t domain, uint32_t attrs,
        const void *image, uint32_t image_size, uint32_t *session)
{
	int ret;
	uint32_t sid;

	if (!session || (attrs != 0 && attrs != FRPC_MODE_UNSIGNED_MODULE) ||
	    (!!image != !!image_size) || image_size > FRPC_INIT_MEM_SIZE)
		return Q8B_NPU_PAS_INVALID;
	*session = 0;
	if (g_init_session || g_transport_unsafe)
		return Q8B_NPU_PAS_BUSY;
	if (!frpc_available())
		return Q8B_NPU_PAS_UNSUPPORTED;
	ret = frpc_call(Q8B_NPU_FRPC_SESSION, (uint64_t)domain, 0);
	if (ret < 0)
		return ret;
	sid = (uint32_t)ret;
	g_init_session = sid;
	if (domain == FRPC_DOMAIN_CDSP)
		g_session = sid;
	g_transport_stopped = 0;
	g_init_released = 0;

	/* USER_PD creation is a separate initialization path from ROOT_PD
	 * attachment.  Linux device_open() only allocates the client/session;
	 * fastrpc_init_create_process() sends CREATE directly with USER_PD. */
	ret = frpc_init_create(sid, attrs, image, image_size);
	if (ret != 0) {
		frpc_session_close(sid);
		return ret;
	}

	*session = sid;
	return 0;
}

int frpc_session_release(uint32_t session)
{
	frpc_rbuf_t rbuf;
	int32_t client_id = FRPC_CLIENT_ID;
	int32_t rv = 0;
	uint32_t sc;
	int rc;
	if (!session) return Q8B_NPU_PAS_INVALID;
	if (g_transport_stopped || g_init_released) return 0;
	if (g_transport_unsafe || !frpc_available()) return Q8B_NPU_PAS_UNSUPPORTED;
	memset(&rbuf, 0, sizeof(rbuf));
	rbuf.data = &client_id;
	rbuf.size = sizeof(client_id);
	rbuf.fd = -1;
	sc = frpc_scalar(FRPC_RMID_INIT_RELEASE, 1u, 0u);
	rc = frpc_invoke(session, FRPC_INIT_HANDLE, sc, &rbuf, 1,
	                 NULL, 0, &rv);
	if (rc || rv) return rc ? rc : rv;
	g_init_released = 1;
	qnn_log(QNN_LOG_LVL_INFO, "fastrpc: USER_PD release acknowledged\n");
	return 0;
}

int frpc_session_close(uint32_t session)
{
	bool transport;
	int rc;

	if (!session || !g_init_session) return 0;
	if (session != g_init_session) return Q8B_NPU_PAS_INVALID;
	if (g_transport_unsafe && (rc = frpc_transport_abort()) != 0) return rc;
	/* Match Linux fastrpc_device_release(): release the framework process
	 * before tearing down the transport session.  This lets the DSP reclaim
	 * per-client user-PD state before releasing its initial heap. */
	if (frpc_session_release(session) != 0 && !g_transport_stopped &&
	    (rc = frpc_transport_abort()) != 0) return rc;
	transport = !g_transport_stopped && frpc_available();
	if (g_init_devaddr) {
		if (transport && frpc_munmap(session, g_init_devaddr,
		        g_init_mem.size) != 0) {
			qnn_log(QNN_LOG_LVL_WARN,
			        "fastrpc: initial heap unmap failed; stopping DSP before reclaim\n");
			if ((rc = frpc_transport_abort()) != 0) return rc;
			transport = false;
		}
		g_init_devaddr = 0;
	}
	if (g_init_mem.va)
		npu_buf_free(&g_init_mem);
	memset(&g_init_mem, 0, sizeof(g_init_mem));
	if (transport) {
		if (frpc_call(Q8B_NPU_FRPC_CLOSE, (uint64_t)session, 0) != 0) {
			if ((rc = frpc_transport_abort()) != 0) return rc;
		} else {
			/* CLOSE disables the SMMU and retires its mapping table. */
			g_transport_epoch++;
		}
	}
	g_init_session = 0;
	if (session == g_session)
		g_session = 0;
	return 0;
}

uint32_t frpc_session_get(void)
{
	return g_session;
}

int frpc_power_vote(uint32_t session, uint32_t level)
{
	if (!frpc_available())
		return Q8B_NPU_PAS_UNSUPPORTED;
	/* HAP_power DCVS v3 vote: level 0 = power saver .. 3 = turbo. */
	return frpc_call(Q8B_NPU_FRPC_PWR,
	                 ((uint64_t)session << 32) | (uint64_t)level, 0);
}

uint64_t frpc_mmap(uint32_t session, uint64_t pa, uint32_t size)
{
	uint64_t ret;

	/* The compact Q8B ABI carries a PFN in the low 32 bits.  The current DMA
	 * pool is below 4 GiB and all exported buffers are page aligned; reject a
	 * caller that would otherwise silently lose address bits or an intra-page
	 * offset. */
	if (g_transport_unsafe || !frpc_available() || !pa ||
	    (pa >> 32) != 0 || (pa & 0xfffull) ||
	    !size)
		return 0;
	/* arg packs the session and the 4 KB page-frame number of the staged
	   dma buffer (the kernel rebuilds pa = pfn << 12); size is the byte
	   length.  Returns the cDSP-visible device address, 0 on failure. */
	ret = frpc_call64(Q8B_NPU_FRPC_MMAP,
	                  ((uint64_t)session << 32) | ((pa >> 12) & 0xffffffffull),
	                  size);
	if ((int64_t)ret < 0)
		return 0;
	return ret;
}

int frpc_munmap(uint32_t session, uint64_t devaddr, uint32_t size)
{
	(void)session;
	if (!frpc_available())
		return Q8B_NPU_PAS_UNSUPPORTED;
	/* MUNMAP identifies the mapping by its consolidated DSP address.  The
	 * session is encoded in that address (SID << 32), so packing it again in
	 * the syscall argument would destroy the IOVA on 64-bit callers. */
	return frpc_call(Q8B_NPU_FRPC_MUNMAP, devaddr, size);
}

/* Retire only the AP reference after a confirmed root rejection/unmap.
 * A failed unmap must not erase the caller's sole ownership record. */
static int frpc_remote_ap_unmap(uint32_t session, frpc_remote_map_t *mapping)
{
	int rc;
	if (mapping->epoch != g_transport_epoch) {
		memset(mapping, 0, sizeof(*mapping));
		return 0;
	}
	rc = frpc_munmap(session, mapping->ap_devaddr, mapping->ap_size);
	if (rc == 0 || frpc_transport_abort() == 0)
		memset(mapping, 0, sizeof(*mapping));
	return rc;
}

static int frpc_remote_stop(frpc_remote_map_t *mapping)
{
	int rc = 0;
	if (mapping->epoch == g_transport_epoch)
		rc = frpc_transport_abort();
	if (rc == 0)
		memset(mapping, 0, sizeof(*mapping));
	return rc;
}

static int frpc_remote_map_result(uint32_t session, frpc_remote_map_t *mapping,
                int rc, int32_t rv, uint64_t vaddr)
{
	if (rc == 0 && rv == 0 && vaddr) {
		mapping->dsp_vaddr = vaddr;
		mapping->uncertain = 0;
		return 0;
	}
	/* frpc_invoke returns the DSP retval as rc too, but writes rv only
	 * after successful packet completion/cleanup. A nonzero rv therefore
	 * identifies an explicit root rejection despite the nonzero rc. */
	if (!rv) {
		/* No usable success/rejection: root may have installed a mapping.
		 * Do not revoke AP access underneath an unknown DSP mapping. */
		(void)frpc_remote_stop(mapping);
	} else {
		mapping->uncertain = 0;
		(void)frpc_remote_ap_unmap(session, mapping);
	}
	return rc ? rc : (rv ? rv : Q8B_NPU_PAS_FAILED);
}

int frpc_remote_mmap(uint32_t session, uint64_t pa, uint32_t size,
			     frpc_remote_map_t *mapping)
{
	return frpc_remote_mmap_flags(session, pa, size, FRPC_MMAP_ADD_PAGES, mapping);
}

int frpc_remote_mmap_flags(uint32_t session, uint64_t pa, uint32_t size,
                uint32_t flags, frpc_remote_map_t *mapping)
{
	struct frpc_mmap_req_msg req;
	struct frpc_phy_page page;
	struct frpc_mmap_rsp_msg rsp;
	frpc_rbuf_t rbufs[3];
	uint64_t ap_devaddr, extent;
	uint32_t ap_size;
	uint32_t sc;
	int32_t rv = 0;
	int rc;

	if (!mapping || !session || !pa || !size ||
	    (flags != 0 && flags != FRPC_MMAP_ADD_PAGES))
		return Q8B_NPU_PAS_INVALID;
	extent = size;
	if (extent > UINT32_MAX - 0xfffull)
		return Q8B_NPU_PAS_INVALID;
	ap_size = (uint32_t)((extent + 0xfffull) & ~0xfffull);
	if (!ap_size)
		return Q8B_NPU_PAS_INVALID;
	memset(mapping, 0, sizeof(*mapping));
	ap_devaddr = frpc_mmap(session, pa, ap_size);
	if (!ap_devaddr)
		return Q8B_NPU_PAS_UNSUPPORTED;
	*mapping = (frpc_remote_map_t){ .ap_devaddr = ap_devaddr,
		.size = size, .ap_size = ap_size, .epoch = g_transport_epoch,
		.uncertain = 1 };

	/* INIT_MMAP is the standard root method used by Linux fastrpc.c:
	 * req_msg and one fastrpc_phy_page are input buffers and rsp_msg is the
	 * DSP virtual address returned by the remote root object. */
	memset(&req, 0, sizeof(req));
	req.client_id = FRPC_CLIENT_ID;
	req.flags = flags;
	req.vaddr = 0;
	req.num = (int32_t)sizeof(page);
	memset(&page, 0, sizeof(page));
	page.addr = ap_devaddr;
	/* Linux fastrpc supplies the complete page-aligned allocation extent in
	 * the phy-page list, even when the caller's logical length is shorter. */
	page.size = ap_size;
	memset(&rsp, 0, sizeof(rsp));
	memset(rbufs, 0, sizeof(rbufs));
	rbufs[0].data = &req;
	rbufs[0].size = sizeof(req);
	rbufs[0].fd = -1;
	rbufs[1].data = &page;
	rbufs[1].size = sizeof(page);
	rbufs[1].fd = -1;
	rbufs[2].data = &rsp;
	rbufs[2].size = sizeof(rsp);
	rbufs[2].fd = -1;
	rbufs[2].is_out = true;
	sc = frpc_scalar(FRPC_RMID_INIT_MMAP, 2u, 1u);
	rc = frpc_invoke(session, FRPC_INIT_HANDLE, sc, rbufs, 3,
	                 NULL, 0, &rv);
	return frpc_remote_map_result(session, mapping, rc, rv, rsp.vaddr);
}

int frpc_remote_munmap(uint32_t session, frpc_remote_map_t *mapping)
{
	struct frpc_munmap_req_msg req;
	frpc_rbuf_t rbuf;
	uint32_t sc;
	int32_t rv = 0;
	int rc;

	if (!mapping || !session || !mapping->ap_devaddr || !mapping->size)
		return Q8B_NPU_PAS_INVALID;
	if (mapping->epoch != g_transport_epoch || mapping->uncertain ||
	    g_transport_unsafe)
		return frpc_remote_stop(mapping);
	if (!mapping->dsp_vaddr)
		return frpc_remote_ap_unmap(session, mapping);
	memset(&req, 0, sizeof(req));
	req.client_id = FRPC_CLIENT_ID;
	req.vaddr = mapping->dsp_vaddr;
	req.size = mapping->ap_size ? mapping->ap_size : mapping->size;
	memset(&rbuf, 0, sizeof(rbuf));
	rbuf.data = &req;
	rbuf.size = sizeof(req);
	rbuf.fd = -1;
	sc = frpc_scalar(FRPC_RMID_INIT_MUNMAP, 1u, 0u);
	rc = frpc_invoke(session, FRPC_INIT_HANDLE, sc, &rbuf, 1,
	                 NULL, 0, &rv);
	if (rv) return rv;
	if (rc) {
		mapping->uncertain = 1;
		(void)frpc_remote_stop(mapping);
		return rc;
	}
	mapping->dsp_vaddr = 0;
	return frpc_remote_ap_unmap(session, mapping);
}

int frpc_remote_mem_map(uint32_t session, int32_t fd, uint64_t pa,
			       uint32_t offset, uint32_t size, uint32_t flags,
			       frpc_remote_map_t *mapping)
{
	struct frpc_mem_map_req_msg req;
	struct frpc_phy_page page;
	struct frpc_mmap_rsp_msg rsp;
	frpc_rbuf_t rbufs[4];
	uint64_t ap_devaddr, extent;
	uint32_t ap_size, sc;
	int32_t rv = 0;
	int rc;

	if (!mapping || !session || fd < 0 || !pa || !size || (pa & 0xfffull) ||
		offset > INT32_MAX)
		return Q8B_NPU_PAS_INVALID;
	extent = (uint64_t)offset + size;
	if (extent > UINT32_MAX || ((extent + 0xfffull) & ~0xfffull) > UINT32_MAX)
		return Q8B_NPU_PAS_INVALID;
	ap_size = (uint32_t)((extent + 0xfffull) & ~0xfffull);
	memset(mapping, 0, sizeof(*mapping));
	ap_devaddr = frpc_mmap(session, pa, ap_size);
	if (!ap_devaddr)
		return Q8B_NPU_PAS_UNSUPPORTED;
	*mapping = (frpc_remote_map_t){ .ap_devaddr = ap_devaddr,
		.size = size, .ap_size = ap_size, .epoch = g_transport_epoch,
		.uncertain = 1 };

	/* INIT_MEM_MAP mirrors Linux fastrpc_req_mem_map(): the request carries
	 * fd/offset/flags, pages carries the AP DMA extent, arg[2] is the
	 * optional data list (zero length here), and rsp returns DSP vaddr. */
	memset(&req, 0, sizeof(req));
	req.client_id = FRPC_CLIENT_ID;
	req.fd = fd;
	req.offset = (int32_t)offset;
	req.flags = flags;
	req.vaddrin = 0;
	req.num = (int32_t)sizeof(page);
	req.data_len = 0;
	memset(&page, 0, sizeof(page));
	page.addr = ap_devaddr;
	page.size = ap_size;
	memset(&rsp, 0, sizeof(rsp));
	memset(rbufs, 0, sizeof(rbufs));
	rbufs[0].data = &req;
	rbufs[0].size = sizeof(req);
	rbufs[0].fd = -1;
	rbufs[1].data = &page;
	rbufs[1].size = sizeof(page);
	rbufs[1].fd = -1;
	rbufs[2].data = &page;
	rbufs[2].size = 0;
	rbufs[2].fd = -1;
	rbufs[3].data = &rsp;
	rbufs[3].size = sizeof(rsp);
	rbufs[3].fd = -1;
	rbufs[3].is_out = true;
	sc = frpc_scalar(FRPC_RMID_INIT_MEM_MAP, 3u, 1u);
	rc = frpc_invoke(session, FRPC_INIT_HANDLE, sc, rbufs, 4,
	                 NULL, 0, &rv);
	return frpc_remote_map_result(session, mapping, rc, rv, rsp.vaddr);
}

int frpc_remote_mem_unmap(uint32_t session, int32_t fd,
				frpc_remote_map_t *mapping)
{
	struct frpc_mem_unmap_req_msg req;
	frpc_rbuf_t rbuf;
	uint32_t sc;
	int32_t rv = 0;
	int rc;

	if (!mapping || !session || fd < 0 || !mapping->ap_devaddr || !mapping->size)
		return Q8B_NPU_PAS_INVALID;
	if (mapping->epoch != g_transport_epoch || mapping->uncertain ||
	    g_transport_unsafe)
		return frpc_remote_stop(mapping);
	if (!mapping->dsp_vaddr)
		return frpc_remote_ap_unmap(session, mapping);
	memset(&req, 0, sizeof(req));
	req.client_id = FRPC_CLIENT_ID;
	req.fd = fd;
	req.vaddrin = mapping->dsp_vaddr;
	req.len = mapping->ap_size ? mapping->ap_size : mapping->size;
	memset(&rbuf, 0, sizeof(rbuf));
	rbuf.data = &req;
	rbuf.size = sizeof(req);
	rbuf.fd = -1;
	sc = frpc_scalar(FRPC_RMID_INIT_MEM_UNMAP, 1u, 0u);
	rc = frpc_invoke(session, FRPC_INIT_HANDLE, sc, &rbuf, 1,
	                 NULL, 0, &rv);
	if (rv) return rv;
	if (rc) {
		mapping->uncertain = 1;
		(void)frpc_remote_stop(mapping);
		return rc;
	}
	mapping->dsp_vaddr = 0;
	return frpc_remote_ap_unmap(session, mapping);
}

struct frpc_remote_buf {
	uint64_t pv;
	uint64_t len;
};

struct frpc_invoke_buf {
	uint32_t num;
	uint32_t pgidx;
};

static uint64_t frpc_meta_size(uint32_t nscalars)
{
	return (uint64_t)nscalars * (sizeof(struct frpc_remote_buf) +
	                             sizeof(struct frpc_invoke_buf) +
	                             sizeof(struct frpc_phy_page)) +
	       (uint64_t)FRPC_MAX_FDS * sizeof(uint64_t) +
	       (uint64_t)FRPC_MAX_CRCS * sizeof(uint32_t);
}

static uint64_t frpc_data_offset(uint32_t nscalars, uint32_t prefix)
{
	/* The restricted Q8B prefix is consumed by the AP kernel.  Linux's
	 * FASTRPC_ALIGN is applied from struct fastrpc_msg.addr, which points at
	 * the first remote argument (payload + prefix), so align the upstream
	 * metadata length first and add the private prefix back afterwards. */
	return prefix + ((frpc_meta_size(nscalars) + (FRPC_ALIGN - 1u)) &
	              ~((uint64_t)FRPC_ALIGN - 1u));
}

static uint64_t frpc_inline_align(uint64_t cursor, uint32_t prefix)
{
	/* cursor is an offset from the private payload base.  FASTRPC_ALIGN is
	 * relative to msg.addr (payload + prefix), so keep the private prefix out of
	 * the rounding operation. */
	return prefix + (((cursor - prefix) + (FRPC_ALIGN - 1u)) &
	              ~((uint64_t)FRPC_ALIGN - 1u));
}

static int frpc_inline_end(const frpc_rbuf_t *rbufs, uint32_t nrbufs,
			   uint64_t *end, uint32_t prefix)
{
	uint64_t cursor;

	if (!end || (nrbufs && !rbufs))
		return Q8B_NPU_PAS_INVALID;
	cursor = frpc_data_offset(nrbufs, prefix);
	for (uint32_t i = 0; i < nrbufs; i++) {
		if (!rbufs[i].size || rbufs[i].pa != 0 || !rbufs[i].data)
			continue;
		if (cursor < prefix || cursor - prefix >
		    UINT64_MAX - (FRPC_ALIGN - 1u))
			return Q8B_NPU_PAS_INVALID;
		cursor = frpc_inline_align(cursor, prefix);
		if (rbufs[i].size > UINT64_MAX - cursor)
			return Q8B_NPU_PAS_INVALID;
		cursor += rbufs[i].size;
	}
	*end = cursor;
	return 0;
}

static void frpc_request_defer(frpc_request_t *req)
{
	req->deferred_next = g_deferred_requests;
	g_deferred_requests = req;
}

static int frpc_request_unmap(frpc_request_t *req)
{
	/* Clear each acknowledged reference independently: POLL must never
	 * repeat a successful unmap after a different mapping failed. */
	for (uint32_t i = 0; i < req->nrbufs; i++) {
		if (!req->mapped_iova[i]) continue;
		int rc = frpc_munmap(req->session, req->mapped_iova[i], req->mapped_size[i]);
		if (rc) return rc;
		req->mapped_iova[i] = 0;
	}
	if (req->payload_iova) {
		int rc = frpc_munmap(req->session, req->payload_iova, req->pay.size);
		if (rc) return rc;
		req->payload_iova = 0;
	}
	return 0;
}

/* Build the payload consumed by drivers/misc/fastrpc.c. The first 16 bytes,
   padded to a page when negotiated, are private to the restricted Q8B
   syscall (session, retval, handle, sc).
   The DSP sees the standard metadata immediately after that prefix: remote
   buffer descriptors, invoke buffer descriptors, page descriptors, fd/crc
   lists and the inline buffers. */
int frpc_request_start(uint32_t session, uint32_t handle, uint32_t sc,
                const frpc_rbuf_t *rbufs, uint32_t nrbufs,
                const int32_t *fds, uint32_t nfds, frpc_request_t **request)
{
	npu_buf_t pay;
	uint8_t *base;
	uint64_t off, data_off;
	uint64_t inline_end, wire_size;
	uint64_t payload_iova = 0;
	uint64_t mapped_iova[FRPC_MAX_RBUFS];
	uint32_t mapped_size[FRPC_MAX_RBUFS];
	uint64_t page_iova[FRPC_MAX_RBUFS];
	uint32_t i;
	uint32_t inbufs, outbufs, inhandles, outhandles;
	int rc;
	frpc_request_t *req;

	if (!request) return Q8B_NPU_PAS_INVALID;
	*request = NULL;

	if (g_transport_unsafe)
		return Q8B_NPU_PAS_BUSY;
	if (!frpc_available())
		return Q8B_NPU_PAS_UNSUPPORTED;
	if (nrbufs > FRPC_MAX_RBUFS || nfds > FRPC_MAX_FDS)
		return Q8B_NPU_PAS_INVALID;
	/* The kernel ownership table currently tracks physical DMA buffers, not
	 * Linux dma-buf file descriptors.  Do not serialize an fd list that the
	 * compact kernel ABI cannot resolve. */
	if (nfds)
		return Q8B_NPU_PAS_UNSUPPORTED;
	if (nrbufs && !rbufs)
		return Q8B_NPU_PAS_INVALID;
	/* Keep the userspace descriptor and the metadata count in lockstep.  A
	 * mismatched scalar word would otherwise make the kernel parse a prefix of
	 * this packet and leave the DSP looking at unrelated bytes. */
	inbufs = (sc >> 16) & 0xffu;
	outbufs = (sc >> 8) & 0xffu;
	inhandles = (sc >> 4) & 0x0fu;
	outhandles = sc & 0x0fu;
	if (inhandles || outhandles || inbufs + outbufs != nrbufs)
		return Q8B_NPU_PAS_INVALID;
	for (i = 0; i < nrbufs; i++) {
		/* This compact transport supports registered DMA buffers and staged
		 * copies.  A nonzero length without either backing store would become
		 * a NULL DSP pointer.  fd-backed mappings are intentionally rejected
		 * until the corresponding kernel fd table exists. */
		if (rbufs[i].fd >= 0 ||
		    (rbufs[i].offset && !rbufs[i].pa) ||
		    (rbufs[i].size && !rbufs[i].pa && !rbufs[i].data))
			return Q8B_NPU_PAS_INVALID;
		if (rbufs[i].pa && rbufs[i].size) {
			uint64_t extent = (uint64_t)rbufs[i].offset +
			                  rbufs[i].size;
			uint64_t rounded = (extent + 0xfffull) & ~0xfffull;
			if (!extent || rounded > UINT32_MAX)
				return Q8B_NPU_PAS_INVALID;
		}
	}

	/* Linux aligns the inline argument area to FASTRPC_ALIGN (128) from the
	 * metadata address in struct fastrpc_msg.  The private Q8B prefix is kept
	 * outside that view and is added back by frpc_data_offset(). */
	data_off = frpc_data_offset(nrbufs, g_meta_offset);
	/* Use the same metadata-relative cursor for allocation and serialization.
	 * Include all the bytes advertised by msg.size: rounding that extent to
	 * a page from msg.addr must fit even with the legacy 16-byte prefix. */
	if (frpc_inline_end(rbufs, nrbufs, &inline_end, g_meta_offset) != 0)
		return Q8B_NPU_PAS_INVALID;
	wire_size = (inline_end - g_meta_offset + 0xfffu) & ~0xfffull;
	if (wire_size > UINT32_MAX - g_meta_offset)
		return Q8B_NPU_PAS_INVALID;
	/* Allocate an ownership record before exposing any DMA to the SMMU. */
	req = calloc(1, sizeof(*req));
	if (!req) return Q8B_NPU_PAS_FAILED;
	if (npu_buf_alloc(&pay, (uint32_t)(g_meta_offset + wire_size)) != 0) {
		free(req);
		return Q8B_NPU_PAS_FAILED;
	}
	req->pay = pay;
	req->session = session;
	req->nrbufs = nrbufs;
	req->epoch = g_transport_epoch;
	memset(mapped_iova, 0, sizeof(mapped_iova));
	memset(mapped_size, 0, sizeof(mapped_size));
	memset(page_iova, 0, sizeof(page_iova));
	/* The invoke packet itself is a DSP-visible buffer.  Its private prefix is
	 * consumed by the kernel, while msg.addr points at the standard metadata
	 * immediately after that prefix. */
	payload_iova = frpc_mmap(session, pay.pa, pay.size);
	if (!payload_iova) {
		npu_buf_free(&pay);
		free(req);
		return Q8B_NPU_PAS_UNSUPPORTED;
	}
	base = (uint8_t *)pay.va;
	memset(base, 0, pay.size);

	/* Private syscall prefix; the kernel turns it into struct fastrpc_msg. */
	uint32_t packet_session = session | (g_meta_offset == Q8B_NPU_FRPC_METADATA_PAGE ?
	        Q8B_NPU_FRPC_PACKET_PAGE : 0) |
	        (g_host_pointers ? Q8B_NPU_FRPC_PACKET_HOST : 0);
	memcpy(base + 0, &packet_session, 4);
	memset(base + 4, 0, 4);
	memcpy(base + 8, &handle, 4);
	memcpy(base + 12, &sc, 4);
	off = g_meta_offset;
	/* remote buffer descriptors */
	for (i = 0; i < nrbufs; i++) {
		struct frpc_remote_buf b = { 0, rbufs[i].size };
		uint64_t a;
		if (!rbufs[i].size) {
			/* fastrpc_get_args() leaves pv/pages zero for empty arguments,
			 * even when a caller supplied a non-null host data pointer. */
			a = 0;
		} else if (rbufs[i].dspaddr != 0) {
			/* A standard INIT_MMAP caller may hand the DSP virtual address
			 * back on a later invoke.  Retain one AP-side reference for the
			 * page list and use the DSP address only in remote_buf.pv. */
			if (!rbufs[i].pa) {
				rc = Q8B_NPU_PAS_INVALID;
				goto unmap_buffers;
			}
			mapped_size[i] = (uint32_t)(((uint64_t)rbufs[i].offset +
					rbufs[i].size + 0xfffull) & ~0xfffull);
			mapped_iova[i] = frpc_mmap(session, rbufs[i].pa,
			                           mapped_size[i]);
			if (!mapped_iova[i]) {
				rc = Q8B_NPU_PAS_UNSUPPORTED;
				goto unmap_buffers;
			}
			if (rbufs[i].offset > UINT64_MAX - rbufs[i].dspaddr) {
				rc = Q8B_NPU_PAS_INVALID;
				goto unmap_buffers;
			}
			a = rbufs[i].dspaddr + rbufs[i].offset;
			page_iova[i] = mapped_iova[i] + rbufs[i].offset;
		} else if (rbufs[i].pa != 0) {
			mapped_size[i] = (uint32_t)(((uint64_t)rbufs[i].offset +
					rbufs[i].size + 0xfffull) & ~0xfffull);
			mapped_iova[i] = frpc_mmap(session, rbufs[i].pa,
			                           mapped_size[i]);
			if (!mapped_iova[i]) {
				rc = Q8B_NPU_PAS_UNSUPPORTED;
				goto unmap_buffers;
			}
			a = (g_host_pointers && rbufs[i].data ?
			        (uint64_t)(uintptr_t)rbufs[i].data : mapped_iova[i]) +
			        rbufs[i].offset;
			page_iova[i] = mapped_iova[i] + rbufs[i].offset;
		} else if (rbufs[i].data) {
			data_off = frpc_inline_align(data_off, g_meta_offset);
			a = (g_host_pointers ? (uint64_t)(uintptr_t)base : payload_iova) + data_off;
			page_iova[i] = payload_iova + data_off;
			if (i < inbufs)
				memcpy(base + data_off, rbufs[i].data, rbufs[i].size);
			data_off += rbufs[i].size;
		} else {
			a = 0;
		}
		b.pv = a;
		memcpy(base + off, &b, sizeof(b));
		off += sizeof(b);
	}
	/* invoke buffer and page lists */
	for (i = 0; i < nrbufs; i++) {
		struct frpc_invoke_buf b = { rbufs[i].size ? 1u : 0u, i };
		memcpy(base + off, &b, sizeof(b));
		off += sizeof(b);
	}
	for (i = 0; i < nrbufs; i++) {
		struct frpc_phy_page p = { 0, 0 };
		uint64_t a;
		memcpy(&a, base + g_meta_offset + i * sizeof(struct frpc_remote_buf), 8);
		if (a) {
			p.addr = page_iova[i] & ~0xfffull;
			p.size = ((page_iova[i] & 0xfffull) +
				rbufs[i].size + 0xfffu) &
				~0xfffull;
		}
		memcpy(base + off, &p, sizeof(p));
		off += sizeof(p);
	}
	/* fdlist and crclist are fixed-size arrays in the upstream payload. */
	for (i = 0; i < nfds && i < FRPC_MAX_FDS; i++) {
		uint64_t fd = (uint32_t)fds[i];
		memcpy(base + off + i * sizeof(fd), &fd, sizeof(fd));
	}

	npu_dcache_flush(base, pay.size);
	/* BUSY from SUBMIT means not accepted, so bounded retry is safe here.
	 * An accepted request is polled; it is never resent after a timeout. */
	rc = frpc_call(g_in_progress ? Q8B_NPU_FRPC_SUBMIT_LISTENER :
	        Q8B_NPU_FRPC_SUBMIT, pay.pa, pay.size);
	if (rc != 0) goto unmap_buffers;
	req->pay = pay;
	req->payload_iova = payload_iova;
	req->pointer_base = g_host_pointers ? (uint64_t)(uintptr_t)base : payload_iova;
	memcpy(req->mapped_iova, mapped_iova, sizeof(mapped_iova));
	memcpy(req->mapped_size, mapped_size, sizeof(mapped_size));
	if (nrbufs) memcpy(req->rbufs, rbufs, nrbufs * sizeof(*rbufs));
	req->session = session;
	req->nrbufs = nrbufs;
	req->inbufs = inbufs;
	req->meta_offset = g_meta_offset;
	req->epoch = g_transport_epoch;
	*request = req;
	return 0;

unmap_buffers:
	req->payload_iova = payload_iova;
	memcpy(req->mapped_iova, mapped_iova, sizeof(mapped_iova));
	memcpy(req->mapped_size, mapped_size, sizeof(mapped_size));
	if (frpc_request_unmap(req) != 0) {
		qnn_log(QNN_LOG_LVL_WARN, "fastrpc: unsubmitted request unmap failed; stopping DSP before reclaim\n");
		if (frpc_transport_abort() != 0) {
			frpc_request_defer(req);
			return rc;
		}
	}
	npu_buf_free(&pay);
	free(req);
	return rc;
}

/* Consume the reply once; reclaim only after all unmaps or secure stop.
 * A retained request still owns its payload and remaining mapping records. */
int frpc_request_poll(frpc_request_t **request, int32_t *retval)
{
	if (!request || !*request) return Q8B_NPU_PAS_INVALID;
	frpc_request_t *req = *request;
	int stopped = req->epoch != g_transport_epoch;
	uint8_t *base = req->pay.va;
	int32_t rv = Q8B_NPU_PAS_UNSUPPORTED;
	if (!stopped) {
		if (!req->completed) {
			int rc = (int)syscall3(SYS_PLATFORM_CTL,
			        Q8B_NPU_FRPC_POLL, req->pay.pa, req->pay.size);
			if (rc != 0) return rc;
			npu_dcache_inval(base, req->pay.size);
			memcpy(&req->retval, base + 4, 4);
			req->completed = 1;
		}
		if (!req->cleanup_error)
			req->cleanup_error = frpc_request_unmap(req);
		if (req->cleanup_error) {
			qnn_log(QNN_LOG_LVL_WARN, "fastrpc: completed request unmap failed; stopping DSP before reclaim\n");
			if (frpc_transport_abort() != 0) return req->cleanup_error;
			stopped = 1;
		}
	}
	if (!stopped) {
		rv = req->retval;
		for (uint32_t i = req->inbufs; i < req->nrbufs; i++) {
			frpc_rbuf_t *arg = &req->rbufs[i];
			uint64_t pv;
			memcpy(&pv, base + req->meta_offset + i * sizeof(struct frpc_remote_buf), 8);
			if (!arg->pa && arg->data && pv >= req->pointer_base &&
			    pv - req->pointer_base <= req->pay.size &&
			    arg->size <= req->pay.size - (pv - req->pointer_base))
				memcpy(arg->data, base + pv - req->pointer_base, arg->size);
		}
	}
	int cleanup_error = req->cleanup_error;
	npu_buf_free(&req->pay);
	free(req);
	*request = NULL;
	if (retval && !cleanup_error) *retval = rv;
	return cleanup_error;
}

int frpc_invoke(uint32_t session, uint32_t handle, uint32_t sc,
                const frpc_rbuf_t *rbufs, uint32_t nrbufs,
                const int32_t *fds, uint32_t nfds, int32_t *retval)
{
	frpc_request_t *req = NULL;
	int32_t rv = 0;
	int rc = frpc_request_start(session, handle, sc, rbufs, nrbufs, fds, nfds, &req);
	if (rc != 0) return rc;
	uint64_t start_ns = 0, now_ns;
	int timed = kernel_tic_nsec(&start_ns) == 0;
	int clock_backstep_logged = 0;
	for (unsigned i = 0; i < 30000; i++) {
		rc = frpc_request_poll(&req, &rv);
		if (rc != Q8B_NPU_PAS_BUSY) break;
		if (timed && kernel_tic_nsec(&now_ns) == 0) {
			/* An interpolated read can fall back to an older coarse tick.
			 * Unsigned subtraction would turn that backstep into a false
			 * 30-second expiry and stop a DSP with live work after only ms.
			 * Keep the original deadline and the bounded polling fallback. */
			if (now_ns < start_ns) {
				if (!clock_backstep_logged)
					qnn_log(QNN_LOG_LVL_WARN,
					        "fastrpc: clock before invoke start handle=%u sc=%08x poll=%u start_ns=%llu now_ns=%llu; keeping pending request\n",
					        handle, sc, i + 1u, (unsigned long long)start_ns,
					        (unsigned long long)now_ns);
				clock_backstep_logged = 1;
			} else if (now_ns - start_ns >= 30000000000ull) break;
		}
		if (g_progress) {
			int previous = g_in_progress;
			g_in_progress = 1;
			g_progress();
			g_in_progress = previous;
		}
		frpc_pause(1000000ull);
	}
	if (req) {
		qnn_log(QNN_LOG_LVL_WARN, "fastrpc: invoke incomplete handle=%u sc=%08x rc=%d; stopping DSP\n", handle, sc, rc);
		if (!g_transport_unsafe && frpc_transport_abort() == 0)
			(void)frpc_request_poll(&req, NULL);
		else {
			qnn_log(QNN_LOG_LVL_WARN, "fastrpc: stop failed; retaining pending DMA\n");
			frpc_request_defer(req);
		}
		return rc;
	}
	if (retval && rc == 0) *retval = rv;
	return rc != 0 ? rc : rv;
}

int frpc_open(uint32_t session, const char *uri, uint32_t *handle)
{
	frpc_rbuf_t rbufs[4];
	uint32_t in[2], out[2] = { 0, 0 };
	char error[255] = { 0 };
	size_t uri_len;
	int32_t rv = 0;
	uint32_t sc;
	int rc;

	if (!handle || !uri)
		return Q8B_NPU_PAS_INVALID;
	*handle = 0;
	uri_len = strlen(uri);
	if (!uri_len || uri_len >= UINT32_MAX)
		return Q8B_NPU_PAS_INVALID;
	if (!frpc_available())
		return Q8B_NPU_PAS_UNSUPPORTED;
	/* quic/fastrpc idl/remotectl.idl and src/remotectl_stub.c:
	 * handle 0, method 0; primitive {nameLen, dlerrorLen}, name,
	 * primitive {remoteHandle, loaderError}, dlerror.  remote_handle64_open
	 * adds a local 64-bit wrapper in libcdsprpc; the DSP handle is 32-bit. */
	in[0] = (uint32_t)uri_len + 1u;
	in[1] = sizeof(error);
	memset(rbufs, 0, sizeof(rbufs));
	for (uint32_t i = 0; i < 4; i++) rbufs[i].fd = -1;
	rbufs[0].data = in;
	rbufs[0].size = sizeof(in);
	rbufs[1].data = (void *)uri;
	rbufs[1].size = in[0];
	rbufs[2].data = out;
	rbufs[2].size = sizeof(out);
	rbufs[2].is_out = true;
	rbufs[3].data = error;
	rbufs[3].size = sizeof(error);
	rbufs[3].is_out = true;
	sc = frpc_scalar(0u, 2u, 2u);
	rc = frpc_invoke(session, 0u, sc, rbufs, 4, NULL, 0, &rv);
	if (rc != 0)
		return rc;
	if (rv != 0)
		return rv;
	if (out[1] != 0) {
		error[sizeof(error) - 1u] = 0;
		qnn_log(QNN_LOG_LVL_WARN, "fastrpc: skel loader error=%u: %s\n",
		        out[1], error);
		return (int32_t)out[1];
	}
	if (out[0] <= FRPC_INIT_HANDLE || out[0] == UINT32_MAX)
		return Q8B_NPU_PAS_FAILED;
	*handle = out[0];
	return 0;
}

int frpc_close(uint32_t session, uint32_t handle)
{
	frpc_rbuf_t rbufs[3];
	uint32_t in[2], loader_error = 0;
	char error[255] = { 0 };
	int32_t rv = 0;
	int rc;

	if (!handle || handle == FRPC_INIT_HANDLE || handle == UINT32_MAX)
		return Q8B_NPU_PAS_INVALID;
	if (!frpc_available())
		return Q8B_NPU_PAS_UNSUPPORTED;
	/* Closing a module calls remotectl.close; invoking method 0 on the
	 * module itself could instead execute one of its application methods. */
	in[0] = handle;
	in[1] = sizeof(error);
	memset(rbufs, 0, sizeof(rbufs));
	for (uint32_t i = 0; i < 3; i++) rbufs[i].fd = -1;
	rbufs[0].data = in;
	rbufs[0].size = sizeof(in);
	rbufs[1].data = &loader_error;
	rbufs[1].size = sizeof(loader_error);
	rbufs[1].is_out = true;
	rbufs[2].data = error;
	rbufs[2].size = sizeof(error);
	rbufs[2].is_out = true;
	rc = frpc_invoke(session, 0u, frpc_scalar(1u, 1u, 2u),
	                 rbufs, 3, NULL, 0, &rv);
	if (rc != 0 || rv != 0)
		return rc != 0 ? rc : rv;
	if (loader_error) {
		error[sizeof(error) - 1u] = 0;
		qnn_log(QNN_LOG_LVL_WARN, "fastrpc: skel close error=%u: %s\n",
		        loader_error, error);
	}
	return (int32_t)loader_error;
}
