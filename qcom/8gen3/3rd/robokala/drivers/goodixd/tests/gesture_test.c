#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../goodix_gesture.h"

typedef struct {
    gesture_evt_t evt;
    uint16_t x;
    uint16_t y;
} evt_rec_t;

static evt_rec_t _recs[64];
static unsigned _rec_count;

static void collect(gesture_evt_t evt, uint16_t x, uint16_t y) {
    assert(_rec_count < sizeof(_recs) / sizeof(_recs[0]));
    _recs[_rec_count].evt = evt;
    _recs[_rec_count].x = x;
    _recs[_rec_count].y = y;
    _rec_count++;
}

static void begin(void) {
    gesture_reset();
    _rec_count = 0;
}

static void expect(unsigned idx, gesture_evt_t evt, uint16_t x, uint16_t y) {
    assert(idx < _rec_count);
    assert(_recs[idx].evt == evt);
    assert(_recs[idx].x == x);
    assert(_recs[idx].y == y);
}

static void expect_count(unsigned n) {
    assert(_rec_count == n);
}

static gesture_frame_t frame1(uint8_t id, uint16_t x, uint16_t y) {
    gesture_frame_t f;
    memset(&f, 0, sizeof(f));
    f.count = 1;
    f.contacts[0].id = id;
    f.contacts[0].x = x;
    f.contacts[0].y = y;
    return f;
}

static gesture_frame_t frame2(uint8_t id0, uint16_t x0, uint16_t y0,
        uint8_t id1, uint16_t x1, uint16_t y1) {
    gesture_frame_t f;
    memset(&f, 0, sizeof(f));
    f.count = 2;
    f.contacts[0].id = id0;
    f.contacts[0].x = x0;
    f.contacts[0].y = y0;
    f.contacts[1].id = id1;
    f.contacts[1].x = x1;
    f.contacts[1].y = y1;
    return f;
}

int main(void) {
    gesture_frame_t f, empty;

    memset(&empty, 0, sizeof(empty));

    /* 1. single-finger quick tap is a left click at the press point */
    begin();
    f = frame1(0, 100, 100);
    gesture_process(&f, 0, collect);
    expect_count(0);
    gesture_process(&empty, 50, collect);
    expect_count(2);
    expect(0, GESTURE_EVT_LEFT_DOWN, 100, 100);
    expect(1, GESTURE_EVT_LEFT_UP, 100, 100);

    /* 2. pending hold commits LEFT down at the timeout, lift releases */
    begin();
    f = frame1(0, 100, 100);
    gesture_process(&f, 0, collect);
    gesture_tick(99, collect);
    expect_count(0);
    gesture_tick(100, collect);
    expect_count(1);
    expect(0, GESTURE_EVT_LEFT_DOWN, 100, 100);
    gesture_process(&empty, 120, collect);
    expect_count(2);
    expect(1, GESTURE_EVT_LEFT_UP, 100, 100);

    /* 3. movement past the tap radius commits a drag before the timeout */
    begin();
    f = frame1(0, 100, 100);
    gesture_process(&f, 0, collect);
    f = frame1(0, 160, 100);
    gesture_process(&f, 10, collect);
    expect_count(1);
    expect(0, GESTURE_EVT_LEFT_DOWN, 160, 100);
    f = frame1(0, 200, 100);
    gesture_process(&f, 20, collect);
    expect_count(2);
    expect(1, GESTURE_EVT_LEFT_DOWN, 200, 100);
    gesture_process(&empty, 30, collect);
    expect_count(3);
    expect(2, GESTURE_EVT_LEFT_UP, 200, 100);

    /* 4. a second finger inside the pending window cancels the left
       press: two-finger tap becomes a right click at the midpoint */
    begin();
    f = frame1(0, 100, 100);
    gesture_process(&f, 0, collect);
    f = frame2(0, 100, 100, 1, 300, 200);
    gesture_process(&f, 30, collect);
    expect_count(0);
    gesture_process(&empty, 60, collect);
    expect_count(2);
    expect(0, GESTURE_EVT_RIGHT_DOWN, 200, 150);
    expect(1, GESTURE_EVT_RIGHT_UP, 200, 150);

    /* 5. two-finger swipe down quantizes into SCROLL_DOWN notches and
       must not produce a right click */
    begin();
    f = frame2(0, 100, 100, 1, 300, 100);
    gesture_process(&f, 0, collect);
    f = frame2(0, 100, 150, 1, 300, 150);
    gesture_process(&f, 20, collect);
    expect_count(2); /* 50 px = 2 notches */
    expect(0, GESTURE_EVT_SCROLL_DOWN, 200, 150);
    expect(1, GESTURE_EVT_SCROLL_DOWN, 200, 150);
    f = frame1(0, 100, 150); /* first finger lifts, one remains */
    gesture_process(&f, 30, collect);
    expect_count(2); /* scrolled: no right click */
    gesture_process(&empty, 40, collect);
    expect_count(2);

    /* 6. two-finger swipe up emits SCROLL_UP notches */
    begin();
    f = frame2(0, 100, 300, 1, 300, 300);
    gesture_process(&f, 0, collect);
    f = frame2(0, 100, 225, 1, 300, 225);
    gesture_process(&f, 20, collect);
    expect_count(3); /* 75 px = 3 notches */
    expect(0, GESTURE_EVT_SCROLL_UP, 200, 225);
    expect(2, GESTURE_EVT_SCROLL_UP, 200, 225);
    gesture_process(&empty, 40, collect);
    expect_count(3);

    /* 7. a still two-finger press with a slight wiggle is a right click */
    begin();
    f = frame2(0, 100, 100, 1, 300, 100);
    gesture_process(&f, 0, collect);
    f = frame2(0, 110, 100, 1, 310, 100);
    gesture_process(&f, 20, collect);
    expect_count(0); /* 10 px: below notch and tap thresholds */
    gesture_process(&empty, 40, collect);
    expect_count(2);
    expect(0, GESTURE_EVT_RIGHT_DOWN, 210, 100);
    expect(1, GESTURE_EVT_RIGHT_UP, 210, 100);

    /* 8. two-finger horizontal travel past the tap radius cancels the
       right click without emitting scroll */
    begin();
    f = frame2(0, 100, 100, 1, 300, 100);
    gesture_process(&f, 0, collect);
    f = frame2(0, 160, 100, 1, 360, 100);
    gesture_process(&f, 20, collect);
    expect_count(0);
    gesture_process(&empty, 40, collect);
    expect_count(0);

    /* 9. a finger switch re-anchors instead of jumping the midpoint */
    begin();
    f = frame2(0, 100, 100, 1, 300, 100);
    gesture_process(&f, 0, collect);
    f = frame2(1, 300, 100, 2, 100, 100); /* ids swapped: same midpoint */
    gesture_process(&f, 20, collect);
    expect_count(0);
    f = frame2(1, 300, 125, 2, 100, 125);
    gesture_process(&f, 30, collect);
    expect_count(1);
    expect(0, GESTURE_EVT_SCROLL_DOWN, 200, 125);

    /* 9b. a finger switch after scrolling must not re-arm the right click */
    begin();
    f = frame2(0, 100, 100, 1, 300, 100);
    gesture_process(&f, 0, collect);
    f = frame2(0, 100, 150, 1, 300, 150);
    gesture_process(&f, 20, collect);
    expect_count(2); /* scrolled */
    f = frame2(1, 300, 150, 2, 100, 150); /* finger walk, same midpoint */
    gesture_process(&f, 30, collect);
    expect_count(2); /* re-anchor emits nothing */
    gesture_process(&empty, 40, collect);
    expect_count(2); /* scrolled: no right click */

    /* 10. a committed left drag interrupted by a second finger releases
       left and hands the gesture to the two-finger path */
    begin();
    f = frame1(0, 100, 100);
    gesture_process(&f, 0, collect);
    f = frame1(0, 200, 100); /* drag: commits LEFT down */
    gesture_process(&f, 10, collect);
    expect_count(1);
    f = frame2(0, 200, 100, 1, 300, 200);
    gesture_process(&f, 20, collect);
    expect_count(2);
    expect(1, GESTURE_EVT_LEFT_UP, 200, 100);
    gesture_process(&empty, 40, collect);
    expect_count(4);
    expect(2, GESTURE_EVT_RIGHT_DOWN, 250, 150);
    expect(3, GESTURE_EVT_RIGHT_UP, 250, 150);

    /* 11. the finger left after a gesture is suppressed until every
       finger lifts; the next tap works normally */
    begin();
    f = frame2(0, 100, 100, 1, 300, 100);
    gesture_process(&f, 0, collect);
    f = frame1(0, 100, 100);
    gesture_process(&f, 20, collect);
    expect_count(2); /* right click emitted at gesture end */
    f = frame1(0, 120, 100);
    gesture_process(&f, 30, collect);
    expect_count(2); /* leftover contact suppressed */
    gesture_process(&empty, 40, collect);
    expect_count(2);
    f = frame1(2, 500, 500);
    gesture_process(&f, 1000, collect);
    expect_count(2);
    gesture_process(&empty, 1010, collect);
    expect_count(4);
    expect(2, GESTURE_EVT_LEFT_DOWN, 500, 500);
    expect(3, GESTURE_EVT_LEFT_UP, 500, 500);

    puts("goodix gesture tests: ok");
    return 0;
}
