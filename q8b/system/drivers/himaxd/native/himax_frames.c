#include "himax_frames.h"

int himax_frames_begin(himax_frames_t* f, himax_frame_read_t read,
                       himax_dev_t* dev, uint32_t addr, unsigned required) {
    if (!f || !read || !dev || required == 0)
        return -1;
    f->read = read;
    f->dev = dev;
    f->addr = addr;
    f->required = required;
    f->good = f->bad = 0;
    f->last = 0;
    f->stage = "begin";
    return 0;
}

int himax_frames_step(himax_frames_t* f) {
    if (!f || !f->read || !f->dev)
        return -1;
    uint32_t value = 0;
    f->stage = "read";
    if (f->read(f->dev, f->addr, &value) != 0) {
        f->stage = "transport-error";
        return -1;
    }
    /* A register on a firmware-answering IC must not read as all-zero or
     * all-ones; treat those as stale/bad frames.  Repeating the previous
     * value is NOT bad: many IC registers are stable, and the point of the
     * loop is transport stability, not value change detection. */
    if (value == 0x00000000u || value == 0xffffffffu) {
        f->bad++;
        f->stage = "bad-frame";
        if (f->bad >= f->required) {
            f->stage = "aborted";
            return -2;
        }
        return 0;
    }
    f->good++;
    f->last = value;
    f->bad = 0;
    f->stage = "good";
    return 1;
}