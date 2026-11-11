/* Host test for the continuous read-frame loop: validates good/bad frame
 * classification, stale detection, the abort-with-`required` rule, and
 * transport-error handling.  No hardware. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "himax_frames.h"

static unsigned calls, fail_at;
static uint32_t seq;
static int fake_read(himax_dev_t* dev, uint32_t addr, uint32_t* value) {
    (void)dev; (void)addr;
    calls++;
    if (fail_at && calls == fail_at)
        return -1;
    /* monotonic sequence so successive frames differ */
    *value = seq;
    return 0;
}

int main(void) {
    himax_dev_t dev;
    memset(&dev, 0, sizeof(dev));
    himax_frames_t f;
    seq = 0x100; calls = 0; fail_at = 0;

    assert(himax_frames_begin(&f, fake_read, &dev, 0x10078, 3) == 0);
    assert(himax_frames_step(&f) == 1 && f.stage && f.good == 1);
    seq = 0x100;                     /* same value is still a good frame */
    assert(himax_frames_step(&f) == 1 && f.bad == 0 && f.good == 2);
    seq = 0x100;
    assert(himax_frames_step(&f) == 1 && f.good == 3);

    /* stale all-zero / all-ones are bad even on first read */
    calls = 0; seq = 0;
    himax_frames_begin(&f, fake_read, &dev, 0, 2);
    assert(himax_frames_step(&f) == 0 && f.bad == 1);
    seq = 0xffffffffu;
    assert(himax_frames_step(&f) == -2 && f.bad == 2);  /* required=2 -> abort */
    /* 0x00/0xff stale still aborts; good resets the bad counter */
    seq = 0; calls = 0;
    himax_frames_begin(&f, fake_read, &dev, 0x2000, 5);
    assert(himax_frames_step(&f) == 0 && f.bad == 1);
    seq = 7;
    assert(himax_frames_step(&f) == 1 && f.bad == 0);
    seq = 7;
    assert(himax_frames_step(&f) == 1);
    /* transport error after two good frames (fresh call counter) */
    calls = 0; seq = 1; fail_at = 0;
    himax_frames_t f2;
    himax_frames_begin(&f2, fake_read, &dev, 0x2000, 5);
    assert(himax_frames_step(&f2) == 1);     /* calls=1 */
    seq = 2;
    assert(himax_frames_step(&f2) == 1);     /* calls=2 */
    fail_at = 3;
    assert(himax_frames_step(&f2) == -1 && strcmp(f2.stage, "transport-error") == 0);
    assert(calls == 3);

    assert(himax_frames_begin(NULL, fake_read, &dev, 0, 1) == -1);
    assert(himax_frames_begin(&f, NULL, &dev, 0, 1) == -1);
    assert(himax_frames_begin(&f, fake_read, NULL, 0, 1) == -1);

    puts("himax frames loop tests: ok");
    return 0;
}