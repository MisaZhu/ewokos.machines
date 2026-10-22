/* Read-only capability diagnostics. Linux's Adreno path requires an SCM
 * transport, but does not gate image loading on PAS advertisement queries. */
#ifndef Q8B_SCM_PROBE_H
#define Q8B_SCM_PROBE_H
#include <stdint.h>
#include <q8b_gpu_pas.h>
typedef int (*q8b_scm_call_t)(uint32_t, uint32_t, uint64_t, uint64_t,
        uint64_t, int, uint64_t *);
static inline int q8b_scm_probe(q8b_scm_call_t call) {
    uint64_t result = 0;
    int ret;
    const uint32_t calls[] = {0x201, 0x202, 0x205};
    ret = call(0x601, 1, 0x02000601, 0, 0, 1, &result);
    if(ret) return ret;
    if(result != 1) return Q8B_GPU_PAS_UNSUPPORTED;
    ret = call(0x601, 1, 0x02000207, 0, 0, 0, &result);
    if(ret) return ret;
    if(result == 1) {
        ret = call(0x207, 1, 13, 0, 0, 0, &result);
        if(ret) return ret;
        /* The raw advertised result is logged by call(), not a prerequisite. */
    }
    for(unsigned i = 0; i < sizeof(calls)/sizeof(calls[0]); i++) {
        ret = call(0x601, 1, 0x02000000u | calls[i], 0, 0, 0, &result);
        if(ret) return ret;
    }
    return 0;
}
#endif
