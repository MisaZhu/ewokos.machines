/*
 * qnn_mem.c - QnnMem_* ION/DMA buffer registration.
 *
 * QnnMem_register hands the backend a dma-buf fd so tensors can be shared
 * zero-copy with the cDSP (the vendor path maps it through the FastRPC SMMU).
 * EwokOS user space has no dma-buf importer, so the registration is backed by
 * the kernel dma pool (dma_alloc block 0 - the same pool libadreno uses): a
 * physically contiguous, NOCACHE, device-visible region whose physical address
 * the reconstructed FastRPC transport can stage for the cDSP SMMU.  The fd and
 * offset are recorded for bookkeeping and returned by getBufferFd; the host
 * pointer handed back is the dma pool VA.
 */
#include <adrenonpu/qnn_core_internal.h>
#include <ewoksys/dma.h>
#include <stdlib.h>
#include <string.h>

void qnn_shared_free_storage(qnn_shared_buf_t *sb)
{
	if (!sb)
		return;
	if (sb->host) {
		dma_free(0, (ewokos_addr_t)(uintptr_t)sb->host);
		sb->host = NULL;
	}
	sb->pa = 0;
	sb->size = 0;
}

qnn_shared_buf_t *qnn_shared_buf_alloc(qnn_backend_obj_t *b, int fd,
                                       uint64_t offset, uint64_t size)
{
	qnn_shared_buf_t *sb;
	ewokos_addr_t va;
	ewokos_addr_t pa;

	if (!b || size == 0)
		return NULL;
	sb = (qnn_shared_buf_t *)calloc(1, sizeof(*sb));
	if (!sb)
		return NULL;
	va = dma_alloc(0, (uint32_t)size);
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
	memset((void *)(uintptr_t)va, 0, (size_t)size);
	sb->fd = fd;
	sb->offset = offset;
	sb->size = size;
	sb->host = (void *)(uintptr_t)va;
	sb->pa = (uint64_t)pa;
	sb->refcount = 1;
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

			if (--sb->refcount > 0)
				return QNN_SUCCESS;
			*pp = sb->next;
			qnn_shared_free_storage(sb);
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
