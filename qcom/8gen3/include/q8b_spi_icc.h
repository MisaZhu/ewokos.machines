/* QUP0 SPI interconnect path -> RPMh command flow.
 *
 * Host-tested candidate (like q8b_rpmh_votes.h): maps the gaokun3 QUP0
 * SPI's three ICC paths (qup-core / qup-config / qup-memory) onto the
 * command-db BCM catalog measured on TW220 and drives the shared-vote
 * ledger, producing the encoded commands a kernel submitter (RSC TCS,
 * npu_pas-style) can write.  No MMIO here.
 *
 * Ordering follows the bring-up notes: CONFIG uses CN0/CN2, memory uses
 * SN3/SN0/SH0/MC0/ACV; ENABLE-mask BCMs (ACV) and per-group averages are
 * submitted last per VCD group.  Ledgers are keyed by the BCM address.
 */
#ifndef Q8B_SPI_ICC_H
#define Q8B_SPI_ICC_H
#include "q8b_rpmh_votes.h"

#define Q8B_ICC_N_PATHS 9u

/* BCM command addresses (command-db catalog, see spitest --resources):
 *   ACV enable mask (bit 3), others are bandwidth with VCD groups. */
#define Q8B_ICC_ACV  0x500acu
#define Q8B_ICC_SH2  0x5000cu
#define Q8B_ICC_SN3  0x50018u
#define Q8B_ICC_SN0  0x50010u
#define Q8B_ICC_SH0  0x50004u
#define Q8B_ICC_MC0  0x50000u
#define Q8B_ICC_CN0  0x5003cu
#define Q8B_ICC_CN2  0x50044u
#define Q8B_ICC_QUP  0x50064u   /* qup-core BCM (single row for this QUP) */

typedef struct {
    uint32_t addr;
    q8b_rpmh_value_t floor;     /* known baseline for init */
} q8b_spi_icc_path_t;

static const q8b_spi_icc_path_t Q8B_SPI_ICC_PATHS[Q8B_ICC_N_PATHS] = {
    { Q8B_ICC_ACV, { 0u, 0u } },
    { Q8B_ICC_SH2, { 0u, 0u } },
    { Q8B_ICC_SN3, { 0u, 0u } },
    { Q8B_ICC_SN0, { 0u, 0u } },
    { Q8B_ICC_SH0, { 0u, 0u } },
    { Q8B_ICC_MC0, { 0u, 0u } },
    { Q8B_ICC_CN0, { 0u, 0u } },
    { Q8B_ICC_CN2, { 0u, 0u } },
    { Q8B_ICC_QUP, { 0u, 0u } },
};

/* Initialize all nine ledgers (idempotent per ledger address).  Returns the
 * count of newly initialized ledgers, or -1 on any invalid path/floor. */
static inline int q8b_spi_icc_init(q8b_rpmh_votes_t ledgers[Q8B_ICC_N_PATHS]) {
    if (!ledgers)
        return -1;
    unsigned n = 0;
    for (unsigned i = 0; i < Q8B_ICC_N_PATHS; i++) {
        enum q8b_rpmh_vote_kind kind = (Q8B_SPI_ICC_PATHS[i].addr == Q8B_ICC_ACV)
                ? Q8B_RPMH_BCM_ENABLE : Q8B_RPMH_BCM_BANDWIDTH;
        uint32_t limit = kind == Q8B_RPMH_BCM_ENABLE
                ? (1u << 4) - 1u : Q8B_RPMH_BCM_LIMIT;
        if (q8b_rpmh_votes_init(&ledgers[i], kind, Q8B_SPI_ICC_PATHS[i].addr,
                                limit, Q8B_SPI_ICC_PATHS[i].floor) == 0)
            n++;
    }
    return (int)n;
}

/* Apply an enable request: ACV bits OR-ed in, bandwidth ledgers get the
 * average rate (x) with peak = average (single-client approximation) while
 * enabled, withdrawn (0,0) when disabled.  ENABLE-mask BMC is submitted
 * last.  Returns 0 on success, -1 if any submit failed (ledger poisoned). */
static inline int q8b_spi_icc_apply(q8b_rpmh_votes_t ledgers[Q8B_ICC_N_PATHS],
        uint32_t client, uint32_t avg_kbps, int enable,
        q8b_rpmh_submit_t submit, void* ctx) {
    if (!ledgers || !client || !submit)
        return -1;
    uint32_t acv_bit = (client & 1u) ? 8u : 1u;   /* caller-chosen enable bit */
    for (unsigned i = 0; i < Q8B_ICC_N_PATHS; i++) {
        q8b_rpmh_value_t v = { 0u, 0u };
        /* ACV is the ENABLE-mask resource; the rest are bandwidth.  Full
         * per-VCD-group ordering is left to the kernel submitter (like
         * npu_pas), which batches commands into one TCS transaction. */
        if (ledgers[i].address == Q8B_ICC_ACV)
            v.y = enable ? acv_bit : 0u;          /* OR/withdraw the bit */
        else if (enable) {
            v.x = avg_kbps > Q8B_RPMH_BCM_LIMIT ? Q8B_RPMH_BCM_LIMIT : avg_kbps;
            v.y = v.x;                            /* peak = avg (approx) */
        }
        if (q8b_rpmh_votes_update(&ledgers[i], client, v, submit, ctx) != 0)
            return -1;
    }
    return 0;
}
#endif