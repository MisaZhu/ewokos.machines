/*
 * qnn_handle.c - handle/object plumbing, config helpers and small shared
 * utilities for the QNN C API implementation.
 *
 * Handles are pointer values (see qnn_core_internal.h); validation is the
 * (magic, type) pair every object carries.  This is the same trade-off the
 * vendor libQnnHtp.so makes: O(1) handle checks, no global lock, and stale
 * handles fault into QNN_ERROR instead of wild dereferences (a use-after-
 * free of the object storage itself cannot be caught here - identical to
 * the vendor library's documented handle contract).
 */
#include <adrenonpu/qnn_core_internal.h>
#include <ewoksys/kernel_tic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *qnn_obj_alloc(uint32_t type, size_t size)
{
	qnn_obj_head_t *o = (qnn_obj_head_t *)calloc(1, size);

	if (!o)
		return NULL;
	o->magic = QNN_OBJ_MAGIC;
	o->type = type;
	return o;
}

void *qnn_obj_from_handle(Qnn_Handle_t h, uint32_t type)
{
	qnn_obj_head_t *o;

	if (h == 0)
		return NULL;
	o = (qnn_obj_head_t *)(uintptr_t)h;
	if (o->magic != QNN_OBJ_MAGIC || o->type != type)
		return NULL;
	return o;
}

void qnn_obj_free(void *obj)
{
	qnn_obj_head_t *o = (qnn_obj_head_t *)obj;

	if (!o)
		return;
	o->magic = 0;
	free(o);
}

char *qnn_strdup_safe(const char *s)
{
	size_t n;
	char *d;

	if (!s)
		return NULL;
	n = strlen(s) + 1;
	d = (char *)malloc(n);
	if (d)
		memcpy(d, s, n);
	return d;
}

const char *qnn_cfg_get(const Qnn_ConfigParam_t *cfg, uint32_t n,
                        const char *key)
{
	uint32_t i;

	for (i = 0; cfg && i < n; i++) {
		if (cfg[i].key && strcmp(cfg[i].key, key) == 0)
			return cfg[i].value;
	}
	return NULL;
}

int qnn_cfg_get_int(const Qnn_ConfigParam_t *cfg, uint32_t n, const char *key,
                    int dflt)
{
	const char *v = qnn_cfg_get(cfg, n, key);

	if (!v || !*v)
		return dflt;
	if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X'))
		return (int)strtoul(v + 2, NULL, 16);
	return (int)strtol(v, NULL, 10);
}

void qnn_cfg_free(char **keys, char **vals, uint32_t *n)
{
	uint32_t i;

	for (i = 0; i < *n; i++) {
		free(keys[i]);
		free(vals[i]);
		keys[i] = NULL;
		vals[i] = NULL;
	}
	*n = 0;
}

int qnn_cfg_store(char **keys, char **vals, uint32_t *n,
                  const Qnn_ConfigParam_t *cfg, uint32_t ncfg, uint32_t cap)
{
	uint32_t i;

	if (!cfg)
		return QNN_SUCCESS;
	for (i = 0; i < ncfg; i++) {
		if (*n >= cap)
			return QNN_ERROR;
		if (!cfg[i].key)
			continue;
		keys[*n] = qnn_strdup_safe(cfg[i].key);
		vals[*n] = qnn_strdup_safe(cfg[i].value ? cfg[i].value : "");
		if (!keys[*n] || !vals[*n]) {
			free(keys[*n]);
			free(vals[*n]);
			keys[*n] = vals[*n] = NULL;
			return QNN_ERROR;
		}
		(*n)++;
	}
	return QNN_SUCCESS;
}

/* Rotating static buffers: getProperty returns const char* with no
   ownership transfer, matching the vendor API where the string stays valid
   until the next call on the same thread.  Eight slots keep a handful of
   concurrent getters independent. */
#define FMT_SLOTS 8
#define FMT_SLOT_SZ 128
static char fmt_slots[FMT_SLOTS][FMT_SLOT_SZ];
static unsigned fmt_next;

const char *qnn_fmt_static(const char *fmt, ...)
{
	char *slot = fmt_slots[fmt_next];
	va_list ap;

	fmt_next = (fmt_next + 1) % FMT_SLOTS;
	va_start(ap, fmt);
	vsnprintf(slot, FMT_SLOT_SZ, fmt, ap);
	va_end(ap);
	return slot;
}

uint64_t qnn_time_usec(void)
{
	uint64_t nsec = 0;

	/* Monotonic clock.  kernel_tic_nsec() reads the kernel-published
	   counter without a syscall; fall back to the (sec, usec) tick when
	   the vsyscall info page is not mapped yet. */
	if (kernel_tic_nsec(&nsec) == 0)
		return nsec / 1000ull;
	{
		uint32_t sec = 0;
		uint64_t usec = 0;

		if (kernel_tic(&sec, &usec) != 0)
			return 0;
		return (uint64_t)sec * 1000000ull + usec;
	}
}

uint64_t qnn_tensor_elements(const qnn_tensor_obj_t *t)
{
	uint64_t n = 1;
	uint32_t i;

	if (!t || t->rank == 0)
		return 0;
	for (i = 0; i < t->rank; i++)
		n *= t->dims[i];
	return n;
}

uint32_t qnn_dtype_size(Qnn_DataType_t dt)
{
	switch (dt) {
	case QNN_DATATYPE_UINT8:
	case QNN_DATATYPE_INT8:   return 1;
	case QNN_DATATYPE_UINT16:
	case QNN_DATATYPE_INT16:
	case QNN_DATATYPE_FLOAT16: return 2;
	case QNN_DATATYPE_UINT32:
	case QNN_DATATYPE_INT32:
	case QNN_DATATYPE_FLOAT32: return 4;
	default:                   return 0;
	}
}
