#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_vc4_loop.py - generator for the self-looping VC4 (V3D 2.1) QPU
kernels used by bsp_g2d.c on BCM2837:

    argb_fill_loop_vc4   one QPU walks N contiguous 16-word groups,
                         one VDW per group (color preloaded in VPM row 0)
    argb_copy_loop_vc4   one QPU walks N 16-pixel spans of one dst row,
                         TMU-gathering each span from a per-lane linear
                         source map (identity blit, rot90/180/270,
                         integer scales) - NO VDR anywhere
    argb_alpha_loop_vc4  copy loop shape plus a second TMU gather of the
                         dst span and the source-over ALU blend from the
                         staged alpha pipeline - replaces its three SRQ
                         launches per 12 spans with one per 12 rows

Safety rules baked in (each one is a verified BCM2837 failure mode):
  * never read a register-file B address < 32: normal ADD-pipe writes
    land in file A only, so B reads return power-on garbage that has
    wedged VideoCore for good when fed to VDW (the "band kernel" bug)
  * never use rf0/ra0/rb0: the staging patch clears branch waddrs to
    0/0, so every taken branch writes its link PC to ra0 and rb0
  * never use a small immediate > 15 (table indices 16..31 alias the
    negative constants on real VC4); #64 goes through sig-loadimm
  * no VDR in a loop: a second VDR to a different address invalidates
    every regfile written before it on real BCM2837 (alpha kernel note);
    the copy loop gathers through TMU instead, which cache_scrub proves
    safe inside a loop
  * <= 512 loop iterations per thread (G2D_BAND_MAX_VDW): the host
    dispatchers enforce it, ~1280 iterations wedges the part

Every instruction form is either produced by the encoder below - which
must reproduce the hardware-proven argb_fill_vc4, argb_blit_vc4 and
cache_scrub_vc4 arrays byte-for-byte before anything is emitted - or is
one of those kernels' raw words verbatim.

Run:  python3 gen_vc4_loop.py       (prints the C arrays; non-zero exit
                                     and no output on any check failure)
"""

import sys

# ---------------------------------------------------------------------
# VC4 (V3D 2.1) instruction encoder - field layout verified against the
# proven kernel words (see selfcheck below).
# ---------------------------------------------------------------------

SIG_ALU, SIG_LDTMU0, SIG_SMIMM, SIG_LOADIMM, SIG_BRANCH = 0x1, 0xa, 0xd, 0xe, 0xf

# waddr magic
W_R0, W_R1, W_R2, W_R3 = 32, 33, 34, 35
W_NOP = 39
W_VPM = 48
W_VPMVCD = 49          # ws=0: read setup (VDR), ws=1: write setup (VDW)
W_VPM_ADDR = 50        # ws=0: vr_addr, ws=1: vw_addr
W_HOST_INT = 38

# raddr magic
RA_UNIF = 32
RA_ELEM = 38           # raddr_a only
RB_QPU = 38            # raddr_b only
RA_NOP = 39
RA_VPM = 48
RA_WAIT = 50           # raddr_a: vl_wait, raddr_b: vw_wait

OP_ADD, OP_SUB, OP_SHR, OP_SHL, OP_AND, OP_OR = 12, 13, 14, 17, 20, 21


def alu(sig=SIG_ALU, ca=0, cm=0, sf=0, ws=0, wa=W_NOP, wm=W_NOP,
        opm=0, opa=0, ra=RA_NOP, rb=RA_NOP, aa=0, ab=0, ma=0, mb=0):
    w = (sig << 60) | (ca << 49) | (cm << 46) | (sf << 45) | (ws << 44)
    w |= (wa << 38) | (wm << 32) | (opm << 29) | (opa << 24)
    w |= (ra << 18) | (rb << 12) | (aa << 9) | (ab << 6) | (ma << 3) | mb
    return w


def nop():
    return alu(ca=1)                    # 0x100209e7009e7000


def mov_unif(wa):
    """mov <wa>, unif (rf or accumulator dst)"""
    return alu(ca=1, wa=wa, opa=OP_OR, ra=RA_UNIF, aa=6, ab=6)


def mov_elem(wa):
    return alu(ca=1, wa=wa, opa=OP_OR, ra=RA_ELEM, aa=6, ab=6)


def mov_qpu_num(wa):
    return alu(ca=1, wa=wa, opa=OP_OR, rb=RB_QPU, aa=7, ab=7)


def mov_from_acc(wa, acc, ws=0):
    """mov <wa>, r<acc>"""
    return alu(ca=1, ws=ws, wa=wa, opa=OP_OR, aa=acc, ab=acc)


def mov_from_rf(wa, rf, ws=0):
    """mov <wa>, rf<rf> (file A read)"""
    return alu(ca=1, ws=ws, wa=wa, opa=OP_OR, ra=rf, aa=6, ab=6)


def shl_acc_imm(acc, imm):
    """shl r<acc>, r<acc>, #imm (small immediate)"""
    assert 0 <= imm <= 15
    return alu(sig=SIG_SMIMM, ca=1, wa=32 + acc, opa=OP_SHL,
               rb=imm, aa=acc, ab=7)


def add_rf_acc(rf, acc):
    """add rf<rf>, rf<rf>, r<acc>"""
    return alu(ca=1, wa=rf, opa=OP_ADD, ra=rf, aa=6, ab=acc)


def sub_rf_acc(rf, acc):
    """sub rf<rf>, rf<rf>, r<acc>"""
    return alu(ca=1, wa=rf, opa=OP_SUB, ra=rf, aa=6, ab=acc)


def add_acc_rf(acc, rf):
    """add r<acc>, r<acc>, rf<rf>"""
    return alu(ca=1, wa=32 + acc, opa=OP_ADD, ra=rf, aa=acc, ab=6)


def sub_setf_rf_imm1(rf):
    """sub.setf rf<rf>, rf<rf>, #1"""
    return alu(sig=SIG_SMIMM, ca=1, sf=1, wa=rf, opa=OP_SUB,
               ra=rf, rb=1, aa=6, ab=7)


def sub_acc_acc(dst_acc, sa, sb):
    """sub r<dst>, r<sa>, r<sb> (accumulator-only: no regfile latency)"""
    return alu(ca=1, wa=32 + dst_acc, opa=OP_SUB, aa=sa, ab=sb)


def add_acc_acc(dst_acc, sa, sb):
    """add r<dst>, r<sa>, r<sb> (accumulator-only)"""
    return alu(ca=1, wa=32 + dst_acc, opa=OP_ADD, aa=sa, ab=sb)


def add_acc_acc_imm(dst_acc, sa, imm):
    """add r<dst>, r<sa>, #imm"""
    assert 0 <= imm <= 15
    return alu(sig=SIG_SMIMM, ca=1, wa=32 + dst_acc, opa=OP_ADD,
               rb=imm, aa=sa, ab=7)


def shr_acc_acc_imm(dst_acc, sa, imm):
    """shr r<dst>, r<sa>, #imm"""
    assert 0 <= imm <= 15
    return alu(sig=SIG_SMIMM, ca=1, wa=32 + dst_acc, opa=OP_SHR,
               rb=imm, aa=sa, ab=7)


def shr_acc_rf_imm(dst_acc, rf, imm):
    """shr r<dst>, rf<rf>, #imm"""
    assert 0 <= imm <= 15
    return alu(sig=SIG_SMIMM, ca=1, wa=32 + dst_acc, opa=OP_SHR,
               ra=rf, rb=imm, aa=6, ab=7)


def shr_rf_rf_imm(dst_rf, src_rf, imm):
    """shr rf<dst>, rf<src>, #imm"""
    assert 0 <= imm <= 15
    return alu(sig=SIG_SMIMM, ca=1, wa=dst_rf, opa=OP_SHR,
               ra=src_rf, rb=imm, aa=6, ab=7)


def and_acc_rf_acc(dst_acc, rf, acc):
    """and r<dst>, rf<rf>, r<acc>"""
    return alu(ca=1, wa=32 + dst_acc, opa=OP_AND, ra=rf, aa=6, ab=acc)


def and_acc_acc(dst_acc, sa, sb):
    """and r<dst>, r<sa>, r<sb> (accumulator-only)"""
    return alu(ca=1, wa=32 + dst_acc, opa=OP_AND, aa=sa, ab=sb)


def and_rf_acc_acc(rf, sa, sb):
    """and rf<rf>, r<sa>, r<sb>"""
    return alu(ca=1, wa=rf, opa=OP_AND, aa=sa, ab=sb)


def and_rf_self_acc(rf, acc):
    """and rf<rf>, rf<rf>, r<acc>"""
    return alu(ca=1, wa=rf, opa=OP_AND, ra=rf, aa=6, ab=acc)


def mul24_acc(dst_acc, sa, sb):
    """mul24 r<dst>, r<sa>, r<sb> (mul pipe, accumulator-only operands)"""
    return alu(ca=0, cm=1, wm=32 + dst_acc, opm=2, ma=sa, mb=sb)


def mul24_acc_rf(dst_acc, sa, rf):
    """mul24 r<dst>, r<sa>, rf<rf>"""
    return alu(ca=0, cm=1, wm=32 + dst_acc, opm=2, ra=rf, ma=sa, mb=6)


def mul24_rf_acc(rf, acc):
    """mul24 rf<rf>, rf<rf>, r<acc> (ws=1: the mul write lands in
    file A, matching the proven gen_kernels 'mul24 rfN, rfN, rX' form)"""
    return alu(ca=0, cm=1, ws=1, wm=rf, opm=2, ra=rf, ma=6, mb=acc)


def vw_wait():
    return alu(ca=1, opa=OP_OR, rb=RA_WAIT, aa=7, ab=7)


def loadimm(wa, imm, ws=0):
    w = imm & 0xFFFFFFFF
    w |= (SIG_LOADIMM << 60) | (1 << 49) | (ws << 44)
    w |= (wa << 38) | (W_NOP << 32)
    return w


def branch_anynz(idx, target):
    """relative branch, cond 'any Z clear', from instruction idx to target"""
    off = (target - (idx + 4)) * 8
    w = (SIG_BRANCH << 60) | (3 << 52) | (1 << 51)
    w |= (W_NOP << 38) | (W_NOP << 32)
    return w | (off & 0xFFFFFFFF)


HOST_EXIT = 0xd00209a7159c0fc0   # proven thread-end interrupt word
PROG_END = 0x300009e7009e7000    # proven sig thrend
LDTMU0 = 0xa00249e7009e7000      # proven ldtmu0 (result -> r4)

# ---------------------------------------------------------------------
# self-check: reproduce the three hardware-proven kernels byte-for-byte.
# Together they exercise every encoder form the new kernels use.
# ---------------------------------------------------------------------

PROVEN_FILL_VC4 = [
    0x1002002715827d80, 0x1002082715827d80, 0x1002006715827d80,
    0xe0021c6700101a00, 0x10020c27159e7000, 0x100209e7159f2fc0,
    0x10021c6715067d80, 0x10021ca715027d80, 0x100209e7159f2fc0,
    0xd00209a7159c0fc0, 0x300009e7009e7000, 0x100209e7009e7000,
    0x100209e7009e7000,
]

PROVEN_BLIT_VC4 = [
    0x1002002715827d80, 0x1002006715827d80, 0x100200a715827d80,
    0x100200e715827d80, 0x100208e7159e6fc0, 0xd00208e7119c27c0,
    0x10020127159e76c0, 0xd00208e7119c47c0, 0x100208e70c0a7780,
    0x10020c67159e76c0, 0x10020ca715027d80, 0x100209e715ca7d80,
    0x100208e715127d80, 0xd00208e7119c77c0, 0x100208e70c0e7780,
    0x10021c67159e76c0, 0x10021ca715067d80, 0x100209e7159f2fc0,
    0xd00209a7159c0fc0, 0x300009e7009e7000, 0x100209e7009e7000,
    0x100209e7009e7000,
]

PROVEN_SCRUB_VC4 = [
    0x1002002715827d80, 0x1002006715827d80,
    0x10020827159a7d80, 0xd0020827119c21c0,
    0x100200270c027c00, 0xe00208a700000040,
    0x10020e2715027d80, 0xa00249e7009e7000,
    0x100200e7159e7900, 0xd00220670d041dc0,
    0xf03809e7ffffffc0, 0x100200270c027c80,
    0x100209e7009e7000, 0x100209e7009e7000,
    0xd00209a7159c0fc0, 0x300009e7009e7000,
    0x100209e7009e7000, 0x100209e7009e7000,
]


def vl_wait():
    return alu(ca=1, opa=OP_OR, ra=RA_WAIT, aa=6, ab=6)


def build_fill_vc4():
    return [
        mov_unif(0),                       # rf0 = dst bus address
        mov_unif(W_R0),                    # r0 = color
        mov_unif(1),                       # rf1 = VDW setup word
        loadimm(W_VPMVCD, 0x00101a00, ws=1),
        mov_from_acc(W_VPM, 0),
        vw_wait(),
        mov_from_rf(W_VPMVCD, 1, ws=1),
        mov_from_rf(W_VPM_ADDR, 0, ws=1),
        vw_wait(),
        HOST_EXIT, PROG_END, nop(), nop(),
    ]


def build_blit_vc4():
    return [
        mov_unif(0), mov_unif(1), mov_unif(2), mov_unif(3),
        mov_qpu_num(W_R3),
        shl_acc_imm(3, 2),
        mov_from_acc(4, 3),                # rf4 = qpu*4
        shl_acc_imm(3, 4),                 # r3 = qpu*64
        add_acc_rf(3, 2),                  # r3 += VDR setup
        mov_from_acc(W_VPMVCD, 3, ws=0),   # vr_setup
        mov_from_rf(W_VPM_ADDR, 0, ws=0),  # vr_addr = src
        vl_wait(),
        mov_from_rf(W_R3, 4),              # r3 = rf4
        shl_acc_imm(3, 7),                 # r3 = qpu*512
        add_acc_rf(3, 3),                  # r3 += VDW setup (rf3)
        mov_from_acc(W_VPMVCD, 3, ws=1),   # vw_setup
        mov_from_rf(W_VPM_ADDR, 1, ws=1),  # vw_addr = dst
        vw_wait(),
        HOST_EXIT, PROG_END, nop(), nop(),
    ]


def build_scrub_vc4():
    return [
        mov_unif(0),                       # rf0 = base
        mov_unif(1),                       # rf1 = line count
        mov_elem(W_R0),
        shl_acc_imm(0, 2),
        add_rf_acc(0, 0),                  # rf0 += elem*4
        loadimm(W_R2, 64),
        mov_from_rf(56, 0),                # tmu0_s = rf0
        LDTMU0,
        mov_from_acc(3, 4),                # rf3 = r4 (consume)
        sub_setf_rf_imm1(1),
        branch_anynz(10, 6),
        add_rf_acc(0, 2),                  # delay: rf0 += 64
        nop(), nop(),
        HOST_EXIT, PROG_END, nop(), nop(),
    ]


# ---------------------------------------------------------------------
# the two new kernels
# ---------------------------------------------------------------------

def build_fill_loop():
    """argb_fill_loop_vc4: u0=dst bus address, u1=color,
    u2=number of contiguous 16-word groups (1..512).
    All QPUs share VPM row 0 (same color per launch - benign)."""
    k = [
        mov_unif(1),                        # 0  rf1 = dst bus address
        mov_unif(W_R0),                     # 1  r0 = color
        mov_unif(2),                        # 2  rf2 = group count
        loadimm(W_VPMVCD, 0x00101a00, ws=1),  # 3 VPM write setup, row 0
        mov_from_acc(W_VPM, 0),             # 4  vpm = color (16 lanes)
        vw_wait(),                          # 5
        loadimm(W_R2, 64),                  # 6  r2 = 64 (group stride)
        # loop:
        loadimm(W_VPMVCD, 0x80904000, ws=1),  # 7 VDW setup: 16 words row 0
        mov_from_rf(W_VPM_ADDR, 1, ws=1),   # 8  vw_addr = rf1 (kick VDW)
        vw_wait(),                          # 9
        add_rf_acc(1, 2),                   # 10 rf1 += 64
        sub_setf_rf_imm1(2),                # 11 rf2 -= 1, set flags
        branch_anynz(12, 7),                # 12
        nop(), nop(), nop(),                # 13..15 delay slots
        HOST_EXIT,                          # 16
        PROG_END, nop(), nop(),             # 17..19
    ]
    return k


def build_copy_loop():
    """argb_copy_loop_vc4: TMU linear-gather span copy, one dst row per
    QPU.  Uniforms per QPU:
      u0 = src address of the first span's lane 0 (L2 alias)
      u1 = dst span address (direct alias)
      u2 = span count (1..512)
      u3 = per-lane source byte step, positive part
      u4 = per-lane source byte step, negative part (subtracted)
      u5 = per-span source byte step (signed; 16 * lane step)
      u6 = VDW setup base 0x80904080 (16 words from VPM row 1)
    Per-QPU VPM windows (rows 1+4q) and the u6+(qpu*4)<<7 VDW setup are
    exactly the proven argb_gather_vc4 shape; TMU-in-loop is the proven
    cache_scrub_vc4 shape.  No VDR."""
    k = [
        mov_unif(1),                        # 0  rf1 = src lane-0 address
        mov_unif(2),                        # 1  rf2 = dst span address
        mov_unif(3),                        # 2  rf3 = span count
        mov_unif(W_R0),                     # 3  r0 = lane step (add part)
        mov_unif(W_R1),                     # 4  r1 = lane step (sub part)
        mov_unif(W_R2),                     # 5  r2 = span step
        mov_unif(7),                        # 6  rf7 = VDW setup base
        mov_qpu_num(W_R3),                  # 7
        shl_acc_imm(3, 2),                  # 8  r3 = qpu*4
        mov_from_acc(4, 3),                 # 9  rf4 = qpu*4
        shl_acc_imm(3, 7),                  # 10 r3 = qpu*512
        add_acc_rf(3, 7),                   # 11 r3 += VDW setup base
        mov_from_acc(6, 3),                 # 12 rf6 = per-QPU VDW setup
        loadimm(W_R3, 0x00101a01),          # 13 VPM write setup, row 1
        add_acc_rf(3, 4),                   # 14 r3 += qpu*4 -> row 1+4q
        mov_from_acc(5, 3),                 # 15 rf5 = per-QPU VPM setup
        mov_elem(W_R3),                     # 16 r3 = lane 0..15
        mul24_acc(0, 3, 0),                 # 17 r0 = lane * step_add
        mul24_acc(1, 3, 1),                 # 18 r1 = lane * step_sub
        # combine in accumulators FIRST: writing rf1 twice back to back
        # trips the regfile write->read latency (the second op reads the
        # STALE rf1, dropping the lane offsets - every lane then gathers
        # the span's lane-0 pixel and the output turns into solid
        # 16-pixel colour blocks on real silicon).
        sub_acc_acc(0, 0, 1),               # 19 r0 -= r1 (net lane step)
        add_rf_acc(1, 0),                   # 20 rf1 += r0 (per lane)
        loadimm(W_R3, 64),                  # 21 r3 = 64 (dst stride)
        # loop:
        mov_from_rf(56, 1),                 # 22 tmu0_s = rf1 (16 addrs)
        LDTMU0,                             # 23 r4 = 16 gathered pixels
        mov_from_rf(W_VPMVCD, 5, ws=1),     # 24 VPM write setup
        mov_from_acc(W_VPM, 4),             # 25 vpm = r4
        mov_from_rf(W_VPMVCD, 6, ws=1),     # 26 VDW setup
        mov_from_rf(W_VPM_ADDR, 2, ws=1),   # 27 vw_addr = rf2 (kick VDW)
        vw_wait(),                          # 28
        add_rf_acc(1, 2),                   # 29 rf1 += span step
        add_rf_acc(2, 3),                   # 30 rf2 += 64
        sub_setf_rf_imm1(3),                # 31 rf3 -= 1, set flags
        branch_anynz(32, 22),               # 32
        nop(), nop(), nop(),                # 33..35 delay slots
        HOST_EXIT,                          # 36
        PROG_END, nop(), nop(),             # 37..39
    ]
    return k


def build_alpha_loop():
    """argb_alpha_loop_vc4: TMU gather + source-over blend + VDW, one
    dst row per QPU.  Same linear-gather eligibility as the copy loop
    (integer maps, every full group in-source); the dst span is
    gathered through the L2 alias and written back through the direct
    alias.  Uniforms per QPU:
      u0 = src address of the first span's lane 0 (L2 alias)
      u1 = dst span address (direct alias, VDW target)
      u2 = span count (1..512)
      u3 = per-lane source byte step, positive part
      u4 = per-lane source byte step, negative part (subtracted)
      u5 = per-span source byte step (signed; 16 * lane step)
      u6 = VDW setup base 0x80904080
      u7 = dst span address (L2 alias, TMU gather)
      u8 = global alpha (1..255, or 256 when fully opaque)
    Blend (endpoint-exact source-over):
      sa'  = (S.a * alpha) >> 8
      sa'' = sa' + (sa' >> 7)              (remaps 255 -> 256)
      out.c = (S.c * sa'' + D.c * (256 - sa'')) >> 8
      out.a = D.a + ((255 - D.a) * sa'') >> 8
    so a transparent source lane leaves D exactly and an opaque source
    lane writes S exactly (a plain >>8 form darkens both endpoints by
    one and fails g2dtest's corner/center pixel checks).
    (mul24 is unsigned: every product is all-positive by construction,
    bounded by 255 * 256 < 2^24.)
    All >>16 / <<16 / <<24 shifts are #8 pairs/triples so the small
    immediates stay in the proven 0..15 range; S/D channel extractions
    are interleaved so no rf register is read within one instruction of
    its write (regfile latency)."""
    k = [
        mov_unif(1),                        # 0  rf1 = src lane-0 address
        mov_unif(2),                        # 1  rf2 = dst span (VDW)
        mov_unif(3),                        # 2  rf3 = span count
        mov_unif(W_R0),                     # 3  r0 = lane step (add part)
        mov_unif(W_R1),                     # 4  r1 = lane step (sub part)
        mov_unif(11),                       # 5  rf11 = span step
        mov_unif(7),                        # 6  rf7 = VDW setup base
        mov_unif(8),                        # 7  rf8 = dst span (TMU)
        mov_unif(9),                        # 8  rf9 = global alpha
        mov_qpu_num(W_R3),                  # 9
        shl_acc_imm(3, 2),                  # 10 r3 = qpu*4
        mov_from_acc(4, 3),                 # 11 rf4 = qpu*4
        shl_acc_imm(3, 7),                  # 12 r3 = qpu*512
        add_acc_rf(3, 7),                   # 13 r3 += VDW setup base
        mov_from_acc(6, 3),                 # 14 rf6 = per-QPU VDW setup
        loadimm(W_R3, 0x00101a01),          # 15 VPM write setup, row 1
        add_acc_rf(3, 4),                   # 16 r3 += qpu*4 -> row 1+4q
        mov_from_acc(5, 3),                 # 17 rf5 = per-QPU VPM setup
        mov_elem(W_R3),                     # 18 r3 = lane 0..15
        mul24_acc(0, 3, 0),                 # 19 r0 = lane * step_add
        mul24_acc(1, 3, 1),                 # 20 r1 = lane * step_sub
        sub_acc_acc(0, 0, 1),               # 21 r0 -= r1 (net lane step)
        add_rf_acc(1, 0),                   # 22 rf1 += r0 (per lane)
        shl_acc_imm(3, 2),                  # 23 r3 = lane * 4
        add_rf_acc(8, 3),                   # 24 rf8 += lane*4 (per lane)
        loadimm(W_R2, 0xff),                # 25 r2 = 255 (loop-invariant)
    ]
    loop = len(k)
    k += [
        # loop: gather S then D (proven tmu0_s -> ldtmu0 -> r4 shape)
        mov_from_rf(56, 1),                 # tmu0_s = rf1 (16 src addrs)
        LDTMU0,                             # r4 = S
        mov_from_acc(13, 4),                # rf13 = S
        mov_from_rf(56, 8),                 # tmu0_s = rf8 (16 dst addrs)
        LDTMU0,                             # r4 = D (stays in r4)
        # sa'' = ((S >> 24) * alpha >> 8) remapped so 255 -> 256
        shr_acc_rf_imm(0, 13, 8),           # r0 = S >> 8
        shr_acc_acc_imm(0, 0, 8),           # r0 = S >> 16
        shr_acc_acc_imm(0, 0, 8),           # r0 = S.a
        mul24_acc_rf(0, 0, 9),              # r0 = S.a * alpha
        shr_acc_acc_imm(0, 0, 8),           # r0 = sa' (0..255)
        shr_acc_acc_imm(3, 0, 7),           # r3 = sa' >> 7
        add_acc_acc(0, 0, 3),               # r0 = sa'' (0..256)
        sub_acc_acc(1, 2, 0),               # r1 = 255 - sa'' (may wrap)
        add_acc_acc_imm(1, 1, 1),           # r1 = 256 - sa'' = inv
        # blue
        and_acc_rf_acc(3, 13, 2),           # r3 = S.b
        and_rf_acc_acc(12, 4, 2),           # rf12 = D.b
        mul24_acc(3, 3, 0),                 # r3 = S.b * sa'
        mul24_rf_acc(12, 1),                # rf12 = D.b * (255-sa')
        shr_rf_rf_imm(14, 13, 8),           # rf14 = S >> 8 (green prep)
        add_acc_rf(3, 12),                  # r3 += rf12
        shr_acc_acc_imm(3, 3, 8),           # r3 = out.b
        mov_from_acc(15, 3),                # rf15 = out (b)
        # green
        and_rf_self_acc(14, 2),             # rf14 = S.g
        shr_acc_acc_imm(3, 4, 8),           # r3 = D >> 8
        mul24_rf_acc(14, 0),                # rf14 = S.g * sa'
        and_acc_acc(3, 3, 2),               # r3 = D.g
        mul24_acc(3, 3, 1),                 # r3 = D.g * (255-sa')
        add_acc_rf(3, 14),                  # r3 += rf14
        shr_acc_acc_imm(3, 3, 8),           # r3 = out.g
        shl_acc_imm(3, 8),                  # r3 <<= 8
        add_rf_acc(15, 3),                  # rf15 = b | g<<8
        # red
        shr_rf_rf_imm(14, 13, 8),           # rf14 = S >> 8
        shr_acc_acc_imm(3, 4, 8),           # r3 = D >> 8
        shr_rf_rf_imm(14, 14, 8),           # rf14 = S >> 16
        shr_acc_acc_imm(3, 3, 8),           # r3 = D >> 16
        and_rf_self_acc(14, 2),             # rf14 = S.r
        and_acc_acc(3, 3, 2),               # r3 = D.r
        mul24_rf_acc(14, 0),                # rf14 = S.r * sa'
        mul24_acc(3, 3, 1),                 # r3 = D.r * (255-sa')
        add_acc_rf(3, 14),                  # r3 += rf14
        shr_acc_acc_imm(3, 3, 8),           # r3 = out.r
        shl_acc_imm(3, 8),                  # r3 <<= 8 ...
        shl_acc_imm(3, 8),                  # ... <<= 16
        add_rf_acc(15, 3),                  # rf15 = b | g<<8 | r<<16
        # alpha: out.a = D.a + ((255 - D.a) * sa') >> 8
        shr_acc_acc_imm(3, 4, 8),           # r3 = D >> 8
        shr_acc_acc_imm(3, 3, 8),           # r3 = D >> 16
        shr_acc_acc_imm(3, 3, 8),           # r3 = D.a
        sub_acc_acc(1, 2, 3),               # r1 = 255 - D.a
        mul24_acc(1, 1, 0),                 # r1 *= sa'
        shr_acc_acc_imm(1, 1, 8),           # r1 >>= 8
        add_acc_acc(3, 3, 1),               # r3 = out.a
        shl_acc_imm(3, 8),                  # r3 <<= 8 ...
        shl_acc_imm(3, 8),                  # ... <<= 16 ...
        shl_acc_imm(3, 8),                  # ... <<= 24
        add_rf_acc(15, 3),                  # rf15 = out pixel
        # store (proven VPM write + VDW shape, rows 1+4q)
        mov_from_rf(W_VPMVCD, 5, ws=1),     # VPM write setup
        mov_from_rf(W_VPM, 15),             # vpm = 16 out pixels
        mov_from_rf(W_VPMVCD, 6, ws=1),     # VDW setup
        mov_from_rf(W_VPM_ADDR, 2, ws=1),   # vw_addr = rf2 (kick VDW)
        vw_wait(),
        # bookkeeping
        mov_from_rf(W_R0, 11),              # r0 = span step
        loadimm(W_R1, 64),                  # r1 = 64 (dst stride)
        add_rf_acc(1, 0),                   # rf1 += span step
        add_rf_acc(8, 1),                   # rf8 += 64
        add_rf_acc(2, 1),                   # rf2 += 64
        sub_setf_rf_imm1(3),                # rf3 -= 1, set flags
    ]
    k.append(branch_anynz(len(k), loop))
    k += [
        nop(), nop(), nop(),                # delay slots
        HOST_EXIT,
        PROG_END, nop(), nop(),
    ]
    return k


# ---------------------------------------------------------------------
# static analyzer: decode every word AFTER simulating the staging patch
# (g2d_vc4_patch_vdw in v3d_g2d.c) and reject the known failure modes.
# ---------------------------------------------------------------------

PATCH_PAIRS = [
    (0x10020c67155a7d80, 0x10021c67155a7d80),
    (0x10020c67159e7480, 0x10021c67159e7480),
    (0x10020c67159e7000, 0x10021c67159e7000),
    (0xe0020c67c0000000, 0xe0021c67c0000000),
    (0x10020ca7159e7240, 0x10021ca7159e7240),
    (0x10020ca715127d80, 0x10021ca715127d80),
]


def simulate_patch(code):
    out = []
    for w in code:
        for f, t in PATCH_PAIRS:
            if w == f:
                w = t
        if (w >> 60) == 0xf:
            w &= ~0x00000fff00000000
        out.append(w)
    return out


def analyze(name, code):
    errs = []
    patched = simulate_patch(code)
    # no source word may collide with a patch 'from' word
    for i, w in enumerate(code):
        for f, _t in PATCH_PAIRS:
            if w == f:
                errs.append('%s[%d]: collides with patch table' % (name, i))
    a_written = set([0])   # ra0 gets the branch link PC - written, poison
    b_written = set([0])
    prev_a_writes = set()  # regfile writes of the immediately previous
    prev_b_writes = set()  # instruction: reading them now is undefined
    for i, w in enumerate(patched):
        cur_a_writes = set()
        cur_b_writes = set()
        sig = w >> 60
        if sig == 0xf:                     # branch
            if not (w >> 51) & 1:
                errs.append('%s[%d]: absolute branch' % (name, i))
            off = w & 0xFFFFFFFF
            if off & 0x80000000:
                off -= 1 << 32
            tgt = i + 4 + off // 8
            if off % 8 or tgt < 0 or tgt >= len(patched):
                errs.append('%s[%d]: branch target %d out of range'
                            % (name, i, tgt))
            prev_a_writes, prev_b_writes = cur_a_writes, cur_b_writes
            continue
        if sig == 0xe:                     # loadimm: no reads
            wa, wm = (w >> 38) & 0x3f, (w >> 32) & 0x3f
            for wad, ws in ((wa, (w >> 44) & 1), (wm, 1 - ((w >> 44) & 1))):
                if wad < 32:
                    (a_written if ws == 0 else b_written).add(wad)
                    (cur_a_writes if ws == 0 else cur_b_writes).add(wad)
            prev_a_writes, prev_b_writes = cur_a_writes, cur_b_writes
            continue
        if sig not in (0x1, 0x3, 0xa, 0xd):   # ALU, thrend, ldtmu, smimm
            errs.append('%s[%d]: unexpected sig %x' % (name, i, sig))
            prev_a_writes, prev_b_writes = cur_a_writes, cur_b_writes
            continue
        ra, rb = (w >> 18) & 0x3f, (w >> 12) & 0x3f
        opa, opm = (w >> 24) & 0x1f, (w >> 29) & 7
        muxes = []
        if opa:
            muxes += [(w >> 9) & 7, (w >> 6) & 7]
        if opm:
            muxes += [(w >> 3) & 7, w & 7]
        smimm = (sig == 0xd)
        if 6 in muxes and ra < 32:
            if ra == 0:
                errs.append('%s[%d]: reads ra0' % (name, i))
            if ra not in a_written:
                errs.append('%s[%d]: reads unwritten ra%d' % (name, i, ra))
            if ra in prev_a_writes:
                errs.append('%s[%d]: reads ra%d written by previous'
                            ' instruction (regfile latency)' % (name, i, ra))
        if 7 in muxes:
            if smimm:
                if rb > 16:
                    errs.append('%s[%d]: small immediate index %d > 16'
                                % (name, i, rb))
            elif rb < 32:
                errs.append('%s[%d]: register-file B read rb%d'
                            % (name, i, rb))
            elif rb in prev_b_writes:
                errs.append('%s[%d]: reads rb%d written by previous'
                            ' instruction (regfile latency)' % (name, i, rb))
        # writes: add pipe ws=0 -> file A, ws=1 -> file B; mul inverted
        ws = (w >> 44) & 1
        wa, wm = (w >> 38) & 0x3f, (w >> 32) & 0x3f
        if opa or (sig == 0x1 and wa != 39) or smimm:
            if wa < 32:
                if wa == 0:
                    errs.append('%s[%d]: writes rf0' % (name, i))
                (a_written if ws == 0 else b_written).add(wa)
                (cur_a_writes if ws == 0 else cur_b_writes).add(wa)
        if opm and wm < 32:
            (b_written if ws == 0 else a_written).add(wm)
            (cur_b_writes if ws == 0 else cur_a_writes).add(wm)
        prev_a_writes, prev_b_writes = cur_a_writes, cur_b_writes
    return errs


def check(name, got, want):
    if got == want:
        return []
    errs = []
    for i in range(max(len(got), len(want))):
        g = got[i] if i < len(got) else None
        w = want[i] if i < len(want) else None
        if g != w:
            errs.append('%s[%d]: got %s want %s' %
                        (name, i,
                         '0x%016x' % g if g is not None else 'missing',
                         '0x%016x' % w if w is not None else 'missing'))
    return errs


def emit_c(name, code, comment):
    print('/* %s */' % comment)
    print('const uint64_t g2d_qpu_%s[] = {' % name)
    for i in range(0, len(code), 3):
        row = ', '.join('0x%016xULL' % w for w in code[i:i + 3])
        print('    %s,' % row)
    print('};')
    print('const unsigned g2d_qpu_%s_n = %d;' % (name, len(code)))
    print('')


def main():
    errs = []
    errs += check('selfcheck fill_vc4', build_fill_vc4(), PROVEN_FILL_VC4)
    errs += check('selfcheck blit_vc4', build_blit_vc4(), PROVEN_BLIT_VC4)
    errs += check('selfcheck scrub_vc4', build_scrub_vc4(), PROVEN_SCRUB_VC4)
    if errs:
        for e in errs:
            sys.stderr.write(e + '\n')
        sys.stderr.write('encoder selfcheck FAILED\n')
        return 1

    fill_loop = build_fill_loop()
    copy_loop = build_copy_loop()
    alpha_loop = build_alpha_loop()
    for name, code in (('fill_loop', fill_loop), ('copy_loop', copy_loop),
                       ('alpha_loop', alpha_loop)):
        errs += analyze(name, code)
    # the analyzer must also accept the three proven kernels except for
    # their (proven-tolerated) rf0 usage - run it and only fail on NEW
    # classes of error to keep the analyzer honest about them
    for name, code in (('fill_vc4', PROVEN_FILL_VC4),
                       ('blit_vc4', PROVEN_BLIT_VC4),
                       ('scrub_vc4', PROVEN_SCRUB_VC4)):
        for e in analyze(name, code):
            if 'ra0' not in e and 'rf0' not in e:
                errs.append('(proven!) ' + e)
    if errs:
        for e in errs:
            sys.stderr.write(e + '\n')
        sys.stderr.write('static analysis FAILED\n')
        return 1

    emit_c('argb_fill_loop_vc4', fill_loop,
           'argb_fill_loop_vc4: contiguous group fill loop.  Uniforms per'
           ' QPU:\n * 0=dst bus address, 1=color, 2=group count (1..512).'
           '  See\n * tools/gen_vc4_loop.py.')
    emit_c('argb_copy_loop_vc4', copy_loop,
           'argb_copy_loop_vc4: TMU linear-gather span-copy loop (no VDR).'
           '\n * Uniforms per QPU: 0=src lane-0 address, 1=dst span'
           ' address,\n * 2=span count (1..512), 3=lane step (add part),'
           ' 4=lane step (sub\n * part), 5=span step (signed), 6=VDW setup'
           ' base 0x80904080.  See\n * tools/gen_vc4_loop.py.')
    emit_c('argb_alpha_loop_vc4', alpha_loop,
           'argb_alpha_loop_vc4: TMU-gather source-over blend loop (no'
           ' VDR).\n * Uniforms per QPU: 0=src lane-0 address, 1=dst span'
           ' address (VDW),\n * 2=span count (1..512), 3=lane step (add'
           ' part), 4=lane step (sub\n * part), 5=span step (signed),'
           ' 6=VDW setup base 0x80904080,\n * 7=dst span address (TMU'
           ' gather), 8=global alpha (1..256).  See\n'
           ' * tools/gen_vc4_loop.py.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
