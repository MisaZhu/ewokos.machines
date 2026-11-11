/*
 * Host test for the ported HX83121A solver.
 *
 * Synthetic capacitance frames are injected into the ported pipeline and the
 * resulting contact positions/tracking are checked.  This validates the port
 * (types, helpers, grid maths) but cannot replace a real panel.
 */
#include <stdio.h>
#include <string.h>

#include "hx_algo.h"

#define BASELINE 0x7ffe

static struct hx_algo _algo;
static uint16_t _raw[HX_PIXELS];
static struct input_mt_pos _pos[HIMAX_MAX_TOUCH];
static int _failures;

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            _failures++; \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

static void frame_clear(void) {
    for (int i = 0; i < HX_PIXELS; i++)
        _raw[i] = BASELINE;
}

/* Raised 3x3 bump with a strict local maximum at (r,c). */
static void frame_bump(int r, int c, int amplitude) {
    for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
            int rr = r + dr, cc = c + dc;
            int weight;
            if (rr < 0 || rr >= HX_ROWS || cc < 0 || cc >= HX_COLS)
                continue;
            if (dr == 0 && dc == 0)
                weight = 4;
            else if (dr == 0 || dc == 0)
                weight = 2;
            else
                weight = 1;
            _raw[rr * HX_COLS + cc] =
                    (uint16_t)(BASELINE + amplitude * weight / 4);
        }
    }
}

static void frame_blob(int r0, int c0, int rows, int cols, int amplitude) {
    for (int r = r0; r < r0 + rows && r < HX_ROWS; r++)
        for (int c = c0; c < c0 + cols && c < HX_COLS; c++)
            _raw[r * HX_COLS + c] = (uint16_t)(BASELINE + amplitude);
}

/* One full pipeline pass, mirroring himaxd's process_frame(). */
static int solve(int* out_count) {
    hx_preprocess_frame(&_algo, _raw);
    hx_detect_macro_zones(&_algo);
    hx_reject_palms(&_algo);
    hx_detect_peaks(&_algo);
    hx_expand_and_resolve(&_algo, _pos, out_count);
    hx_track_contacts(&_algo, _pos, *out_count);
    return hx_count_stable_tracks(&_algo);
}

static void algo_reset(void) {
    memset(&_algo, 0, sizeof(_algo));
    hx_algo_init_defaults(&_algo);
    frame_clear();
}

/* Feed identical frames until the debounce of new contacts expires. */
static int settle(int frames) {
    int count = 0;
    int stable = 0;

    for (int i = 0; i < frames; i++) {
        frame_clear();
        stable = solve(&count);
    }
    return stable;
}

static void test_single_touch_position(void) {
    int count = 0;
    int stable;

    algo_reset();
    for (int i = 0; i < 4; i++) {
        frame_clear();
        frame_bump(20, 30, 3000);
        stable = solve(&count);
    }
    CHECK(count == 1, "contacts %d", count);
    CHECK(stable == 1, "stable tracks %d", stable);
    /* Column 30 of 60 maps to 2560 px: 256*30/6 = 1280 + 21 px offset. */
    CHECK(_algo.tracks[0].x > 1270 && _algo.tracks[0].x < 1330,
            "x %d", _algo.tracks[0].x);
    /* Row 20 of 40 maps to 1600 px: 5*256*20/32 = 800 + 20 px offset. */
    CHECK(_algo.tracks[0].y > 800 && _algo.tracks[0].y < 850,
            "y %d", _algo.tracks[0].y);
}

static void test_tracking_follows_motion(void) {
    int count = 0;
    int x0, x1;

    algo_reset();
    for (int i = 0; i < 4; i++) {
        frame_clear();
        frame_bump(20, 30, 3000);
        solve(&count);
    }
    x0 = _algo.tracks[0].x;
    for (int i = 0; i < 4; i++) {
        frame_clear();
        frame_bump(20, 33, 3000);
        solve(&count);
    }
    x1 = _algo.tracks[0].x;
    CHECK(count == 1, "contacts %d", count);
    CHECK(x1 > x0 + 60, "track did not move right: %d -> %d", x0, x1);
    CHECK(_algo.tracks[0].age >= 8, "track re-created, age %u",
            _algo.tracks[0].age);
}

static void test_release_after_empty_frames(void) {
    int count = 0;

    algo_reset();
    for (int i = 0; i < 4; i++) {
        frame_clear();
        frame_bump(10, 10, 3000);
        solve(&count);
    }
    CHECK(hx_count_stable_tracks(&_algo) == 1, "touch not established");
    for (int i = 0; i < 6; i++)
        settle(1);
    CHECK(hx_count_stable_tracks(&_algo) == 0, "touch not released");
}

static void test_palm_rejected(void) {
    int count = 0;

    algo_reset();
    for (int i = 0; i < 4; i++) {
        frame_clear();
        frame_blob(5, 5, 15, 30, 2000);
        solve(&count);
    }
    CHECK(count == 0, "palm produced %d contacts", count);
    CHECK(_algo.peak_count == 0, "palm produced %u peaks", _algo.peak_count);
}

static void test_two_fingers(void) {
    int count = 0;

    algo_reset();
    for (int i = 0; i < 4; i++) {
        frame_clear();
        frame_bump(10, 15, 3000);
        frame_bump(30, 45, 2600);
        solve(&count);
    }
    CHECK(count == 2, "contacts %d", count);
    CHECK(hx_count_stable_tracks(&_algo) == 2, "stable %d",
            hx_count_stable_tracks(&_algo));
}

static void test_baseline_invalid_pixel(void) {
    int count = 0;

    algo_reset();
    /* Pixel [0][0] is invalid on this panel and must be zeroed. */
    _raw[0] = BASELINE + 9000;
    hx_preprocess_frame(&_algo, _raw);
    CHECK(_algo.frame[0][0] == 0, "frame[0][0] %d", _algo.frame[0][0]);
    (void)count;
}

static void test_silence_is_quiet(void) {
    int count = 0;

    algo_reset();
    for (int i = 0; i < 10; i++) {
        frame_clear();
        solve(&count);
    }
    CHECK(count == 0, "silent frame produced %d contacts", count);
    CHECK(_algo.peak_count == 0, "silent frame produced peaks");
}

int main(void) {
    test_single_touch_position();
    test_tracking_follows_motion();
    test_release_after_empty_frames();
    test_palm_rejected();
    test_two_fingers();
    test_baseline_invalid_pixel();
    test_silence_is_quiet();
    if (_failures) {
        printf("%d failure(s)\n", _failures);
        return 1;
    }
    printf("hx_algo_test: all checks passed\n");
    return 0;
}
