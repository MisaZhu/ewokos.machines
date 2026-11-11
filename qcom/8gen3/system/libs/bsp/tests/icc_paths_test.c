/* Host test for the QUP0 SPI ICC command-flow candidate: verifies ledger
 * init over the measured BCM set, enable/disable encoding (ACV enable bit,
 * bandwidth average/peak), idempotence, clamping and poisoned submit. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "q8b_rpmh_votes.h"
#include "q8b_spi_icc.h"

#define MAXLOG 64
static struct { uint32_t addr, data; unsigned n; } log[MAXLOG];
static unsigned log_n;
static unsigned fail_at;   /* 0 = never fail */
static int submit(void* ctx, uint32_t address, uint32_t data) {
    (void)ctx;
    log_n++;
    if (fail_at && log_n == fail_at)
        return -1;
    if (log_n <= MAXLOG) {
        log[log_n - 1].addr = address;
        log[log_n - 1].data = data;
    }
    return 0;
}

static void reset(void) { log_n = 0; fail_at = 0; memset(log, 0, sizeof(log)); }

int main(void) {
    q8b_rpmh_votes_t ledgers[Q8B_ICC_N_PATHS];
    memset(ledgers, 0, sizeof(ledgers));

    /* init: all nine ledgers created */
    assert(q8b_spi_icc_init(ledgers) == (int)Q8B_ICC_N_PATHS);
    assert(q8b_spi_icc_init(ledgers) == 0);   /* idempotent */
    assert(ledgers[0].address == Q8B_ICC_ACV && ledgers[8].address == Q8B_ICC_QUP);
    assert(ledgers[0].kind == Q8B_RPMH_BCM_ENABLE);
    assert(ledgers[1].kind == Q8B_RPMH_BCM_BANDWIDTH);

    /* enable: 8 bandwidth + 1 ACV = 9 submits; ACV last in array order */
    reset();
    assert(q8b_spi_icc_apply(ledgers, 1, 500, 1, submit, NULL) == 0);
    assert(log_n == Q8B_ICC_N_PATHS);
    {
        int acv_at = -1;
        int bw_sum = 0;
        unsigned acv_bits = 0;
        for (unsigned i = 0; i < log_n; i++) {
            uint32_t d = log[i].data;
            if (log[i].addr == Q8B_ICC_ACV) {
                acv_at = (int)i;
                acv_bits = d & 0x3fffu;
                assert((d & Q8B_RPMH_BCM_VALID) && (d & Q8B_RPMH_BCM_COMMIT));
            } else {
                assert(((d >> 14) & 0x3fffu) == 500u);   /* avg */
                assert((d & 0x3fffu) == 500u);           /* peak */
                bw_sum++;
            }
        }
        assert(acv_at == 0 && acv_bits == 8u && bw_sum == 8);
    }
    /* same client updates in place; ACV bit unchanged so it is not
     * resubmitted (only changed values hit the wire) */
    reset();
    assert(q8b_spi_icc_apply(ledgers, 1, 900, 1, submit, NULL) == 0);
    assert(log_n == 8);                          /* 8 BW ledgers moved */
    assert(ledgers[0].current.y == 8u);          /* ACV bit retained */
    for (unsigned i = 0; i < log_n; i++)
        assert(((log[i].data >> 14) & 0x3fffu) == 900u);
    /* clamping */
    reset();
    assert(q8b_spi_icc_apply(ledgers, 1, 0x40000u, 1, submit, NULL) == 0);
    for (unsigned i = 0; i < log_n; i++)
        if (log[i].addr != Q8B_ICC_ACV)
            assert(((log[i].data >> 14) & 0x3fffu) == Q8B_RPMH_BCM_LIMIT);
    /* disable withdraws every path */
    reset();
    assert(q8b_spi_icc_apply(ledgers, 1, 0, 0, submit, NULL) == 0);
    for (unsigned i = 0; i < log_n; i++) {
        uint32_t d = log[i].data;
        if (log[i].addr == Q8B_ICC_ACV)
            assert((d & 0x3fffu) == 0u && !(d & Q8B_RPMH_BCM_VALID));
        else
            assert(((d >> 14) & 0x3fffu) == 0u && (d & 0x3fffu) == 0u);
    }
    /* poisoned submit: withdraw first so the next enable moves ACV and the
     * very first submit fails; the ledger freezes and reports -1 */
    reset();
    assert(q8b_spi_icc_apply(ledgers, 2, 0, 0, submit, NULL) == 0);
    reset(); fail_at = 1;
    assert(q8b_spi_icc_apply(ledgers, 3, 800, 1, submit, NULL) == -1);
    assert(ledgers[0].poisoned);
    reset(); fail_at = 0;
    assert(q8b_spi_icc_apply(ledgers, 1, 1000, 1, submit, NULL) == -1); /* frozen */

    puts("q8b QUP0 SPI ICC path tests: ok");
    return 0;
}