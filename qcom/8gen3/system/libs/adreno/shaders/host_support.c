#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>

const char *util_get_process_name(void) {
    return "q8b-builtin-shaders";
}

void slog(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

/* Same portable qsort adapter as the device frontend, without replacing
 * native libc's memstream and temporary-file functions. */
static __thread int (*compare_fn)(const void *, const void *, void *);
static __thread void *compare_arg;
static int compare(const void *a, const void *b) {
    return compare_fn(a, b, compare_arg);
}
void util_tls_qsort_r(void *base, size_t count, size_t size,
        int (*fn)(const void *, const void *, void *), void *arg) {
    compare_fn = fn;
    compare_arg = arg;
    qsort(base, count, size, compare);
}
#if defined(__APPLE__)
int pthread_getcpuclockid(pthread_t thread, clockid_t *clock_id) {
    (void)thread;
    if (!clock_id) return EINVAL;
    *clock_id = CLOCK_MONOTONIC;
    return 0;
}
#endif
