/* Host regression: observed SCM64=yes/PAS13=no must not prevent Linux's
 * normal image-authentication path. Probe never invokes a mutating call.
 * cc -std=c11 -Wall -Wextra -Werror -I machines/q8b/include \
 *   machines/q8b/test/scm_probe_test.c -o /tmp/q8b-scm-probe-test
 */
#include <assert.h>
#include <stdio.h>
#include "../kernel/bsp/scm-probe.h"
static int mode, count, supported_query;
static int fake_call(uint32_t fn, uint32_t info, uint64_t a, uint64_t b,
        uint64_t c, int fast, uint64_t *result) {
    count++;
    assert(info == 1 && b == 0 && c == 0);
    assert(fn == 0x601 || fn == 0x207); /* read-only calls only */
    assert(fast == (count == 1));
    *result = 0;
    if(count == 1) {
        assert(fn == 0x601 && a == 0x02000601);
        if(mode == 1) return 0; /* no SCM64 convention */
        if(mode == 2) return Q8B_GPU_PAS_BUSY;
        *result = 1;
    } else if(fn == 0x601 && a == 0x02000207) {
        *result = mode == 3 ? 0 : 1;
    } else if(fn == 0x207) {
        assert(a == 13);
        supported_query++;
        *result = 0; /* exact board observation, advisory */
    } else {
        assert(a == 0x02000201 || a == 0x02000202 || a == 0x02000205);
        /* Even an unavailable advertisement is not authentication's result. */
        *result = 0;
    }
    return 0;
}
int main(void) {
    assert(q8b_scm_probe(fake_call) == 0);
    assert(count == 6 && supported_query == 1);
    mode = 1; count = 0;
    assert(q8b_scm_probe(fake_call) == Q8B_GPU_PAS_UNSUPPORTED && count == 1);
    mode = 2; count = 0;
    assert(q8b_scm_probe(fake_call) == Q8B_GPU_PAS_BUSY && count == 1);
    mode = 3; count = supported_query = 0;
    assert(q8b_scm_probe(fake_call) == 0);
    assert(count == 5 && supported_query == 0);
    puts("Q8B SCM probe: advisory PAS results, missing transport and BUSY PASS");
    return 0;
}
