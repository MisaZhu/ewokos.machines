/* Host test for the multi-touch edge frames: edge derivation, the slot-reuse
 * lift, the overflow rule (a reader that falls behind must still end up with
 * the right contacts, through whole frames only, and never with one finger
 * taken for another), and the tracker glue the daemon uses.  No hardware. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "himax_mt.h"
#include "himax_mt_tracks.h"

#define N             HIMAX_MT_MAX_CONTACTS
#define REC(act, id)  (HIMAX_MT_FRAME | ((act) << 8) | (id))

static himax_mt_t mt;
static himax_mt_contact_t cur[N];
/* Which finger the test has put in each slot. */
static unsigned finger[N];
static unsigned last_finger;

/* What a client makes of the stream, following xtouch's framing: a header
 * opens a frame, the frame takes effect once its last record arrived. */
static himax_mt_contact_t seen[N];
static himax_mt_contact_t pending[N];
static unsigned seen_presses[N];
static unsigned owed;
static unsigned frames_seen;
/* The client's view the last time it had caught up. */
static bool sync_active[N];
static unsigned sync_finger[N];
static unsigned sync_presses[N];

static void set(unsigned id, uint16_t x, uint16_t y) {
    if (!cur[id].active)
        finger[id] = ++last_finger;
    cur[id].active = true;
    cur[id].x = x;
    cur[id].y = y;
}

/* Another finger takes the slot without a free frame in between. */
static void hand_over(unsigned id, uint16_t x, uint16_t y) {
    finger[id] = ++last_finger;
    cur[id].active = true;
    cur[id].fresh = true;
    cur[id].x = x;
    cur[id].y = y;
}

static void lift(unsigned id) {
    cur[id].active = false;
}

static unsigned step(void) {
    unsigned n = himax_mt_update(&mt, cur, NULL);

    for (unsigned id = 0; id < N; id++)
        cur[id].fresh = false;
    assert(mt.count <= HIMAX_MT_QUEUE_SIZE);
    return n;
}

static void expect(uint16_t state, uint16_t x, uint16_t y) {
    himax_mt_record_t rec;
    assert(himax_mt_pop(&mt, &rec));
    assert(rec.state == state && rec.x == x && rec.y == y);
}

static void client_read(unsigned max) {
    himax_mt_record_t rec;
    while (max-- != 0 && himax_mt_pop(&mt, &rec)) {
        unsigned id = rec.state & 0xffU;
        unsigned act = (rec.state >> 8) & 0x3U;

        if (rec.state == HIMAX_MT_FRAME) {
            assert(owed == 0);              /* never a torn frame */
            assert(rec.x >= 1 && rec.x <= N);
            owed = rec.x;
            memcpy(pending, seen, sizeof(pending));
            continue;
        }
        assert((rec.state & HIMAX_MT_FRAME) != 0 && owed != 0);
        assert(id < N);
        /* every edge must make sense against what the client holds */
        if (act == HIMAX_MT_ACT_PRESS) {
            assert(!seen[id].active);
            seen_presses[id]++;
        } else {
            assert(seen[id].active);
        }
        pending[id].active = act != HIMAX_MT_ACT_RELEASE;
        pending[id].x = rec.x;
        pending[id].y = rec.y;
        if (--owed == 0) {
            memcpy(seen, pending, sizeof(seen));
            frames_seen++;
        }
    }
}

/* The client has caught up: it holds the current contacts, and it was told
 * of every finger change - a slot that holds another finger than at the last
 * catch-up was pressed again (which the edge check above only allows after a
 * release), a slot that holds the same one was not. */
static void client_matches_cur(void) {
    assert(owed == 0 && mt.count == 0);
    for (unsigned id = 0; id < N; id++) {
        assert(seen[id].active == cur[id].active);
        if (cur[id].active) {
            assert(seen[id].x == cur[id].x && seen[id].y == cur[id].y);
            assert(mt.delivered[id].gen == mt.emitted[id].gen);
            if (sync_active[id] && sync_finger[id] == finger[id])
                assert(seen_presses[id] == sync_presses[id]);
            else
                assert(seen_presses[id] != sync_presses[id]);
        }
        sync_active[id] = cur[id].active;
        sync_finger[id] = finger[id];
        sync_presses[id] = seen_presses[id];
    }
}

static void reset(void) {
    himax_mt_init(&mt);
    memset(cur, 0, sizeof(cur));
    memset(seen, 0, sizeof(seen));
    memset(seen_presses, 0, sizeof(seen_presses));
    memset(sync_active, 0, sizeof(sync_active));
    memset(sync_presses, 0, sizeof(sync_presses));
    owed = 0;
    frames_seen = 0;
}

static uint32_t rnd_state;

static unsigned rnd(unsigned n) {
    rnd_state = rnd_state * 1664525U + 1013904223U;
    return (rnd_state >> 8) % n;
}

/* The tracker, configured the way the daemon configures it. */
static struct hx_algo algo;

static void tracker_reset(void) {
    reset();
    memset(&algo, 0, sizeof(algo));
    hx_algo_init_defaults(&algo);
    algo.debounce_base = 0;
    algo.track_start_debounce = 1;
    algo.track_lost_frames = 2;
}

static unsigned tracker_frame(struct input_mt_pos* det, int cnt) {
    himax_mt_contact_t now[N];

    hx_track_contacts(&algo, det, cnt);
    /* the daemon's start gate, which track_start_debounce 1 leaves open */
    algo.touch_active = hx_count_stable_tracks(&algo) > 0;
    himax_mt_from_tracks(algo.tracks, now, 2559, 1599);
    return himax_mt_update(&mt, now, NULL);
}

int main(void) {
    himax_mt_record_t rec;
    himax_mt_record_t queued[HIMAX_MT_UPDATE_MAX];
    struct input_mt_pos det[2];

    /* one finger: press, hold, move, release at the last position */
    reset();
    assert(step() == 0 && mt.count == 0);
    set(0, 100, 200);
    assert(himax_mt_update(&mt, cur, queued) == 2);
    assert(queued[0].state == HIMAX_MT_FRAME && queued[0].x == 1);
    assert(queued[1].state == REC(HIMAX_MT_ACT_PRESS, 0) &&
            queued[1].x == 100 && queued[1].y == 200);
    expect(HIMAX_MT_FRAME, 1, 0);
    expect(REC(HIMAX_MT_ACT_PRESS, 0), 100, 200);
    assert(step() == 0);                        /* parked: no edge */
    set(0, 101, 200);
    assert(step() == 2);
    expect(HIMAX_MT_FRAME, 1, 0);
    expect(REC(HIMAX_MT_ACT_MOVE, 0), 101, 200);
    lift(0);
    assert(step() == 2);
    expect(HIMAX_MT_FRAME, 1, 0);
    expect(REC(HIMAX_MT_ACT_RELEASE, 0), 101, 200);
    assert(!himax_mt_pop(&mt, &rec));
    assert(mt.presses == 1 && mt.releases == 1 && mt.drops == 0);

    /* several fingers share a frame; the slot index is the id */
    reset();
    set(2, 10, 20);
    set(7, 30, 40);
    assert(step() == 3);
    expect(HIMAX_MT_FRAME, 2, 0);
    expect(REC(HIMAX_MT_ACT_PRESS, 2), 10, 20);
    expect(REC(HIMAX_MT_ACT_PRESS, 7), 30, 40);
    set(7, 31, 41);                             /* only 7 moved */
    set(9, 50, 60);
    lift(2);
    assert(step() == 4);
    expect(HIMAX_MT_FRAME, 3, 0);
    expect(REC(HIMAX_MT_ACT_RELEASE, 2), 10, 20);
    expect(REC(HIMAX_MT_ACT_MOVE, 7), 31, 41);
    expect(REC(HIMAX_MT_ACT_PRESS, 9), 50, 60);
    /* NULL is "no contact": everything lifts */
    assert(himax_mt_update(&mt, NULL, NULL) == 3);
    expect(HIMAX_MT_FRAME, 2, 0);
    expect(REC(HIMAX_MT_ACT_RELEASE, 7), 31, 41);
    expect(REC(HIMAX_MT_ACT_RELEASE, 9), 50, 60);

    /* a slot handed to a new finger lifts first, in a frame of its own */
    reset();
    set(0, 100, 100);
    set(1, 900, 900);
    assert(step() == 3);
    client_read(~0U);
    hand_over(0, 2000, 1500);
    set(1, 901, 900);
    assert(step() == 5);
    expect(HIMAX_MT_FRAME, 2, 0);
    expect(REC(HIMAX_MT_ACT_RELEASE, 0), 100, 100);
    expect(REC(HIMAX_MT_ACT_MOVE, 1), 901, 900);
    expect(HIMAX_MT_FRAME, 1, 0);
    expect(REC(HIMAX_MT_ACT_PRESS, 0), 2000, 1500);
    /* fresh on a free slot is a plain press */
    hand_over(3, 5, 5);
    assert(step() == 2);
    expect(HIMAX_MT_FRAME, 1, 0);
    expect(REC(HIMAX_MT_ACT_PRESS, 3), 5, 5);

    /* a reader that never reads: the queue stays bounded, and once it does
     * read it lands on the current contacts, lifts included */
    reset();
    set(0, 0, 0);
    set(1, 500, 500);
    for (unsigned i = 1; i <= 200; i++) {
        set(0, (uint16_t)i, (uint16_t)i);
        if (i == 150)
            lift(1);
        step();
    }
    assert(mt.drops != 0);
    client_read(~0U);
    client_matches_cur();
    assert(seen[0].active && seen[0].x == 200 && !seen[1].active);

    /* the reader holds a finger, the slot goes to another one, and the lift
     * between them is dropped with the overflow: the reader must still see
     * the first finger end where it last saw it, then the second begin */
    reset();
    set(0, 100, 100);
    step();
    client_read(~0U);
    client_matches_cur();
    hand_over(0, 2000, 1500);
    assert(step() == 4);                        /* queued, never read */
    for (unsigned i = 1; mt.drops == 0; i++) {
        assert(i < 100);
        set(0, (uint16_t)(2000 + i), 1500);
        step();
    }
    assert(mt.count == 4);
    expect(HIMAX_MT_FRAME, 1, 0);
    expect(REC(HIMAX_MT_ACT_RELEASE, 0), 100, 100);
    expect(HIMAX_MT_FRAME, 1, 0);
    expect(REC(HIMAX_MT_ACT_PRESS, 0), cur[0].x, 1500);
    /* ... and from there on it is one finger again */
    set(0, 9, 9);
    assert(step() == 2);
    expect(HIMAX_MT_FRAME, 1, 0);
    expect(REC(HIMAX_MT_ACT_MOVE, 0), 9, 9);

    /* same when the first finger lifted and the slot was pressed again, the
     * release and the press both among the dropped frames */
    reset();
    set(0, 100, 100);
    step();
    client_read(~0U);
    client_matches_cur();
    lift(0);
    assert(step() == 2);
    set(0, 500, 500);
    assert(step() == 2);
    for (unsigned i = 1; mt.drops == 0; i++) {
        assert(i < 100);
        set(0, (uint16_t)(500 + i), 500);
        step();
    }
    assert(mt.count == 4);
    expect(HIMAX_MT_FRAME, 1, 0);
    expect(REC(HIMAX_MT_ACT_RELEASE, 0), 100, 100);
    expect(HIMAX_MT_FRAME, 1, 0);
    expect(REC(HIMAX_MT_ACT_PRESS, 0), cur[0].x, 500);

    /* the reader stopped in the middle of a frame: that frame is finished
     * before anything newer, and a slot it presses is lifted again when the
     * finger there has changed since */
    reset();
    set(0, 1, 1);
    set(1, 2, 2);
    set(2, 3, 3);
    step();
    client_read(2);                             /* header + first press */
    assert(owed == 2 && mt.head_left == 2);
    /* run on until an update overflows, so the queue is exactly a rebuild */
    for (unsigned i = 0, drops = 0; i < 200 || mt.drops == drops; i++) {
        assert(i < 400);
        drops = mt.drops;
        set(0, (uint16_t)(10 + i), 1);
        set(2, 3, (uint16_t)(10 + i));
        if (i == 100)
            lift(1);
        if (i == 120)
            set(1, 77, 88);                     /* back on the same slot */
        if (i == 150)
            hand_over(2, 900, 900);
        step();
    }
    assert(mt.drops != 0);
    expect(REC(HIMAX_MT_ACT_PRESS, 1), 2, 2);   /* the frame it was in */
    expect(REC(HIMAX_MT_ACT_PRESS, 2), 3, 3);
    expect(HIMAX_MT_FRAME, 3, 0);
    expect(REC(HIMAX_MT_ACT_MOVE, 0), cur[0].x, 1);
    expect(REC(HIMAX_MT_ACT_RELEASE, 1), 2, 2);
    expect(REC(HIMAX_MT_ACT_RELEASE, 2), 3, 3);
    expect(HIMAX_MT_FRAME, 2, 0);
    expect(REC(HIMAX_MT_ACT_PRESS, 1), 77, 88);
    expect(REC(HIMAX_MT_ACT_PRESS, 2), 3, cur[2].y);
    assert(!himax_mt_pop(&mt, &rec));

    /* a reader that keeps up, slowly: one record per update, long run */
    reset();
    for (unsigned i = 0; i < 2000; i++) {
        unsigned id = i % N;
        if ((i / 7U) % 3U == 0)
            lift(id);
        else
            set(id, (uint16_t)(i * 3U), (uint16_t)(i * 5U));
        step();
        client_read(1);
    }
    client_read(~0U);
    client_matches_cur();
    assert(frames_seen != 0);

    /* everything lifts through a full queue too */
    for (unsigned i = 0; i < 100; i++) {
        set(4, (uint16_t)i, 9);
        step();
    }
    memset(cur, 0, sizeof(cur));
    himax_mt_update(&mt, NULL, NULL);
    client_read(~0U);
    client_matches_cur();

    /* fingers coming, going and trading slots at random under a reader
     * that stalls, trickles and catches up */
    reset();
    rnd_state = 20261118U;
    {
        unsigned mode = 0;
        unsigned catchups = 0;

        for (unsigned i = 0; i < 200000; i++) {
            for (unsigned id = 0; id < N; id++) {
                unsigned r = rnd(id < 3 ? 16 : 64);
                uint16_t x = (uint16_t)rnd(2560);
                uint16_t y = (uint16_t)rnd(1600);

                if (!cur[id].active) {
                    if (r < 2)
                        set(id, x, y);
                } else if (r < 2) {
                    lift(id);
                } else if (r == 2) {
                    hand_over(id, x, y);
                } else if (r < 10) {
                    set(id, x, y);
                }
            }
            step();
            if (i % 37U == 0)
                mode = rnd(4);
            if (mode == 1)
                client_read(1);
            else if (mode == 2)
                client_read(rnd(8));
            else if (mode == 3 || i % 997U == 0)
                client_read(~0U);
            if (mt.count == 0 && owed == 0) {
                client_matches_cur();
                catchups++;
            }
        }
        client_read(~0U);
        client_matches_cur();
        assert(mt.drops > 100 && catchups > 100 && frames_seen > 1000);
    }

    /* tracker glue: a finger detected once stays one contact while the
     * tracker rides out its lost frames - one press, one release */
    tracker_reset();
    det[0].x = 1000;
    det[0].y = 800;
    assert(tracker_frame(det, 1) == 2);
    assert(algo.tracks[0].active && algo.tracks[0].age == 1);
    expect(HIMAX_MT_FRAME, 1, 0);
    expect(REC(HIMAX_MT_ACT_PRESS, 0), 1000, 800);
    assert(tracker_frame(det, 0) == 0);
    assert(algo.tracks[0].active && algo.tracks[0].age == 1);
    assert(tracker_frame(det, 0) == 0);
    assert(tracker_frame(det, 0) == 2);
    expect(HIMAX_MT_FRAME, 1, 0);
    expect(REC(HIMAX_MT_ACT_RELEASE, 0), 1000, 800);
    assert(mt.presses == 1 && mt.releases == 1);

    /* ... and is still that contact when it is detected again */
    tracker_reset();
    assert(tracker_frame(det, 1) == 2);
    assert(tracker_frame(det, 0) == 0);
    det[0].x = 1008;
    assert(tracker_frame(det, 1) == 2);
    expect(HIMAX_MT_FRAME, 1, 0);
    expect(REC(HIMAX_MT_ACT_PRESS, 0), 1000, 800);
    expect(HIMAX_MT_FRAME, 1, 0);
    assert(himax_mt_pop(&mt, &rec));
    assert(rec.state == REC(HIMAX_MT_ACT_MOVE, 0));

    /* the tracker gives the slot to a finger elsewhere in the frame the
     * first one expires: lift there, then press here */
    tracker_reset();
    det[0].x = 1000;
    det[0].y = 800;
    assert(tracker_frame(det, 1) == 2);
    client_read(~0U);
    assert(tracker_frame(det, 0) == 0);
    assert(tracker_frame(det, 0) == 0);
    det[0].x = 2400;
    det[0].y = 1500;
    assert(tracker_frame(det, 1) == 4);
    assert(algo.tracks[0].active && !algo.tracks[1].active);
    expect(HIMAX_MT_FRAME, 1, 0);
    expect(REC(HIMAX_MT_ACT_RELEASE, 0), 1000, 800);
    expect(HIMAX_MT_FRAME, 1, 0);
    expect(REC(HIMAX_MT_ACT_PRESS, 0), 2400, 1500);

    puts("himax_mt_test: all checks passed");
    return 0;
}
