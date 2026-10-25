/*
 * qnn_mem.c - QnnMem_* ION/DMA buffer registration.
 *
 * QnnMem_register hands the backend a dma-buf fd so tensors can be shared
 * zero-copy with the cDSP (the vendor path maps it through the FastRPC SMMU).
 * EwokOS user space has no dma-buf importer, so the registration is backed by
 * the kernel dma pool (dma_alloc block 0 - the same pool libadreno uses): a
 * physically contiguous, NOCACHE, device-visible region whose physical address
 * the reconstructed FastRPC transport can stage for the cDSP SMMU.  EwokOS
 * has no dma-buf importer yet, so external fds are declined explicitly; the
 * fd=-1 path still honors offset by exposing a pointer into a page-aligned
 * DMA allocation while mapping the complete allocation extent.
 */
#include <adrenonpu/qnn_core_internal.h>
#include <ewoksys/dma.h>
#include <stdlib.h>
#include <string.h>

int qnn_shared_free_storage(qnn_shared_buf_t *sb)
{
	if (!sb)
		return QNN_ERROR;
	if (sb->devaddr && sb->map_epoch == frpc_mapping_epoch()) {
		if (!sb->map_session || frpc_munmap(sb->map_session,
		        sb->devaddr, sb->map_size) != 0)
			return QNN_ERROR;
	}
	sb->devaddr = 0;
	sb->map_session = 0;
	sb->map_epoch = 0;
	if (sb->alloc_host) {
		dma_free(0, (ewokos_addr_t)(uintptr_t)sb->alloc_host);
		sb->alloc_host = NULL;
	}
	sb->host = NULL;
	sb->pa = 0;
	sb->map_size = 0;
	sb->size = 0;
	return QNN_SUCCESS;
}

qnn_shared_buf_t *qnn_shared_buf_alloc(qnn_backend_obj_t *b, int fd,
                                       uint64_t offset, uint64_t size)
{
	qnn_shared_buf_t *sb;
	ewokos_addr_t va;
	ewokos_addr_t pa;

	uint64_t total;
	uint32_t map_size;

	/* There is no dma-buf fd importer in EwokOS yet.  Declining fd-backed
	 * registrations is safer than allocating unrelated storage while claiming
	 * that it aliases the caller's fd. */
	if (!b || b->native_mode || fd >= 0 || size == 0 || offset > UINT32_MAX ||
	    size > UINT32_MAX || offset > UINT32_MAX - size)
		return NULL;
	total = offset + size;
	map_size = (uint32_t)((total + 0xfffull) & ~0xfffull);
	if (!map_size)
		return NULL;
	sb = (qnn_shared_buf_t *)calloc(1, sizeof(*sb));
	if (!sb)
		return NULL;
	va = dma_alloc(0, map_size);
	if (va == 0) {
		free(sb);
		return NULL;
	}
	pa = dma_phy_addr(0, va);
	if (pa == 0) {
		dma_free(0, va);
		free(sb);
		return NULL;
	}
	memset((void *)(uintptr_t)va, 0, map_size);
	sb->fd = fd;
	sb->offset = offset;
	sb->size = size;
	sb->alloc_host = (void *)(uintptr_t)va;
	sb->host = (void *)(uintptr_t)(va + offset);
	sb->pa = (uint64_t)pa;
	sb->map_size = map_size;
	sb->refcount = 1;
	/* Keep the mapping with the registration entry so unregister and backend
	 * teardown can release the exact FastRPC ownership record.  A declined
	 * mapping leaves the CPU-visible registration usable. */
	if (b->npu_available && b->frpc_session) {
		sb->devaddr = frpc_mmap(b->frpc_session, sb->pa, sb->map_size);
		if (sb->devaddr) {
			sb->map_session = b->frpc_session;
			sb->map_epoch = frpc_mapping_epoch();
		}
	}
	/* Prepend to the backend's shared list. */
	sb->next = b->shared;
	b->shared = sb;
	return sb;
}

qnn_shared_buf_t *qnn_shared_buf_find(qnn_backend_obj_t *b, void *host)
{
	qnn_shared_buf_t *sb;

	if (!b)
		return NULL;
	for (sb = b->shared; sb; sb = sb->next) {
		if (sb->host == host)
			return sb;
	}
	return NULL;
}

int qnn_shared_buf_free(qnn_backend_obj_t *b, void *host)
{
	qnn_shared_buf_t **pp;

	if (!b)
		return QNN_ERROR;
	for (pp = &b->shared; *pp; pp = &(*pp)->next) {
		if ((*pp)->host == host) {
			qnn_shared_buf_t *sb = *pp;

			if (sb->refcount > 1) {
				sb->refcount--;
				return QNN_SUCCESS;
			}
			if (qnn_shared_free_storage(sb) != QNN_SUCCESS)
				return QNN_ERROR;
			*pp = sb->next;
			free(sb);
			return QNN_SUCCESS;
		}
	}
	return QNN_ERROR;
}

Qnn_ErrorHandle_t QnnMem_register(Qnn_BackendHandle_t backend, int fd,
                                  uint64_t offset, uint64_t size,
                                  void **hostPtr)
{
	qnn_backend_obj_t *b = (qnn_backend_obj_t *)
		qnn_obj_from_handle(backend, QNN_OBJ_BACKEND);
	qnn_shared_buf_t *sb;

	if (!b || !hostPtr || size == 0)
		return QNN_ERROR;
	sb = qnn_shared_buf_alloc(b, fd, offset, size);
	if (!sb)
		return QNN_ERROR;
	*hostPtr = sb->host;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnMem_unregister(Qnn_BackendHandle_t backend, void *hostPtr)
{
	qnn_backend_obj_t *b = (qnn_backend_obj_t *)
		qnn_obj_from_handle(backend, QNN_OBJ_BACKEND);

	if (!b || !hostPtr)
		return QNN_ERROR;
	return qnn_shared_buf_free(b, hostPtr);
}

Qnn_ErrorHandle_t QnnMem_getBufferFd(Qnn_BackendHandle_t backend, void *hostPtr,
                                     int *fd)
{
	qnn_backend_obj_t *b = (qnn_backend_obj_t *)
		qnn_obj_from_handle(backend, QNN_OBJ_BACKEND);
	qnn_shared_buf_t *sb;

	if (!b || !hostPtr || !fd)
		return QNN_ERROR;
	sb = qnn_shared_buf_find(b, hostPtr);
	if (!sb)
		return QNN_ERROR;
	*fd = sb->fd;
	return QNN_SUCCESS;
}
