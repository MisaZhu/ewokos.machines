/* host_glue.c - minimal host (macOS/glibc) glue for the vendored NIR subset.
 * Provides util_tls_qsort_r (the u_qsort.cpp fallback the frontend excludes).
 * The EwokOS device build gets this from pglcompiler_ewokos.c instead. */
#include <stddef.h>
#include <stdlib.h>

static _Thread_local int (*tls_compar)(const void*, const void*, void*);
static _Thread_local void* tls_arg;

static int tls_trampoline(const void* a, const void* b) {
    return tls_compar(a, b, tls_arg);
}

void util_tls_qsort_r(void* base, size_t nmemb, size_t size,
                      int (*compar)(const void*, const void*, void*), void* arg) {
    tls_compar = compar;
    tls_arg = arg;
    qsort(base, nmemb, size, tls_trampoline);
}