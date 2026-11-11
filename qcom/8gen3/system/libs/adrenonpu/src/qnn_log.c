/*
 * qnn_log.c - QnnLog_* implementation (qnn_core.h).
 *
 * One process-wide callback, like the vendor backend's QnnLog.  When no
 * callback is registered everything routes to klog (/dev/log ring buffer),
 * so daemon-side diagnostics keep working with the EwokOS slog capture
 * convention used by libadreno.
 */
#include <adrenonpu/qnn_core_internal.h>
#include <ewoksys/klog.h>
#include <stdarg.h>
#include <stdio.h>

static QnnLog_Callback_t g_cb;
static void *g_cb_data;

void qnn_log(uint32_t level, const char *fmt, ...)
{
	/* Callbacks may opt into protocol traces. The default system log stays
	 * free of per-request diagnostics. */
	if (!g_cb && level > QNN_LOG_LVL_INFO) return;
	char msg[256];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	if (g_cb) {
		g_cb(level, msg, g_cb_data);
		return;
	}
	slog("qnn[%u]: %s", level, msg);
}

Qnn_ErrorHandle_t QnnLog_initialize(QnnLog_Callback_t cb, void *userData)
{
	g_cb = cb;
	g_cb_data = userData;
	qnn_log(QNN_LOG_LVL_INFO, "adrenonpu log initialized\n");
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnLog_setCallback(QnnLog_Callback_t cb, void *userData)
{
	g_cb = cb;
	g_cb_data = userData;
	return QNN_SUCCESS;
}

Qnn_ErrorHandle_t QnnLog_free(void)
{
	g_cb = NULL;
	g_cb_data = NULL;
	return QNN_SUCCESS;
}
