#ifndef Q8B_GOODIX_GESTURE_H
#define Q8B_GOODIX_GESTURE_H

#include <stddef.h>
#include <stdint.h>

/* Gesture state machine over Goodix Berlin contact frames.
 *
 * Berlin event frames carry exactly the currently active contacts (the
 * mainline driver reports every frame entry as an active slot and lets
 * input_mt_sync_frame release the absent ones), so a contact that stops
 * appearing has been lifted.  The caller feeds one frame per touch event
 * and a periodic tick for timeouts; recognized gestures are returned
 * through the emit callback as button/scroll events in raw panel
 * coordinates. */

#define GESTURE_MAX_TOUCH 10U

/* Single finger is held this long before LEFT down is committed (a second
 * finger landing inside the window turns the press into a two-finger
 * gesture instead of a left click). */
#define GESTURE_PENDING_MS 100U

/* Raw-panel pixels.  A single finger past this commits a drag; a two-finger
 * midpoint displaced more than this cancels the right click. */
#define GESTURE_TAP_MOVE_PX 50U

/* Midpoint travel that produces one wheel notch.  Wheel events carry no
 * magnitude, so a swipe is quantized into notches at this granularity. */
#define GESTURE_SCROLL_NOTCH_PX 25U

typedef enum {
    GESTURE_EVT_NONE = 0,
    GESTURE_EVT_LEFT_DOWN,
    GESTURE_EVT_LEFT_UP,
    GESTURE_EVT_RIGHT_DOWN,
    GESTURE_EVT_RIGHT_UP,
    GESTURE_EVT_SCROLL_UP,
    GESTURE_EVT_SCROLL_DOWN,
} gesture_evt_t;

typedef struct {
    uint8_t id; /* touch id: (status >> 4), must be < GESTURE_MAX_TOUCH */
    uint16_t x;
    uint16_t y;
} gesture_contact_t;

typedef struct {
    uint8_t count;
    gesture_contact_t contacts[GESTURE_MAX_TOUCH];
} gesture_frame_t;

typedef void (*gesture_emit_fn)(gesture_evt_t evt, uint16_t x, uint16_t y);

void gesture_reset(void);
void gesture_process(const gesture_frame_t* frame, uint64_t now_ms,
        gesture_emit_fn emit);
void gesture_tick(uint64_t now_ms, gesture_emit_fn emit);

#endif
