#ifndef __WIFI_LOG_H__
#define __WIFI_LOG_H__

#include <ewoksys/klog.h>
#include <ewoksys/proc.h>
#include <ewoksys/kernel_tic.h>

#define usleep          proc_usleep
#define get_timer(x)    (kernel_tic_ms(0) - (x))

void log_init(void);
void wifi_log(const char *format, ...);
char* wifi_get_log(void);
#endif
