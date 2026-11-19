#ifndef HIMAX_MT_TRACKS_H
#define HIMAX_MT_TRACKS_H
#include <string.h>

#include "himax_mt.h"
#include "hx_algo.h"

/* The tracker slot index is the contact id on the wire. */
#if HIMAX_MT_MAX_CONTACTS != HIMAX_MAX_TOUCH
#error "himax_mt contact ids must cover every tracker slot"
#endif

/*
 * The stable tracks of this frame as himax_mt contacts, clamped to
 * 0..x_max / 0..y_max.  Returns how many there are.  Kept apart from the
 * daemon so the host tests run the tracker through the same glue.
 */
static inline unsigned himax_mt_from_tracks(const struct hx_track* tracks,
        himax_mt_contact_t* cur, unsigned x_max, unsigned y_max) {
    unsigned n = 0;

    memset(cur, 0, sizeof(cur[0]) * HIMAX_MT_MAX_CONTACTS);
    for (unsigned i = 0; i < HIMAX_MT_MAX_CONTACTS; i++) {
        const struct hx_track* trk = &tracks[i];

        if (!trk->active || trk->debounce != 0)
            continue;
        cur[i].active = true;
        /*
         * A track created this frame is a new finger, even when the slot was
         * another finger's until now.  Age alone does not say so: age only
         * advances on a matched frame, so a track that was detected once and
         * is riding out its lost-frame allowance still has age 1.
         */
        cur[i].fresh = trk->age == 1 && trk->missed == 0;
        cur[i].x = (uint16_t)(trk->x < 0 ? 0 :
                (unsigned)trk->x > x_max ? x_max : (unsigned)trk->x);
        cur[i].y = (uint16_t)(trk->y < 0 ? 0 :
                (unsigned)trk->y > y_max ? y_max : (unsigned)trk->y);
        n++;
    }
    return n;
}
#endif
