/* Host test against the real g2dclient.c implementation. The sleep stub
 * escapes the permanent wait so no fault injection touches the board. */
#include <g2dclient/g2dclient.h>
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

/* The installed Ewok SDK uses newlib's assert ABI; use the host abort. */
void __assert_func(const char *file, int line, const char *func, const char *expr) {
    printf("%s:%d %s: %s\n",file,line,func,expr);
    abort();
}

static jmp_buf parked;
static int sleeps, logs;
void klog(const char *fmt, ...) { (void)fmt; logs++; }
int proc_nsleep_precise(uint64_t nsec) {
    assert(nsec == 1000000000);
    sleeps++;
    longjmp(parked,1);
}
int main(void) {
    g2d_quiesce_on_fault(G2D_OK);
    g2d_quiesce_on_fault(G2D_ERR_FAILED);
    g2d_quiesce_on_fault(G2D_ERR_NOT_SUPPORTED);
    assert(sleeps == 0 && logs == 0);
    if(setjmp(parked) == 0) {
        g2d_quiesce_on_fault(G2D_ERR_TIMEOUT);
        assert(!"submitted timeout returned to caller");
    }
    if(setjmp(parked) == 0) {
        g2d_quiesce_on_fault(G2D_ERR_MMU_FAULT);
        assert(!"GPU fault returned to caller");
    }
    assert(sleeps == 2 && logs == 2);
    puts("g2d fault policy: replayable declines return; terminal faults park PASS");
    return 0;
}
