/*
 * test_fifo.c -- host unit test for the pure pushbuffer/GPFIFO encoders in
 * ga10b_fifo.h.  Expected headers are the EXACT words nvgpu rel-36 emits
 * (common/ce/ce_app.c nvgpu_prepare_ce_op, subchannel 4) plus the host
 * semaphore / SET_OBJECT words from mm-fifo.md (subchannel 0).  If the
 * encoder reproduces these, the bit layout is correct.
 *
 * Also covers the pure TOP device-table row parser (ga10b.h).
 *
 * Build: cc -I<dir> -Wall -Wextra -o /tmp/test_fifo <dir>/test/test_fifo.c
 */
#include <stdint.h>
#include <stdio.h>
#include "ga10b.h"
#include "ga10b_regs.h"
#include "ga10b_fifo.h"

static int fails;
#define CHECK(expr, want) do { \
    uint32_t _g = (expr), _w = (want); \
    if (_g != _w) { \
        printf("FAIL %s: got 0x%08x want 0x%08x\n", #expr, _g, _w); \
        fails++; \
    } \
} while (0)

int main(void) {
    /* nvgpu CE-app headers (subchannel 4) -- verbatim from ce_app.c */
    CHECK(ga10b_pb_inc(4, GA10B_MTHD_SET_OBJECT, 1),      0x20018000u);
    CHECK(ga10b_pb_inc(4, GA10B_CE_OFFSET_IN_UPPER, 2),   0x20028100u);
    CHECK(ga10b_pb_inc(4, GA10B_CE_SET_SRC_PHYS_MODE, 1), 0x20018098u);
    CHECK(ga10b_pb_inc(4, GA10B_CE_OFFSET_OUT_UPPER, 6),  0x20068102u);
    CHECK(ga10b_pb_inc(4, GA10B_CE_SET_DST_PHYS_MODE, 1), 0x20018099u);
    CHECK(ga10b_pb_inc(4, GA10B_CE_LAUNCH_DMA, 1),        0x200180c0u);
    CHECK(GA10B_SUBCH_CE, 4u);

    /* subchannel 0: SET_OBJECT + host semaphore methods (mm-fifo.md) */
    CHECK(ga10b_pb_inc(0, GA10B_MTHD_SET_OBJECT, 1),      0x20010000u);
    CHECK(ga10b_pb_inc(0, 0x005cu, 1),                    0x20010017u);
    CHECK(ga10b_pb_inc(0, 0x0060u, 1),                    0x20010018u);
    CHECK(ga10b_pb_inc(0, 0x006cu, 1),                    0x2001001bu);
    CHECK(ga10b_pb_inc(0, 0x0020u, 1),                    0x20010008u);

    /* SEC_OP field: INC=1 NONINC=3 IMM=4 at bits 31:29 */
    CHECK(ga10b_pb_inc(0, 0, 1) >> 29,                    1u);
    CHECK(ga10b_pb_noninc(0, 0, 1) >> 29,                 3u);
    CHECK(ga10b_pb_imm(0, 0, 0) >> 29,                    4u);
    CHECK(ga10b_pb_noninc(0, 0, 1),                       0x60010000u);

    /* GPFIFO entry: raw lo32 + hi32 | dwords<<10 */
    uint32_t e[2];
    ga10b_gpfifo_entry(e, 0x123456789abcULL, 0x100u);
    CHECK(e[0], 0x56789abcu);
    CHECK(e[1], 0x00041234u);
    /* length field masks to 21 bits, sits at 10 */
    ga10b_gpfifo_entry(e, 0x40000ULL, 0x1fffffu);
    CHECK(e[0], 0x00040000u);
    CHECK(e[1], 0x7ffffc00u);
    /* va bits 40+ land in entry1 low byte without disturbing length */
    ga10b_gpfifo_entry(e, 0xff00000000ULL, 1u);
    CHECK(e[0], 0x00000000u);
    CHECK(e[1], 0x000004ffu);

    /* ---- instance block encoders (hw_pbdma_ga10b.h / hw_ram_ga10b.h) ---- */
    /* gp_base: (va>>3 & 0x1fffffff)<<3 == lo32(va) & ~7 */
    CHECK(ga10b_gp_base(0x123456789abcULL), 0x56789ab8u);
    /* gp_base_hi: va[39:32] | log2(entries)<<16 */
    CHECK(ga10b_gp_base_hi(0x123456789abcULL, 10), 0x000a0034u);
    CHECK(ga10b_ramfc_target(1), 0x00030001u);
    CHECK(ga10b_ramfc_intr_notify(0x98), 0x80000098u);
    CHECK(ga10b_ramfc_ch_info(0, 5), 0x00050000u);
    CHECK(ga10b_ramfc_ch_info(3, 5), 0x00050300u);
    /* pdb_lo: ncoh|vol|ver2|big64K = 0xc07, pa>>12 in bits 31:12 */
    CHECK(ga10b_ramin_pdb_lo(0x123456000ULL), 0x23456c07u);

    /* ---- runlist entries (ram_rl_entry_* fields) ---- */
    uint32_t rl[4];
    ga10b_rl_tsg(rl, 7, 1);
    /* entry_type_tsg | scale 3 | timeout 128: the 1024 us default
     * timeslice (gv11b_runlist_get_tsg_entry); zero would never run */
    CHECK(rl[0], 0x80030001u);
    CHECK(rl[1], 0x00000001u);   /* length = 1 channel */
    CHECK(rl[2], 0x00000007u);   /* tsgid */
    CHECK(rl[3], 0x00000000u);
    ga10b_rl_ch(rl, 3, 0xabcdef000ULL, 0x123456000ULL);
    /* inst/userd target ncoh (3<<4 | 3<<6) | userd_ptr_lo<<8 */
    CHECK(rl[0], 0x234560f0u);
    CHECK(rl[1], 0x00000001u);   /* userd hi */
    CHECK(rl[2], 0xbcdef003u);   /* chid | inst_ptr_lo<<12 */
    CHECK(rl[3], 0x0000000au);   /* inst hi */

    /* ---- RAMFC constants (gp10b_pbdma_get_signature = GPFIFO class,
     * gm20b_pbdma_acquire_val(0) = retry_man_2 | retry_exp_2) ---- */
    CHECK(GA10B_RAMFC_SIG_VALID, GA10B_CLASS_CHANNEL_GPFIFO_B);
    CHECK(GA10B_RAMFC_SIG_VALID, 0x0000c76fu);
    CHECK(GA10B_RAMFC_ACQUIRE_VAL, 0x00000102u);

    /* ---- TOP device table rows (ga10b_top_parse_next_dev) ---- */
    static const uint32_t rows[] = {
        0x00000000u,                             /* empty row: skipped */
        0x80000000u, 0xc0400005u, 0x00800000u,   /* GR: 3 rows */
        0x00000000u, 0x00000000u,
        0x93000021u, 0xc0104006u, 0x00800001u,   /* LCE inst 0 */
        0x95000000u, 0x00110000u,                /* 2-row non-engine dev */
        0x93010022u, 0xc0104007u, 0x00800002u,   /* LCE inst 1 */
        0x05000000u, 0x00000000u,                /* no chain bit: broken */
    };
    ga10b_devtable_t t;
    ga10b_devtable_parse(rows, sizeof(rows) / sizeof(rows[0]), &t);
    CHECK(t.count, 4u);
    CHECK(t.dev[0].type, GA10B_DEVTYPE_GRAPHICS);
    CHECK(t.dev[0].pri_base, 0x00400000u);
    CHECK(t.dev[0].reset_id, 5u);
    CHECK((uint32_t)t.dev[0].is_engine, 1u);
    CHECK(t.dev[0].runlist_base, 0x00800000u);
    CHECK(t.dev[0].rleng_id, 0u);
    CHECK(t.dev[1].type, GA10B_DEVTYPE_LCE);
    CHECK(t.dev[1].inst_id, 0u);
    CHECK(t.dev[1].pri_base, 0x00104000u);
    CHECK(t.dev[1].reset_id, 6u);
    CHECK(t.dev[1].rleng_id, 1u);
    CHECK(t.dev[2].type, 0x15u);
    CHECK(t.dev[2].pri_base, 0x00110000u);
    CHECK((uint32_t)t.dev[2].is_engine, 0u);
    CHECK(t.dev[2].runlist_base, 0u);
    CHECK(t.dev[3].inst_id, 1u);
    CHECK(t.dev[3].reset_id, 7u);
    CHECK(t.dev[3].rleng_id, 2u);
    CHECK(ga10b_device_find(&t, GA10B_DEVTYPE_LCE, 1) == &t.dev[3], 1u);
    CHECK(ga10b_device_find(&t, GA10B_DEVTYPE_GRAPHICS, 0) == &t.dev[0], 1u);
    CHECK(ga10b_device_find(&t, GA10B_DEVTYPE_LCE, 2) == NULL, 1u);
    /* a table truncated inside a device yields no partial entry */
    ga10b_devtable_parse(rows, 2, &t);
    CHECK(t.count, 0u);

    /* ---- C7C0 compute method headers + QMDV03_00 encoder ---- */
    CHECK(ga10b_pb_inc(0, GA10B_CP_SET_QMD_VERSION, 1),    0x200100a2u);
    CHECK(ga10b_pb_inc(0, GA10B_CP_SEND_PCAS_A, 1),        0x200100adu);
    CHECK(ga10b_pb_inc(0, GA10B_CP_SEND_SIGNALING_PCAS2_B, 1), 0x200100b0u);
    CHECK(ga10b_pb_inc(0, GA10B_CP_SET_SHADER_LOCAL_MEMORY_A, 2),
                                                           0x200201e4u);
    CHECK(GA10B_QMD_VERSION_V03_00,                        0x00300030u);

    uint32_t q[64];
    for (int i = 0; i < 64; i++) q[i] = 0;
    ga10b_qmd_minimal(q, 0x1234567000ULL, 0xabcde000ULL, 0x55u, 32u);
    CHECK(q[4],  0x00000040u);   /* SM_GLOBAL_CACHING_ENABLE */
    CHECK(q[5],  0xfc000000u);
    CHECK(q[12], 1u);
    CHECK(q[18], 0x00200030u);   /* V03_00 | blockDim.x=32 */
    CHECK(q[19], 0x00010001u);
    CHECK(q[20], 0x00002000u);   /* REGISTER_COUNT_V = raw 32 GPRs <<8 */
    CHECK(q[24], 0xabcde000u);   /* RELEASE0 addr lo */
    CHECK(q[25], 0x40880000u);   /* upper 0 | membar19 | en23 | 1word30 */
    CHECK(q[26], 0x00000055u);
    CHECK(q[48], 0x34567000u);   /* PROGRAM_ADDRESS lo */
    CHECK(q[49], 0x00000012u);
    CHECK(q[51], 0x87000000u);   /* SASS sm87 */
    CHECK(q[0] | q[1] | q[63], 0u);   /* untouched dwords stay zero */

    /* REGISTER_COUNT_V is a 9-bit field (656:648): 511 fits, 512 wraps */
    ga10b_qmd_minimal(q, 0, 0, 0, 511u);
    CHECK(q[20], 0x0001ff00u);
    ga10b_qmd_minimal(q, 0, 0, 0, 512u);
    CHECK(q[20], 0x00000000u);

    if (fails == 0) {
        printf("test_fifo: ALL PASS\n");
        return 0;
    }
    printf("test_fifo: %d FAIL\n", fails);
    return 1;
}