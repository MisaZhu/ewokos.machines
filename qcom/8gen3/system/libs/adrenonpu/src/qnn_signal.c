/*
 * qnn_signal.c - QnnSignal_* implementation.
 *
 * A signal is a lightweight completion token for the async entry points
 * (QnnGraph_executeAsync, QnnContext_createFromBinary*).  This port runs those
 * operations inline - there is no cDSP command queue to defer to - so a signal
 * is already marked by the time the launching call returns; wait() therefore
 * observes completion immediately.  The poll loop is kept genuine (bounded by
 * the caller's timeout) so the contract still holds if a future transport
 * defers the work to a worker.
 */
#include <adrenonpu/qnn_core_internal.h>
#include <ewoksys/proc.h>
#include <stdlib.h>

#define SIGNAL_POLL_NS 1000000ull   /* 1 ms */

void qnn_signal_mark(Qnn_SignalHandle_t sig, int state)
{
	qnn_signal_obj_t *s = (qnn_signal_obj_t *)
		qnn_obj_from_handle(sig, QNN_OBJ_SIGNAL);

	if (!s)
		return;
	s->state = state;
}

Qnn_ErrorHandle_t QnnSignal_create(Qnn_BackendHandle_t backend,
                                   Qnn_SignalHandle_t *sig)
{
	qnn_backend_obj_t *b = (qnn_backend_obj_t *)
		qnn_obj_from_handle(backend, QNN_OBJ_BACKEND);
	qnn_signal_obj_t *s;

	if (!b || !sig)
		return QNN_ERROR;
	s = (qnn_signal_obj_t *)qnn_obj_alloc(QNN_OBJ_SIGNAL, sizeof(*s));
	if (!s)
		return QNN_ERROR;
	s->state = 0;
	*sig = (Qnn_SignalHandle_t)(uintptr_t)s;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnSignal_wait(Qnn_SignalHandle_t sig, uint32_t timeoutMs)
{
	qnn_signal_obj_t *s = (qnn_signal_obj_t *)
		qnn_obj_from_handle(sig, QNN_OBJ_SIGNAL);
	uint32_t waited = 0;

	if (!s)
		return QNN_ERROR;
	for (;;) {
		if (s->state == 1)
			return QNN_SUCCESS;      /* done      */
		if (s->state == 2)
			return QNN_ERROR;        /* cancelled */
		if (timeoutMs != 0 && waited >= timeoutMs)
			return QNN_ERROR;        /* timed out */
		proc_nsleep_precise(SIGNAL_POLL_NS);
		waited++;                        /* ~1 ms per poll */
	}
}

Qnn_ErrorHandle_t QnnSignal_free(Qnn_SignalHandle_t sig)
{
	qnn_signal_obj_t *s = (qnn_signal_obj_t *)
		qnn_obj_from_handle(sig, QNN_OBJ_SIGNAL);

	if (!s)
		return QNN_ERROR;
	qnn_obj_free(s);
	return QNN_SUCCESS;
}
