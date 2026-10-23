/*
 * npu_fastrpc.c - FastRPC client transport for the SC8280XP cDSP.
 *
 * This is the client half of the reconstruction described in npu_fastrpc.h:
 * the invoke payload is laid out exactly as drivers/misc/fastrpc.c parses it
 * (header {session,retval}, handle, scalar descriptor, fd list, arg offsets,
 * then the 8-byte-aligned buffer data), and the scalar descriptor is packed
 * with the FASTRPC_SCALAR macro semantics.  Everything rides the restricted
 * Q8B kernel ABI on syscall3(SYS_PLATFORM_CTL, op, arg, size), the same door
 * libadreno uses for the GPU PAS.
 *
 * The current kernel implements only GPU PAS id 13, so the QUERY op returns
 * Q8B_NPU_PAS_UNSUPPORTED and every entry point here declines cleanly (no
 * crash, no hang) - exactly how the Adreno zap flow declines on a kernel
 * without SCM.  When a cDSP FastRPC service is added kernel-side, the payload
 * marshalling below is already faithful to the Linux layout.
 */
#include <adrenonpu/qnn_core_internal.h>
#include <adrenonpu/npu_cdsp.h>
#include <ewoksys/syscall.h>
#include <ewoksys/proc.h>
#include <syscalls.h>
#include <stdlib.h>
#include <string.h>

#define FRPC_RETRY_MAX 20u
#define FRPC_RETRY_NS  30000000ull   /* 30 ms, matches the Linux PAS retry */

/* -1 unknown, 0 absent, 1 present. */
static int g_frpc_avail = -1;
static uint32_t g_session;

int frpc_call(uint32_t op, uint64_t arg, uint32_t size)
{
	int ret;
	unsigned retry = 0;

	do {
		ret = (int)syscall3(SYS_PLATFORM_CTL, (ewokos_addr_t)op,
		                    (ewokos_addr_t)arg, (ewokos_addr_t)size);
		if (ret != Q8B_NPU_PAS_BUSY)
			break;
		proc_nsleep_precise(FRPC_RETRY_NS);
	} while (++retry <= FRPC_RETRY_MAX);
	return ret;
}

bool frpc_available(void)
{
	int ret;

	if (g_frpc_avail >= 0)
		return g_frpc_avail == 1;
	ret = frpc_call(Q8B_NPU_PAS_QUERY, 0, 0);
	g_frpc_avail = ((uint32_t)ret == Q8B_NPU_PAS_ABI) ? 1 : 0;
	if (g_frpc_avail == 0)
		qnn_log(QNN_LOG_LVL_INFO,
		        "fastrpc: kernel NPU transport unavailable (ret=%d)\n",
		        ret);
	return g_frpc_avail == 1;
}

int frpc_session_open(uint32_t domain, uint32_t *session)
{
	int ret;

	if (!session)
		return Q8B_NPU_PAS_INVALID;
	if (!frpc_available())
		return Q8B_NPU_PAS_UNSUPPORTED;
	ret = frpc_call(Q8B_NPU_FRPC_SESSION, (uint64_t)domain, 0);
	if (ret < 0)
		return ret;
	*session = (uint32_t)ret;
	if (domain == FRPC_DOMAIN_CDSP)
		g_session = (uint32_t)ret;
	return 0;
}

void frpc_session_close(uint32_t session)
{
	if (!frpc_available())
		return;
	(void)frpc_call(Q8B_NPU_FRPC_CLOSE, (uint64_t)session, 0);
	if (session == g_session)
		g_session = 0;
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
	int ret;

	if (!frpc_available())
		return 0;
	/* arg packs the session and the 4 KB page-frame number of the staged
	   dma buffer (the kernel rebuilds pa = pfn << 12); size is the byte
	   length.  Returns the cDSP-visible device address, 0 on failure. */
	ret = frpc_call(Q8B_NPU_FRPC_MMAP,
	                ((uint64_t)session << 32) | ((pa >> 12) & 0xffffffffull),
	                size);
	if (ret < 0)
		return 0;
	return (uint64_t)(uint32_t)ret;
}

int frpc_munmap(uint32_t session, uint64_t devaddr, uint32_t size)
{
	if (!frpc_available())
		return Q8B_NPU_PAS_UNSUPPORTED;
	return frpc_call(Q8B_NPU_FRPC_MUNMAP,
	                 ((uint64_t)session << 32) | (devaddr & 0xffffffffull),
	                 size);
}

/* Build the invoke payload (see file header), stage it in a dma buffer so the
   kernel can hand the cDSP a physical address, issue INVOKE, then copy the
   out-buffers and the remote retval back. */
int frpc_invoke(uint32_t session, uint32_t handle, uint32_t sc,
                const frpc_rbuf_t *rbufs, uint32_t nrbufs,
                const int32_t *fds, uint32_t nfds, int32_t *retval)
{
	npu_buf_t pay;
	uint8_t *base;
	uint64_t off;
	uint32_t i;
	uint64_t data_off;
	int rc;
	int32_t rv = 0;

	if (!frpc_available())
		return Q8B_NPU_PAS_UNSUPPORTED;
	if (nrbufs > FRPC_MAX_RBUFS || nfds > FRPC_MAX_FDS)
		return Q8B_NPU_PAS_INVALID;

	/* Payload size: header(8) + handle(4) + sc(4) + fds + args + data. */
	{
		uint64_t sz = 16 + (uint64_t)nfds * 4u + (uint64_t)nrbufs * 8u;

		for (i = 0; i < nrbufs; i++) {
			if (rbufs[i].pa == 0 && rbufs[i].data)
				sz += ((uint64_t)rbufs[i].size + 7u) & ~7ull;
		}
		if (npu_buf_alloc(&pay, (uint32_t)sz) != 0)
			return Q8B_NPU_PAS_FAILED;
	}
	base = (uint8_t *)pay.va;
	memset(base, 0, pay.size);

	/* header { session, retval } */
	memcpy(base + 0, &session, 4);
	memset(base + 4, 0, 4);
	memcpy(base + 8, &handle, 4);
	memcpy(base + 12, &sc, 4);
	off = 16;
	for (i = 0; i < nfds; i++) {
		memcpy(base + off, &fds[i], 4);
		off += 4;
	}
	/* arg slots: payload-relative offsets of each buffer's data. */
	data_off = off + (uint64_t)nrbufs * 8u;
	for (i = 0; i < nrbufs; i++) {
		uint64_t a;

		if (rbufs[i].pa != 0) {
			a = rbufs[i].pa;      /* registered dma buffer by pa */
		} else if (rbufs[i].data) {
			a = data_off;
			if (!rbufs[i].is_out && rbufs[i].size)
				memcpy(base + data_off, rbufs[i].data,
				       rbufs[i].size);
			data_off = (data_off + rbufs[i].size + 7u) & ~7ull;
		} else {
			a = 0;
		}
		memcpy(base + off, &a, 8);
		off += 8;
	}

	npu_dcache_flush(base, pay.size);
	rc = frpc_call(Q8B_NPU_FRPC_INVOKE, pay.pa, pay.size);
	if (rc != 0) {
		npu_buf_free(&pay);
		return rc;
	}
	npu_dcache_inval(base, pay.size);
	memcpy(&rv, base + 4, 4);
	/* Copy back out-buffers that were staged as payload copies. */
	off = 16 + (uint64_t)nfds * 4u;
	for (i = 0; i < nrbufs; i++) {
		uint64_t a;

		memcpy(&a, base + off, 8);
		off += 8;
		if (rbufs[i].pa == 0 && rbufs[i].is_out && rbufs[i].data &&
		    a + rbufs[i].size <= pay.size)
			memcpy(rbufs[i].data, base + a, rbufs[i].size);
	}
	npu_buf_free(&pay);
	if (retval)
		*retval = rv;
	return rv;
}

int frpc_open(uint32_t session, const char *uri, uint32_t *handle)
{
	frpc_rbuf_t rbufs[2];
	uint32_t h = 0;
	int32_t rv = 0;
	uint32_t sc;
	int rc;

	if (!handle || !uri)
		return Q8B_NPU_PAS_INVALID;
	if (!frpc_available())
		return Q8B_NPU_PAS_UNSUPPORTED;
	/* remote_handle64_open(uri, &h): a root-object (handle 0) invoke with
	   the URI as a by-value in-buffer and the handle as an out-buffer. */
	memset(rbufs, 0, sizeof(rbufs));
	rbufs[0].data = (void *)uri;
	rbufs[0].size = (uint32_t)strlen(uri) + 1u;
	rbufs[0].fd = -1;
	rbufs[0].is_out = false;
	rbufs[1].data = &h;
	rbufs[1].size = 4;
	rbufs[1].fd = -1;
	rbufs[1].is_out = true;
	sc = frpc_scalar(0u, 1u, 0u, 2u);
	rc = frpc_invoke(session, 0u, sc, rbufs, 2, NULL, 0, &rv);
	if (rc != 0)
		return rc;
	*handle = h;
	return rv;
}

int frpc_close(uint32_t session, uint32_t handle)
{
	int32_t rv = 0;
	uint32_t sc = frpc_scalar(0u, 1u, 0u, 0u);

	if (!frpc_available())
		return Q8B_NPU_PAS_UNSUPPORTED;
	return frpc_invoke(session, handle, sc, NULL, 0, NULL, 0, &rv) == 0 ?
	       (int)rv : Q8B_NPU_PAS_FAILED;
}
