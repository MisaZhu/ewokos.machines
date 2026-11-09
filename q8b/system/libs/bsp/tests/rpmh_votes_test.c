#include <q8b_rpmh_votes.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned calls, fail;
static uint32_t last_address, last_data;
static int submit(void* context, uint32_t address, uint32_t data) {
    assert(context == &calls);
    calls++;
    last_address = address; last_data = data;
    return fail ? -1 : 0;
}
static int set(q8b_rpmh_votes_t* r, uint32_t client, uint32_t x, uint32_t y) {
    return q8b_rpmh_votes_update(r, client, (q8b_rpmh_value_t){x, y}, submit, &calls);
}
static void bandwidth(void) {
    q8b_rpmh_votes_t r = {0};
    assert(q8b_rpmh_votes_init(&r, Q8B_RPMH_BCM_BANDWIDTH, 0x50004,
            0x3fff, (q8b_rpmh_value_t){1, 1}) == 0);
    calls = 0;
    assert(set(&r, 1, 3000, 9000) == 0); /* GPU */
    assert(set(&r, 2, 10, 20) == 0); /* SPI must not reduce GPU */
    assert(r.current.x == 3011 && r.current.y == 9000);
    assert(last_address == 0x50004 && last_data ==
            (Q8B_RPMH_BCM_COMMIT | Q8B_RPMH_BCM_VALID | (3011u << 14) | 9000u));
    assert(set(&r, 2, 2, 3) == 0 && r.current.x == 3003 && r.current.y == 9000);
    assert(set(&r, 1, 0, 0) == 0 && r.current.x == 3 && r.current.y == 3);
    assert(set(&r, 2, 0, 0) == 0 && r.current.x == 1 && r.current.y == 1);
    unsigned before = calls;
    assert(set(&r, 999, 0, 0) == 0 && calls == before);
    assert(set(&r, 1, 0x3fff, 0x3fff) == 0);
    assert(set(&r, 2, 0x3fff, 1) == 0 && r.current.x == 0x3fff);
    assert(set(&r, 1, 0, 0) == 0 && r.current.x == 0x3fff && r.current.y == 1);
    assert(set(&r, 2, 0x4000, 0) == -1);
    assert(set(&r, 0, 1, 1) == -1);
    assert(q8b_rpmh_votes_init(&r, Q8B_RPMH_BCM_BANDWIDTH, 0x50004,
            0x3fff, (q8b_rpmh_value_t){0, 0}) == -1);
}
static void corners_and_masks(void) {
    q8b_rpmh_votes_t arc = {0}, acv = {0};
    assert(q8b_rpmh_votes_init(&arc, Q8B_RPMH_ARC_CORNER, 0x30000,
            8, (q8b_rpmh_value_t){2, 0}) == 0);
    assert(set(&arc, 1, 8, 0) == 0 && last_data == 8);
    unsigned before = calls;
    assert(set(&arc, 2, 3, 0) == 0 && calls == before);
    assert(set(&arc, 1, 0, 0) == 0 && last_data == 3); /* lower lease retained */
    assert(set(&arc, 2, 0, 0) == 0 && last_data == 2); /* baseline retained */
    assert(set(&arc, 1, 9, 0) == -1 && set(&arc, 1, 1, 1) == -1);
    assert(q8b_rpmh_votes_init(&acv, Q8B_RPMH_BCM_ENABLE, 0x500ac,
            8, (q8b_rpmh_value_t){0, 0}) == 0);
    assert(set(&acv, 1, 0, 8) == 0);
    before = calls;
    assert(set(&acv, 2, 0, 8) == 0 && calls == before);
    assert(acv.current.y == 8); /* two enables do not become bit 4 */
    assert(set(&acv, 1, 0, 0) == 0 && calls == before);
    assert(set(&acv, 2, 0, 0) == 0 && last_data == Q8B_RPMH_BCM_COMMIT);
    assert(set(&acv, 1, 1, 8) == -1 && set(&acv, 1, 0, 4) == -1);
}
static void failures(void) {
    q8b_rpmh_votes_t r = {0};
    assert(set(&r, 1, 1, 0) == -1);
    assert(q8b_rpmh_votes_init(&r, Q8B_RPMH_ARC_CORNER, 0x30000,
            8, (q8b_rpmh_value_t){1, 0}) == 0);
    for (unsigned i = 1; i <= Q8B_RPMH_MAX_VOTERS; i++)
        assert(set(&r, i, 2, 0) == 0);
    unsigned before = calls;
    assert(set(&r, 9, 3, 0) == -1 && calls == before);
    assert(set(&r, 1, 0, 0) == 0);
    assert(set(&r, 9, 3, 0) == 0); /* zero withdrawal frees a slot */
    fail = 1; before = calls;
    assert(set(&r, 9, 0, 0) == -1 && calls == before + 1);
    assert(r.poisoned && r.pending_client == 9 && !r.pending.x);
    assert(r.current.x == 3); /* do not claim failed withdrawal succeeded */
    before = calls;
    assert(set(&r, 9, 0, 0) == -1 && set(&r, 2, 8, 0) == -1 && calls == before);
    assert(q8b_rpmh_votes_init(&r, Q8B_RPMH_ARC_CORNER, 0x30000,
            8, (q8b_rpmh_value_t){0, 0}) == -1);
    fail = 0;
    q8b_rpmh_votes_t clean = {0}, copy = clean;
    assert(q8b_rpmh_votes_init(&clean, Q8B_RPMH_ARC_CORNER, 0x50000,
            8, (q8b_rpmh_value_t){0, 0}) == -1);
    assert(q8b_rpmh_votes_init(&clean, Q8B_RPMH_ARC_CORNER, 0x30004,
            8, (q8b_rpmh_value_t){0, 0}) == -1);
    assert(q8b_rpmh_votes_init(&clean, Q8B_RPMH_ARC_CORNER, 0x30000,
            8, (q8b_rpmh_value_t){9, 0}) == -1);
    assert(q8b_rpmh_votes_init(&clean, Q8B_RPMH_BCM_ENABLE, 0x50000,
            0, (q8b_rpmh_value_t){0, 0}) == -1);
    assert(memcmp(&copy, &clean, sizeof(clean)) == 0);
}
int main(void) {
    bandwidth(); corners_and_masks(); failures();
    puts("q8b shared RPMh vote tests: ok");
    return 0;
}
