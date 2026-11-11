/* Shared RPMh vote ledger, with no MMIO or syscall dependencies.
 *
 * One central owner must serialize ALL users of a resource and hold its lock
 * through submit(). An idle TCS is not a resource ownership mechanism. Before
 * initialization, the owner must establish a known baseline and account for
 * existing clients. Do not initialize this over an unknown firmware vote.
 *
 * Encoded BCM average requests are summed across clients, while peak is max.
 * This can overvote disjoint nodes of one BCM; rounding may also differ from
 * Linux. Exact aggregation requires per-node bandwidth accounting before
 * conversion. This ledger alone is not a general interconnect provider.
 * ACV enable masks use OR, and ARC corner indices use max. Caller exit never
 * implicitly releases a vote; release is allowed only after device quiescence.
 * A failed submit freezes the ledger until external hardware reconciliation.
 * Zero-initialize a ledger before its first init; reinitialization is refused.
 * This candidate is host-tested only; it is not a deployed resource broker.
 */
#ifndef Q8B_RPMH_VOTES_H
#define Q8B_RPMH_VOTES_H
#include <stdint.h>

#define Q8B_RPMH_MAX_VOTERS 8u
#define Q8B_RPMH_BCM_COMMIT (1u << 30)
#define Q8B_RPMH_BCM_VALID (1u << 29)
#define Q8B_RPMH_BCM_LIMIT 0x3fffu

enum q8b_rpmh_vote_kind {
    Q8B_RPMH_ARC_CORNER, Q8B_RPMH_BCM_BANDWIDTH, Q8B_RPMH_BCM_ENABLE
};
typedef struct { uint32_t x, y; } q8b_rpmh_value_t;
typedef struct {
    uint32_t client;
    q8b_rpmh_value_t value;
} q8b_rpmh_voter_t;
typedef struct {
    enum q8b_rpmh_vote_kind kind;
    uint32_t address, limit, poisoned, pending_client;
    q8b_rpmh_value_t floor, current, pending;
    q8b_rpmh_voter_t voters[Q8B_RPMH_MAX_VOTERS];
} q8b_rpmh_votes_t;
typedef int (*q8b_rpmh_submit_t)(void*, uint32_t address, uint32_t data);

static inline int q8b_rpmh_value_valid(const q8b_rpmh_votes_t* r,
        q8b_rpmh_value_t v) {
    if (r->kind == Q8B_RPMH_ARC_CORNER) return !v.y && v.x <= r->limit;
    if (r->kind == Q8B_RPMH_BCM_ENABLE) return !v.x && !(v.y & ~r->limit);
    return r->kind == Q8B_RPMH_BCM_BANDWIDTH &&
            v.x <= Q8B_RPMH_BCM_LIMIT && v.y <= Q8B_RPMH_BCM_LIMIT;
}
static inline int q8b_rpmh_votes_init(q8b_rpmh_votes_t* r,
        enum q8b_rpmh_vote_kind kind, uint32_t address, uint32_t limit,
        q8b_rpmh_value_t known_floor) {
    if (!r || r->address || kind > Q8B_RPMH_BCM_ENABLE || kind < Q8B_RPMH_ARC_CORNER ||
            (kind == Q8B_RPMH_ARC_CORNER ?
             ((address >> 16) != 3 || (address & 15) || limit > 0xffffu) :
             ((address >> 16) != 5 || (address & 3))) ||
            (kind == Q8B_RPMH_BCM_BANDWIDTH && limit != Q8B_RPMH_BCM_LIMIT) ||
            (kind == Q8B_RPMH_BCM_ENABLE &&
             (!limit || (limit & ~Q8B_RPMH_BCM_LIMIT)))) return -1;
    q8b_rpmh_votes_t next = {0};
    next.kind = kind; next.address = address; next.limit = limit;
    if (!q8b_rpmh_value_valid(&next, known_floor)) return -1;
    next.floor = next.current = known_floor;
    *r = next;
    return 0;
}
static inline void q8b_rpmh_value_add(const q8b_rpmh_votes_t* r,
        q8b_rpmh_value_t* total, q8b_rpmh_value_t v) {
    if (r->kind == Q8B_RPMH_ARC_CORNER) {
        if (v.x > total->x) total->x = v.x;
    } else if (r->kind == Q8B_RPMH_BCM_ENABLE) total->y |= v.y;
    else {
        total->x += v.x;
        if (total->x > Q8B_RPMH_BCM_LIMIT) total->x = Q8B_RPMH_BCM_LIMIT;
        if (v.y > total->y) total->y = v.y;
    }
}
static inline uint32_t q8b_rpmh_value_encode(const q8b_rpmh_votes_t* r,
        q8b_rpmh_value_t v) {
    if (r->kind == Q8B_RPMH_ARC_CORNER) return v.x;
    return Q8B_RPMH_BCM_COMMIT | ((v.x || v.y) ? Q8B_RPMH_BCM_VALID : 0) |
            (v.x << 14) | v.y;
}
/* A zero value withdraws only this client's request. submit must wait for a
 * fresh response-required RSC IRQ and complete TCS teardown before returning 0.
 * A failed or timed-out submission might already have affected hardware, so
 * the old committed ledger is retained, poisoned, and cannot accept retries. */
static inline int q8b_rpmh_votes_update(q8b_rpmh_votes_t* r, uint32_t client,
        q8b_rpmh_value_t value, q8b_rpmh_submit_t submit, void* context) {
    if (!r || !r->address || !client || !submit || r->poisoned ||
            !q8b_rpmh_value_valid(r, value)) return -1;
    unsigned slot = Q8B_RPMH_MAX_VOTERS, empty = Q8B_RPMH_MAX_VOTERS;
    for (unsigned i = 0; i < Q8B_RPMH_MAX_VOTERS; i++) {
        if (r->voters[i].client == client) slot = i;
        if (!r->voters[i].client && empty == Q8B_RPMH_MAX_VOTERS) empty = i;
    }
    if (slot == Q8B_RPMH_MAX_VOTERS) {
        if (!value.x && !value.y) return 0;
        slot = empty;
    }
    if (slot == Q8B_RPMH_MAX_VOTERS) return -1;
    q8b_rpmh_value_t next = r->floor;
    for (unsigned i = 0; i < Q8B_RPMH_MAX_VOTERS; i++)
        if (i == slot) q8b_rpmh_value_add(r, &next, value);
        else if (r->voters[i].client)
            q8b_rpmh_value_add(r, &next, r->voters[i].value);
    if ((next.x != r->current.x || next.y != r->current.y) &&
            submit(context, r->address, q8b_rpmh_value_encode(r, next)) != 0) {
        r->poisoned = 1;
        r->pending_client = client;
        r->pending = value;
        return -1;
    }
    r->voters[slot].client = (value.x || value.y) ? client : 0;
    r->voters[slot].value = value;
    r->current = next;
    return 0;
}
#endif
