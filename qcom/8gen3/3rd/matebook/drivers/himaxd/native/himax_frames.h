#ifndef HIMAX_FRAMES_H
#define HIMAX_FRAMES_H
#include <stdint.h>
#include "himax_protocol.h"

/*
 * Continuous read-frame loop for the native-SPI bring-up probe (host tests
 * only; the loop logic is testable without hardware).  Repeats a register
 * read N times and validates each result: a frame is "good" when the new
 * value differs from an all-0xff/stale pattern OR matches a caller check.
 * Returns the number of good frames (-1 when the first read fails).
 */
typedef int (*himax_frame_read_t)(himax_dev_t* dev, uint32_t addr,
                                  uint32_t* value);

typedef struct {
    himax_frame_read_t read;
    himax_dev_t* dev;
    uint32_t addr;
    unsigned required;       /* consecutive bad frames to abort */
    unsigned good, bad;
    uint32_t last;
    const char* stage;
} himax_frames_t;

int himax_frames_begin(himax_frames_t* f, himax_frame_read_t read,
                       himax_dev_t* dev, uint32_t addr, unsigned required);
/* Run one read: value must be non-stale (not 0x00000000/0xffffffff) and
 * either change or pass the sequence check.  Returns 1 (good), 0 (bad,
 * count toward required), -2 (aborted), -1 (transport error). */
int himax_frames_step(himax_frames_t* f);
#endif