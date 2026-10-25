/* Use native libc headers before the device config's SDK compatibility macros. */
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <time.h>
#include "../../adrenocompiler/port/mesa_config.h"
#if defined(__clang__)
#undef HAVE___BUILTIN_ADD_OVERFLOW_P
#undef HAVE___BUILTIN_SUB_OVERFLOW_P
#endif
#if defined(__APPLE__)
#ifdef __cplusplus
extern "C" {
#endif
int pthread_getcpuclockid(pthread_t thread, clockid_t *clock_id);
#ifdef __cplusplus
}
#endif
#endif
