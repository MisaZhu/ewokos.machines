#ifndef SHIM_EWOKSYS_KLOG_H
#define SHIM_EWOKSYS_KLOG_H
/* host shim: klog routes to the trace/printf shim in host_shim.c */
int klog(const char* fmt, ...);
#endif