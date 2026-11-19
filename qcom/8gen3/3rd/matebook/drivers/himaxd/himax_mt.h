#ifndef HIMAX_MT_H
#define HIMAX_MT_H
#include <stdbool.h>
#include <stdint.h>

/*
 * Multi-touch edge frames for /dev/touch0 (no hardware, host-testable).
 *
 * The record stream is the one xtouch already reads from the TW220 goodixd:
 * three uint16_t fields per record, a header followed by one record per
 * contact edge of the frame.
 *
 *   state HIMAX_MT_FRAME ........ header: x = record count, y = 0
 *   state HIMAX_MT_FRAME | (act << 8) | id ... contact record, act is
 *     HIMAX_MT_ACT_PRESS/MOVE/RELEASE, id 0..9; x/y are solver coordinates
 *     (a release carries the contact's last known position)
 *
 * Only edges are sent: a contact that did not move produces no record, and a
 * frame with no edge at all produces nothing.  Gesture policy and
 * single-finger mouse compatibility live in xtouch/xserverd.
 *
 * Nothing here locks.  himax_mt_update() and himax_mt_pop() must not run
 * inside one another: the caller serialises them (in the daemon the pop is an
 * IPC handler that can cut into the sampling loop anywhere).
 */
#define HIMAX_MT_FRAME          0x8000U
#define HIMAX_MT_ACT_PRESS      1U
#define HIMAX_MT_ACT_MOVE       2U
#define HIMAX_MT_ACT_RELEASE    3U
#define HIMAX_MT_MAX_CONTACTS   10U
/* The depth the single-contact queue had for one finger (16 positions, now a
 * header and a record each). */
#define HIMAX_MT_QUEUE_SIZE     32U
/* Most records one update queues: a lift frame and the frame proper. */
#define HIMAX_MT_UPDATE_MAX     (2U * (1U + HIMAX_MT_MAX_CONTACTS))

/* An update must fit behind the rest of a frame the reader is half through. */
#if HIMAX_MT_QUEUE_SIZE < HIMAX_MT_UPDATE_MAX + HIMAX_MT_MAX_CONTACTS
#error "HIMAX_MT_QUEUE_SIZE cannot hold a half-read frame plus one update"
#endif

typedef struct {
    uint16_t state;
    uint16_t x;
    uint16_t y;
} himax_mt_record_t;

/* One tracker slot as the caller sees it; the slot index is the contact id. */
typedef struct {
    bool active;
    /* The slot holds a contact that started this frame, even if the slot
     * was in use the frame before.  Set it for that one frame only. */
    bool fresh;
    uint16_t x;
    uint16_t y;
} himax_mt_contact_t;

/* A slot as the stream knows it.  `gen` names the contact: every press gets
 * a new one, so two fingers that used the same slot are never taken for one,
 * however many frames between them were dropped. */
typedef struct {
    bool active;
    unsigned gen;
    uint16_t x;
    uint16_t y;
} himax_mt_slot_t;

typedef struct {
    himax_mt_record_t rec;
    unsigned gen;
} himax_mt_entry_t;

typedef struct {
    himax_mt_entry_t queue[HIMAX_MT_QUEUE_SIZE];
    unsigned count;
    /* Records of the frame at the head of the queue whose header the reader
     * already took; 0 when the queue starts on a frame boundary. */
    unsigned head_left;
    /* Contact set after every queued frame / after the frames read so far. */
    himax_mt_slot_t emitted[HIMAX_MT_MAX_CONTACTS];
    himax_mt_slot_t delivered[HIMAX_MT_MAX_CONTACTS];
    unsigned last_gen;
    unsigned presses;
    unsigned releases;
    unsigned drops;
} himax_mt_t;

void himax_mt_init(himax_mt_t* mt);
/* Queue the frames that take the reader to `cur` (HIMAX_MT_MAX_CONTACTS
 * slots, NULL for no contact).  Returns the number of records queued and
 * copies them to `queued` (room for HIMAX_MT_UPDATE_MAX) unless it is NULL. */
unsigned himax_mt_update(himax_mt_t* mt, const himax_mt_contact_t* cur,
        himax_mt_record_t* queued);
/* Take the oldest record; false when the queue is empty. */
bool himax_mt_pop(himax_mt_t* mt, himax_mt_record_t* out);
#endif
