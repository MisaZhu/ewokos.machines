# gen_kernels.py - build the eight V3D 4.2 (BCM2711) CSD kernels and the
# four VC4 (V3D 2.1, BCM2837) SRQ kernels for the raspix bsp_g2d back
# end and emit g2d_qpu_kernels.h.
#
# The V3D 4.2 CSD kernels are 1:1 binary translations of the
# hardware-proven raspi5 (V3D 7.1) kernels, produced by
# translate_71_to_42.py from machines/raspi5's g2d_qpu_kernels.h
# (per-operand raddr slots -> shared raddr_a/raddr_b + mux fields,
# VPACK/V8PACK emulated, add<->mul pipe moves re-homed; each output is
# structurally re-verified).  The hand-written 4.2 kernels below
# (kernel_fill/kernel_affine/kernel_alpha) are kept as ISA reference
# but are no longer emitted.
#
# The VC4 kernels are re-implementations of the raspi5 algorithms
# adapted to the VC4-generation ISA:
#   - plain ub=0 conditional branches (the uniform stream never moves;
#     every uniform is loaded up front with ldunifrf),
#   - straightforward non-pipelined loops, paired tmud/tmua stores,
#   - the proven thread-end protocol (tmuwt, nop, thrsw, thrsw, nops).
#
# Usage:  python3 gen_kernels.py [outfile]
#         (default outfile: ../g2d_qpu_kernels.h)

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qpuasm import K42, K21, decode, decode21, AsmError, SMALL_IMM21
import translate_71_to_42

MAX_WORDS = 512   # CSD_CODE_WORDS in v3d_g2d.c (376-word argb_alpha)

RASPI5_KERNELS_H = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    '..', '..', '..', '..', '..', '..', '..',
    'raspi5', 'system', 'libs', 'bsp', 'src', 'g2d',
    'g2d_qpu_kernels.h'))


# --------------------------------------------------------------------------
# shared snippets
# --------------------------------------------------------------------------

def ldunifrf_seq(k, first, count):
    """u[first..first+count-1] -> rf[first..] via ldunifrf"""
    for i in range(count):
        k.alu(sig=('ldunifrf',), sigdst='rf%d' % (first + i),
              comment='u%d -> rf%d' % (first + i, first + i))


def clip_flags(k, t, x, x0, x1, y, y0, y1):
    """per-lane flag = (x0 <= x < x1) and (y0 <= y < y1)"""
    # small immediates reach only the ADD pipe's mux B, so the flag
    # reset is a self-subtract (always 0 -> Z set on all lanes)
    k.alu(a=('sub', t, t, t), flags={'apf': 'pushz'}, comment='flag = true')
    k.alu(a=('sub', t, x, x0), flags={'auf': 'andnn'}, comment='x >= x0')
    k.alu(a=('sub', t, x, x1), flags={'auf': 'andn'}, comment='x < x1')
    k.alu(a=('sub', t, y, y0), flags={'auf': 'andnn'}, comment='y >= y0')
    k.alu(a=('sub', t, y, y1), flags={'auf': 'andn'}, comment='y < y1')


def step16_64(k, r16, r64, t):
    """r16 = 16, r64 = 64 (16 has no small-immediate encoding;
    small immediates reach only the ADD pipe)"""
    k.alu(a=('sub', t, t, t), comment='0')
    k.alu(a=('add', t, t, '#15'), comment='15')
    k.alu(a=('add', r16, t, '#1'), comment='x step = 16')
    k.alu(a=('shl', r64, r16, '#2'), comment='addr step = 64')


def exit_main(k):
    k.alu(a=('tmuwt', None), comment='wait for TMU writes')
    k.nop(1)
    k.alu(sig=('thrsw',), comment='thread switch')
    k.alu(sig=('thrsw',), comment='thread switch -> THREND')
    k.nop(4, 'THREND delay slots')


def exit_guard(k):
    k.label('gexit')
    k.alu(sig=('thrsw',), comment='guard exit')
    k.alu(sig=('thrsw',), comment='THREND')
    k.nop(4, 'THREND delay slots')


# --------------------------------------------------------------------------
# argb_fill (16 uniforms)
#   rf0..rf15 = u0..u15; rf16 lane; rf17 x; rf18 y; rf19 addr;
#   rf20 scratch addr; rf21 group counter; rf22 main counter;
#   rf23 16; rf24 64; rf25/rf26 temps
# --------------------------------------------------------------------------

def kernel_fill():
    k = K42('argb_fill')
    ldunifrf_seq(k, 0, 16)
    k.alu(a=('eidx', 'rf16'), comment='lane')
    k.alu(m=('mov', 'rf17', 'rf16'), comment='x = lane')
    k.alu(a=('shl', 'rf26', 'rf16', '#2'), comment='lane * 4')
    k.alu(a=('add', 'rf19', 'rf6', 'rf26'), comment='addr = phys + lane*4')
    # qid / band (rf26 = qid after this block)
    k.alu(a=('tidx', 'rf26'), comment='tidx')
    k.alu(a=('shr', 'rf26', 'rf26', '#2'), comment='tidx >> 2')
    k.alu(a=('and', 'rf26', 'rf26', '#15'), comment='qid = (tidx>>2)&15')
    k.alu(m=('smul24', 'rf18', 'rf26', 'rf12'), comment='y = qid * rows')
    k.alu(m=('smul24', 'rf25', 'rf26', 'rf13'), comment='qid * rows_stride')
    k.alu(a=('add', 'rf19', 'rf19', 'rf25'), comment='addr += band offset')
    k.alu(m=('msub', 'rf25', 'rf3', 'rf18'), flags={'mpf': 'pushn'},
          comment='(h-1) - y, push N')
    k.branch('gexit', 'anya', 'band past last row -> exit')
    k.nop(3, 'branch delay slots')
    # per-lane scratch address
    k.alu(m=('mov', 'rf20', 'rf14'), comment='scratch base')
    k.alu(a=('shl', 'rf25', 'rf26', '#6'), comment='qid * 64')
    k.alu(a=('add', 'rf20', 'rf20', 'rf25'), comment='')
    k.alu(a=('shl', 'rf25', 'rf16', '#2'), comment='lane * 4')
    k.alu(a=('add', 'rf20', 'rf20', 'rf25'), comment='')
    # constants + counters
    k.alu(m=('mov', 'rf21', 'rf1'), comment='group counter = L-1')
    step16_64(k, 'rf23', 'rf24', 'rf25')
    k.alu(m=('mov', 'rf22', 'rf5'), comment='main counter = L*rows')
    # full / clipped split
    k.alu(a=('sub', 'rf25', 'rf11', '#1'), flags={'apf': 'pushz'},
          comment='full == 1 ?')
    k.branch('fast', 'anya', 'full surface fast path')
    k.nop(3, 'branch delay slots')
    # ---- clipped loop: one TMU store per group -------------------------
    k.label('slow_loop')
    k.alu(m=('mov', 'rf25', 'rf19'), comment='group addr = addr')
    clip_flags(k, 'rf26', 'rf17', 'rf7', 'rf8', 'rf18', 'rf9', 'rf10')
    k.alu(m=('mov', 'rf25', 'rf20'), flags={'mc': 'ifna'},
          comment='out-of-rect lane -> scratch')
    k.alu(m=('mov', 'tmud', 'rf0'), comment='store data = color')
    k.alu(m=('mov', 'tmua', 'rf25'), comment='store fires')
    k.alu(a=('add', 'rf19', 'rf19', 'rf24'), comment='addr += 64')
    k.alu(m=('msub', 'rf21', 'rf21', '#1'), flags={'mpf': 'pushn'},
          comment='group counter --')
    k.alu(m=('mov', 'rf21', 'rf1'), flags={'mc': 'ifa'},
          comment='reload L-1 at row wrap')
    k.alu(a=('add', 'rf18', 'rf18', '#1'), flags={'ac': 'ifa'},
          comment='y ++ at row wrap')
    k.alu(a=('add', 'rf17', 'rf17', 'rf23'), comment='x += 16')
    k.alu(m=('mov', 'rf17', 'rf16'), flags={'mc': 'ifa'},
          comment='x = lane at row wrap')
    k.alu(m=('msub', 'rf19', 'rf19', 'rf4'), flags={'mc': 'ifa'},
          comment='addr -= row jump at wrap')
    k.alu(a=('sub', 'rf22', 'rf22', '#1'), flags={'apf': 'pushz'},
          comment='main counter --')
    k.branch('slow_loop', 'anyna', 'loop while any counter >= 0')
    k.nop(3, 'branch delay slots')
    exit_main(k)
    # ---- full-surface fast loop ----------------------------------------
    k.label('fast')
    k.alu(m=('mov', 'rf22', 'rf12'), comment='counter = rows')
    k.label('fast_loop')
    k.alu(m=('mov', 'tmud', 'rf0'), comment='store data = color')
    k.alu(m=('mov', 'tmua', 'rf19'), comment='store fires')
    k.alu(a=('add', 'rf19', 'rf19', 'rf24'), comment='addr += 64')
    k.alu(m=('msub', 'rf21', 'rf21', '#1'), flags={'mpf': 'pushn'},
          comment='group counter --')
    k.alu(m=('mov', 'rf21', 'rf1'), flags={'mc': 'ifa'},
          comment='reload L-1 at row wrap')
    k.alu(m=('msub', 'rf22', 'rf22', '#1'), flags={'mc': 'ifa'},
          comment='rows -- at row wrap')
    k.alu(m=('msub', 'rf19', 'rf19', 'rf4'), flags={'mc': 'ifa'},
          comment='addr -= row jump at wrap')
    k.alu(a=('sub', 'rf25', 'rf22', '#0'), flags={'apf': 'pushz'},
          comment='rows == 0 ?')
    k.branch('fast_loop', 'anyna', 'loop while any rows != 0')
    k.nop(3, 'branch delay slots')
    exit_main(k)
    exit_guard(k)
    return k


# --------------------------------------------------------------------------
# argb_blit / argb_rotate (24 uniforms, shared skeleton)
#   rf0..rf23 = u0..u23; rf24 lane; rf25 x; rf26 y; rf27 dst addr;
#   rf28 scratch addr; rf29 group counter; rf30 main counter;
#   rf31 16; rf32 64; rf33..rf35 temps; rf38/rf39 full-path product
#   accumulators; rf40 pixel
# --------------------------------------------------------------------------

def affine_map(k, t1, t2, t3, x, y):
    """t1 = ((pu*x + qu*y) >> 15) + cu ; t2 = ((pv*x + qv*y) >> 15) + cv"""
    k.alu(m=('smul24', t1, 'rf0', x), comment='pu*x')
    k.alu(m=('smul24', t2, 'rf1', y), comment='qu*y')
    k.alu(a=('add', t1, t1, t2), comment='')
    k.alu(a=('asr', t1, t1, '#15'), comment='>> 15')
    k.alu(a=('add', t1, t1, 'rf4'), comment='+ cu -> u')
    k.alu(m=('smul24', t2, 'rf2', x), comment='pv*x')
    k.alu(m=('smul24', t3, 'rf3', y), comment='qv*y')
    k.alu(a=('add', t2, t2, t3), comment='')
    k.alu(a=('asr', t2, t2, '#15'), comment='>> 15')
    k.alu(a=('add', t2, t2, 'rf5'), comment='+ cv -> v')


def affine_bounds_flags(k, t3, u, v):
    """flag = u/v inside the source surface (before clamping)"""
    k.alu(a=('sub', t3, t3, t3), flags={'apf': 'pushz'},
          comment='in-bounds flag')
    k.alu(a=('sub', t3, u, '#0'), flags={'auf': 'andnn'}, comment='u >= 0')
    k.alu(a=('sub', t3, 'rf6', u), flags={'auf': 'andnn'},
          comment='u <= src_w-1')
    k.alu(a=('sub', t3, v, '#0'), flags={'auf': 'andnn'}, comment='v >= 0')
    k.alu(a=('sub', t3, 'rf7', v), flags={'auf': 'andnn'},
          comment='v <= src_h-1')


def affine_clamp(k, u, v):
    k.alu(a=('max', u, u, '#0'), comment='u = max(u, 0)')
    k.alu(a=('min', u, u, 'rf6'), comment='u = min(u, src_w-1)')
    k.alu(a=('max', v, v, '#0'), comment='v = max(v, 0)')
    k.alu(a=('min', v, v, 'rf7'), comment='v = min(v, src_h-1)')


def affine_load(k, t2, u, v, pixel):
    """TMU load of src[(v,u)] into pixel (u/v already clamped)"""
    k.alu(m=('smul24', t2, v, 'rf8'), comment='v * src_w')
    k.alu(a=('add', t2, t2, u), comment='+ u')
    k.alu(a=('shl', t2, t2, '#2'), comment='* 4')
    k.alu(a=('add', t2, t2, 'rf10'), comment='+ src phys')
    k.alu(m=('mov', 'tmua', t2), comment='TMU read addr = src pixel')
    k.alu(sig=('ldtmu',), sigdst=pixel, comment='pixel = *src')


def kernel_affine(rotate):
    k = K42('argb_rotate' if rotate else 'argb_blit')
    t1, t2, t3 = 'rf33', 'rf34', 'rf35'
    pixel = 'rf40'
    ldunifrf_seq(k, 0, 24)
    k.alu(a=('eidx', 'rf24'), comment='lane')
    k.alu(m=('mov', 'rf25', 'rf24'), comment='x = lane')
    k.alu(a=('shl', t1, 'rf24', '#2'), comment='lane * 4')
    k.alu(a=('add', 'rf27', 'rf9', t1), comment='addr = dst phys + lane*4')
    # qid / band (t1 = qid after this block)
    k.alu(a=('tidx', t1), comment='tidx')
    k.alu(a=('shr', t1, t1, '#2'), comment='tidx >> 2')
    k.alu(a=('and', t1, t1, '#15'), comment='qid = (tidx>>2)&15')
    k.alu(m=('smul24', 'rf26', t1, 'rf21'), comment='y = qid * rows')
    k.alu(m=('smul24', t2, t1, 'rf22'), comment='qid * rows_stride')
    k.alu(a=('add', 'rf27', 'rf27', t2), comment='addr += band offset')
    k.alu(m=('msub', t2, 'rf13', 'rf26'), flags={'mpf': 'pushn'},
          comment='(dst_h-1) - y, push N')
    k.branch('gexit', 'anya', 'band past last row -> exit')
    k.nop(3, 'branch delay slots')
    # per-lane scratch address (t1 still = qid)
    k.alu(m=('mov', 'rf28', 'rf23'), comment='scratch base')
    k.alu(a=('shl', t2, t1, '#6'), comment='qid * 64')
    k.alu(a=('add', 'rf28', 'rf28', t2), comment='')
    k.alu(a=('shl', t2, 'rf24', '#2'), comment='lane * 4')
    k.alu(a=('add', 'rf28', 'rf28', t2), comment='')
    # constants + counters
    k.alu(m=('mov', 'rf29', 'rf11'), comment='group counter = L-1')
    step16_64(k, 'rf31', 'rf32', t2)
    # full / clipped split
    k.alu(a=('sub', t2, 'rf20', '#1'), flags={'apf': 'pushz'},
          comment='full == 1 ?')
    k.branch('full', 'anya', 'full surface fast path')
    k.nop(3, 'branch delay slots')
    k.alu(m=('mov', 'rf30', 'rf15'), comment='main counter = L*rows')

    # ---- clipped loop ---------------------------------------------------
    k.label('clip_loop')
    affine_map(k, t1, t2, t3, 'rf25', 'rf26')
    if rotate:
        affine_bounds_flags(k, t3, t1, t2)
    affine_clamp(k, t1, t2)
    affine_load(k, t2, t1, t2, pixel)
    if rotate:
        k.alu(m=('msub', pixel, pixel, pixel), flags={'mc': 'ifna'},
              comment='out-of-source lane -> transparent 0')
        k.alu(m=('mov', 'tmud', pixel), comment='store data')
        k.alu(m=('mov', 'tmua', 'rf27'), comment='store fires -> dst')
    else:
        k.alu(m=('mov', t1, 'rf27'), comment='store addr = dst')
        clip_flags(k, t2, 'rf25', 'rf16', 'rf17', 'rf26', 'rf18', 'rf19')
        k.alu(m=('mov', t1, 'rf28'), flags={'mc': 'ifna'},
              comment='out-of-rect lane -> scratch')
        k.alu(m=('mov', 'tmud', pixel), comment='store data')
        k.alu(m=('mov', 'tmua', t1), comment='store fires')
    # bookkeeping + main counter
    k.alu(a=('add', 'rf27', 'rf27', 'rf32'), comment='addr += 64')
    k.alu(m=('msub', 'rf29', 'rf29', '#1'), flags={'mpf': 'pushn'},
          comment='group counter --')
    k.alu(m=('mov', 'rf29', 'rf11'), flags={'mc': 'ifa'},
          comment='reload L-1 at row wrap')
    k.alu(a=('add', 'rf26', 'rf26', '#1'), flags={'ac': 'ifa'},
          comment='y ++ at row wrap')
    k.alu(a=('add', 'rf25', 'rf25', 'rf31'), comment='x += 16')
    k.alu(m=('mov', 'rf25', 'rf24'), flags={'mc': 'ifa'},
          comment='x = lane at row wrap')
    k.alu(m=('msub', 'rf27', 'rf27', 'rf14'), flags={'mc': 'ifa'},
          comment='addr -= row jump at wrap')
    k.alu(a=('sub', 'rf30', 'rf30', '#1'), flags={'apf': 'pushz'},
          comment='main counter --')
    k.branch('clip_loop', 'anyna', 'loop while any counter >= 0')
    k.nop(3, 'branch delay slots')
    exit_main(k)

    # ---- full-surface loop (incremental map) ----------------------------
    k.label('full')
    k.alu(m=('smul24', 'rf38', 'rf0', 'rf25'), comment='pu*x')
    k.alu(m=('smul24', t1, 'rf1', 'rf26'), comment='qu*y')
    k.alu(a=('add', 'rf38', 'rf38', t1), comment='u product')
    k.alu(m=('smul24', 'rf39', 'rf2', 'rf25'), comment='pv*x')
    k.alu(m=('smul24', t1, 'rf3', 'rf26'), comment='qv*y')
    k.alu(a=('add', 'rf39', 'rf39', t1), comment='v product')
    k.alu(m=('mov', 'rf30', 'rf21'), comment='main counter = rows')
    k.label('full_loop')
    k.alu(a=('asr', t1, 'rf38', '#15'), comment='u product >> 15')
    k.alu(a=('add', t1, t1, 'rf4'), comment='+ cu -> u')
    k.alu(a=('asr', t2, 'rf39', '#15'), comment='v product >> 15')
    k.alu(a=('add', t2, t2, 'rf5'), comment='+ cv -> v')
    if rotate:
        affine_bounds_flags(k, t3, t1, t2)
    affine_clamp(k, t1, t2)
    affine_load(k, t2, t1, t2, pixel)
    if rotate:
        k.alu(m=('msub', pixel, pixel, pixel), flags={'mc': 'ifna'},
              comment='out-of-source lane -> transparent 0')
    k.alu(m=('mov', 'tmud', pixel), comment='store data')
    k.alu(m=('mov', 'tmua', 'rf27'), comment='store fires -> dst')
    # bookkeeping: products are stepped, not re-evaluated
    k.alu(a=('add', 'rf27', 'rf27', 'rf32'), comment='addr += 64')
    k.alu(a=('add', 'rf38', 'rf38', 'rf18'), comment='u product += pu16')
    k.alu(a=('add', 'rf39', 'rf39', 'rf19'), comment='v product += pv16')
    k.alu(m=('msub', 'rf29', 'rf29', '#1'), flags={'mpf': 'pushn'},
          comment='group counter --')
    k.alu(m=('mov', 'rf29', 'rf11'), flags={'mc': 'ifa'},
          comment='reload L-1 at row wrap')
    k.alu(m=('msub', 'rf30', 'rf30', '#1'), flags={'mc': 'ifa'},
          comment='rows -- at row wrap')
    k.alu(m=('msub', 'rf38', 'rf38', 'rf16'), flags={'mc': 'ifa'},
          comment='u product -= uxwrap')
    k.alu(m=('msub', 'rf39', 'rf39', 'rf17'), flags={'mc': 'ifa'},
          comment='v product -= vxwrap')
    k.alu(m=('msub', 'rf27', 'rf27', 'rf14'), flags={'mc': 'ifa'},
          comment='addr -= row jump at wrap')
    k.alu(a=('sub', t1, 'rf30', '#0'), flags={'apf': 'pushz'},
          comment='rows == 0 ?')
    k.branch('full_loop', 'anyna', 'loop while any rows != 0')
    k.nop(3, 'branch delay slots')
    exit_main(k)
    exit_guard(k)
    return k


# --------------------------------------------------------------------------
# argb_alpha (23 uniforms)
#   rf0..rf22 = u0..u22; rf23 lane; rf24 x; rf25 y; rf26 dst addr;
#   rf27 scratch addr; rf28 group counter; rf29 main counter; rf30 64;
#   rf31 S; rf32 D; rf33..rf37 blend temps
# --------------------------------------------------------------------------

def kernel_alpha():
    k = K42('argb_alpha')
    t1, t2, t3, t4, t5 = 'rf33', 'rf34', 'rf35', 'rf36', 'rf37'
    S, D = 'rf31', 'rf32'
    ldunifrf_seq(k, 0, 23)
    k.alu(a=('eidx', 'rf23'), comment='lane')
    k.alu(m=('mov', 'rf24', 'rf23'), comment='x = lane')
    k.alu(a=('shl', t1, 'rf23', '#2'), comment='lane * 4')
    k.alu(a=('add', 'rf26', 'rf7', t1), comment='addr = dst phys + lane*4')
    # qid / band (t1 = qid after this block)
    k.alu(a=('tidx', t1), comment='tidx')
    k.alu(a=('shr', t1, t1, '#2'), comment='tidx >> 2')
    k.alu(a=('and', t1, t1, '#15'), comment='qid = (tidx>>2)&15')
    k.alu(m=('smul24', 'rf25', t1, 'rf20'), comment='y = qid * rows')
    k.alu(m=('smul24', t2, t1, 'rf21'), comment='qid * rows_stride')
    k.alu(a=('add', 'rf26', 'rf26', t2), comment='addr += band offset')
    k.alu(m=('msub', t2, 'rf10', 'rf25'), flags={'mpf': 'pushn'},
          comment='(dst_h-1) - y, push N')
    k.branch('gexit', 'anya', 'band past last row -> exit')
    k.nop(3, 'branch delay slots')
    # per-lane scratch address (t1 still = qid)
    k.alu(m=('mov', 'rf27', 'rf22'), comment='scratch base')
    k.alu(a=('shl', t2, t1, '#6'), comment='qid * 64')
    k.alu(a=('add', 'rf27', 'rf27', t2), comment='')
    k.alu(a=('shl', t2, 'rf23', '#2'), comment='lane * 4')
    k.alu(a=('add', 'rf27', 'rf27', t2), comment='')
    # constants + counters
    k.alu(m=('mov', 'rf28', 'rf9'), comment='group counter = L-1')
    k.alu(a=('shl', 'rf30', 'rf17', '#2'), comment='addr step = 64')
    k.alu(m=('mov', 'rf29', 'rf12'), comment='main counter = L*rows')

    k.label('loop')
    # src sample: u = (pu*x >> 15) + cu ; v = (qv*y >> 15) + cv
    k.alu(m=('smul24', t1, 'rf0', 'rf24'), comment='pu*x')
    k.alu(a=('asr', t1, t1, '#15'), comment='>> 15')
    k.alu(a=('add', t1, t1, 'rf1'), comment='+ cu -> u')
    k.alu(m=('smul24', t2, 'rf2', 'rf25'), comment='qv*y')
    k.alu(a=('asr', t2, t2, '#15'), comment='>> 15')
    k.alu(a=('add', t2, t2, 'rf3'), comment='+ cv -> v')
    affine_clamp(k, t1, t2)
    k.alu(m=('smul24', t2, t2, 'rf6'), comment='v * src_w')
    k.alu(a=('add', t2, t2, t1), comment='+ u')
    k.alu(a=('shl', t2, t2, '#2'), comment='* 4')
    k.alu(a=('add', t2, t2, 'rf8'), comment='+ src phys')
    k.alu(m=('mov', 'tmua', t2), comment='TMU read addr = src pixel')
    k.alu(sig=('ldtmu',), sigdst=S, comment='S = *src')
    # dst sample: real dst pixel when in-rect, scratch word otherwise
    k.alu(m=('mov', t2, 'rf27'), comment='read addr = scratch')
    clip_flags(k, t1, 'rf24', 'rf13', 'rf14', 'rf25', 'rf15', 'rf16')
    k.alu(m=('mov', t2, 'rf26'), flags={'mc': 'ifa'},
          comment='in-rect lane -> dst addr')
    k.alu(m=('mov', 'tmua', t2), comment='TMU read addr')
    k.alu(sig=('ldtmu',), sigdst=D, comment='D = *dst')
    # ---- blend: out.c = (S.c*sa' + D.c*(255-sa')) >> 8, over-alpha -----
    k.alu(a=('shr', t1, S, 'rf17'), comment='S >> 16')
    k.alu(a=('shr', t1, t1, '#8'), comment='S >> 24')
    k.alu(a=('and', t1, t1, 'rf19'), comment='src alpha')
    k.alu(m=('umul24', t3, t1, 'rf11'), comment='sa * global alpha')
    k.alu(a=('shr', t3, t3, '#8'), comment="sa' = (sa*alpha) >> 8")
    k.alu(a=('sub', t4, 'rf19', t3), comment='inv = 255 - sa\'')
    # blue -> t1
    k.alu(a=('and', t2, S, 'rf19'), comment='S.b')
    k.alu(m=('umul24', t2, t2, t3), comment="S.b * sa'")
    k.alu(a=('and', t1, D, 'rf19'), comment='D.b')
    k.alu(m=('umul24', t1, t1, t4), comment='D.b * inv')
    k.alu(a=('add', t1, t2, t1), comment='')
    k.alu(a=('shr', t1, t1, '#8'), comment='out.b')
    # green -> t2 (folds into t1 << 8)
    k.alu(a=('shr', t2, S, '#8'), comment='S >> 8')
    k.alu(a=('and', t2, t2, 'rf19'), comment='S.g')
    k.alu(m=('umul24', t2, t2, t3), comment="S.g * sa'")
    k.alu(a=('shr', t5, D, '#8'), comment='D >> 8')
    k.alu(a=('and', t5, t5, 'rf19'), comment='D.g')
    k.alu(m=('umul24', t5, t5, t4), comment='D.g * inv')
    k.alu(a=('add', t2, t2, t5), comment='')
    k.alu(a=('shr', t2, t2, '#8'), comment='out.g')
    k.alu(a=('shl', t2, t2, '#8'), comment='<< 8')
    k.alu(a=('add', t1, t1, t2), comment='b | g<<8')
    # red -> t2
    k.alu(a=('shr', t2, S, 'rf17'), comment='S >> 16')
    k.alu(a=('and', t2, t2, 'rf19'), comment='S.r')
    k.alu(m=('umul24', t2, t2, t3), comment="S.r * sa'")
    k.alu(a=('shr', t5, D, 'rf17'), comment='D >> 16')
    k.alu(a=('and', t5, t5, 'rf19'), comment='D.r')
    k.alu(m=('umul24', t5, t5, t4), comment='D.r * inv')
    k.alu(a=('add', t2, t2, t5), comment='')
    k.alu(a=('shr', t2, t2, '#8'), comment='out.r')
    # alpha -> t5: da + ((255-da)*sa') >> 8
    k.alu(a=('shr', t5, D, 'rf17'), comment='D >> 16')
    k.alu(a=('shr', t5, t5, '#8'), comment='D >> 24')
    k.alu(a=('and', t5, t5, 'rf19'), comment='dst alpha')
    k.alu(a=('sub', t4, 'rf19', t5), comment='255 - da')
    k.alu(m=('umul24', t4, t4, t3), comment="(255-da) * sa'")
    k.alu(a=('shr', t4, t4, '#8'), comment='>> 8')
    k.alu(a=('add', t5, t5, t4), comment='out.a')
    # compose
    k.alu(a=('shl', t5, t5, 'rf17'), comment='a << 16')
    k.alu(a=('shl', t5, t5, '#8'), comment='a << 24')
    k.alu(a=('shl', t2, t2, 'rf17'), comment='r << 16')
    k.alu(a=('add', t5, t5, t2), comment='a | r')
    k.alu(a=('add', t1, t5, t1), comment='out pixel')
    # store (rect-gated)
    k.alu(m=('mov', t2, 'rf26'), comment='store addr = dst')
    clip_flags(k, t5, 'rf24', 'rf13', 'rf14', 'rf25', 'rf15', 'rf16')
    k.alu(m=('mov', t2, 'rf27'), flags={'mc': 'ifna'},
          comment='out-of-rect lane -> scratch')
    k.alu(m=('mov', 'tmud', t1), comment='store data')
    k.alu(m=('mov', 'tmua', t2), comment='store fires')
    # bookkeeping + main counter
    k.alu(m=('msub', 'rf28', 'rf28', '#1'), flags={'mpf': 'pushn'},
          comment='group counter --')
    k.alu(a=('add', 'rf24', 'rf24', 'rf17'), comment='x += 16')
    k.alu(m=('mov', 'rf28', 'rf9'), flags={'mc': 'ifa'},
          comment='reload L-1 at row wrap')
    k.alu(a=('add', 'rf25', 'rf25', '#1'), flags={'ac': 'ifa'},
          comment='y ++ at row wrap')
    k.alu(m=('mov', 'rf24', 'rf23'), flags={'mc': 'ifa'},
          comment='x = lane at row wrap')
    k.alu(a=('add', 'rf26', 'rf26', 'rf30'), comment='addr += 64')
    k.alu(m=('msub', 'rf26', 'rf26', 'rf18'), flags={'mc': 'ifa'},
          comment='addr -= row jump at wrap')
    k.alu(a=('sub', 'rf29', 'rf29', '#1'), flags={'apf': 'pushz'},
          comment='main counter --')
    k.branch('loop', 'anyna', 'loop while any counter >= 0')
    k.nop(3, 'branch delay slots')
    exit_main(k)
    exit_guard(k)
    return k


# --------------------------------------------------------------------------
# VC4 (V3D 2.1, BCM2837/Pi3) kernels - SRQ flavor
#
# Threading: one thread per QPU via the SRQ, 16 SIMD lanes each; the
# QPU id comes from a per-QPU uniform (u1), the lane from elem_num
# (raddr 38).  Uniforms are read with 'mov rx, unif' (raddr 32) after
# writing unifa (waddr 40) - the uniform stream itself never moves.
# TMU reads give one 32-bit word per 'add t0s, base, off' + ldtmu0
# into r4; VC4 has NO TMU store, so every group is written through
# VPM (waddr 48) and DMAed out by the VDW (vpmvcd waddr 49 + vpmaddr
# waddr 50, GPU_FFT store protocol incl. the vw_wait stall).  Exit is
# the GPU_FFT host-interrupt protocol (K21.exit).
#
# Register-file discipline (vc4_validate_shaders.c): a rf READ needs
# its raddr resident on the PREVIOUS instruction and at most 2 distinct
# raddrs across the pair - the V21 wrapper inserts raddr-carrying nops
# whenever the (up to 2) live addresses change.
# --------------------------------------------------------------------------

class V21:
    """K21 wrapper enforcing the raddr residency rule.

    A register-file READ needs its raddr resident on the PREVIOUS
    instruction (one-instruction delay) and at most 2 distinct raddrs
    per instruction.  The wrapper tracks which rf addresses the last
    emitted instruction carries ('live') and inserts raddr-carrying
    nops whenever the next read is not covered.  Instructions that
    carry no raddrs (loadimm, ldtmu0, branches) clear the live set.
    """

    def __init__(self, name):
        self.k = K21(name)
        self.live = frozenset()

    @staticmethod
    def _reads_of(op):
        if not op:
            return frozenset()
        rs = []
        for s in op[2:]:
            if s.startswith('rf'):
                rs.append(int(s[2:]))
        return frozenset(rs)

    def _bridge(self, reads):
        if reads and not reads <= self.live:
            if len(reads) > 2:
                raise AsmError('more than 2 rf reads in one instruction')
            self.k.nop_reads(*sorted(reads), comment='rf residency bridge')
            self.live = reads

    def alu(self, a=None, m=None, sig='', flags=None, comment=''):
        if sig == 'ldtmu0':                 # carries no raddrs at all
            self.k.alu(sig=sig, comment=comment)
            self.live = frozenset()
            return
        reads = self._reads_of(a) | self._reads_of(m)
        big_imm = any(s.startswith('#') and
                      int(s[1:], 0) not in SMALL_IMM21
                      for op in (a, m) if op for s in op[2:])
        self._bridge(reads)
        self.k.alu(a=a, m=m, sig=sig, flags=flags, comment=comment)
        # a loadimm occupies bits 31:0 - it carries no raddrs
        self.live = frozenset() if big_imm else reads

    def nop(self, n=1, comment=''):
        self.k.nop(n, comment)              # residency preserved

    def nop_reads(self, ra=39, rb=39, comment=''):
        self.k.nop_reads(ra, rb, comment)
        self.live = frozenset({ra, rb} - {39})

    def branch(self, target, cond='always', comment=''):
        self.k.branch(target, cond, comment)
        self.live = frozenset()             # target arrives cold

    def label(self, name):
        self.k.label(name)
        self.live = frozenset()             # jump targets arrive cold

    def exit(self):
        self.k.exit()
        self.live = frozenset()


def vc4_ldunif(k, targets):
    """sequential uniform reads: u[i] -> targets[i].  Each 'mov rX, unif'
    (raddr 32, sig NONE) consumes one stream word and auto-advances the
    pointer; raddr 32 needs no residency.  A None target consumes the
    word without keeping it."""
    for i, t in enumerate(targets):
        if t is None:
            k.k.alu(a=('or', None, 'unif', 'unif'), comment='u%d -' % i)
        else:
            k.k.alu(a=('mov', t, 'unif'), comment='u%d' % i)
    k.live = frozenset()


def vc4_tmu_load(k, addr_src, dst, comment='tmu load'):
    """one 32-bit TMU read: the address write into tmu0_s enqueues the
    read, ldtmu0 stalls until the word lands in r4 (GPU_FFT protocol)"""
    k.alu(a=('add', 'tmu0_s', addr_src, '#0'), comment=comment + ': addr')
    k.k.alu(sig='ldtmu0', comment='ldtmu0')
    k.live = frozenset()
    k.alu(a=('mov', dst, 'r4'), comment=comment + ': = r4')


def vc4_store(k, pixel, tag):
    """VPM + VDW store of the 16 lane pixels.  Live regs: rf22 = VPM
    write setup (one 16-lane row, horizontal, 32-bit, Y=qid), rf23 =
    vdw_setup_0 base (ID=2 | HORIZ | qid<<7), rf11 = count (1..16),
    rf14 = xstart (absolute), rf4 = dst row base (x=0), rf24 = gx
    (current group's absolute x).  The VPM X field of the VDW setup
    word is only 4 bits wide (word units), so it must be the
    GROUP-LOCAL xstart - gx, not the absolute xstart.  The memory
    address, by contrast, is absolute: rf4 + gx*4 + hstart*4 (rf4 is
    the ROW base, vc4_bookkeep only adds 64 per group).  The count is
    the DEPTH field [22:16] (row length in memory words) with UNITS=1
    (one memory row): per the 3D guide Tables 34/35, UNITS is the row
    count and BLOCKMODE=0 steps the VPM Y origin per row, so putting
    count into UNITS would DMA garbage rows qid+1.. instead of our
    single row qid.
    NOTE: vpmvcd/vpmaddr are B-bank magic writes - qpuasm routes them
    with ws=1 so they hit VPMVCD_WR_SETUP/VPM_ST_ADDR (a ws=0 write
    would land in the LOAD side and the DMA would never fire)."""
    k.k.alu(a=('or', None, 'vw_wait', 'vw_wait'),
            comment='wait for the previous VDW DMA')
    k.alu(a=('mov', 'vpmvcd', 'rf22'),
            comment='%sVPM write setup (row qid)' % tag)
    k.alu(a=('mov', 'vpm', pixel),
            comment='%s16 lanes -> VPM row qid' % tag)
    k.k.alu(a=('or', None, 'vw_wait', 'vw_wait'),
            comment='QPU->VPM write must complete before the VDW setup'
                    ' (GPU_FFT store protocol)')
    k.alu(a=('mov', 'r1', 'rf11'), comment='count')
    k.alu(a=('shl', 'r1', 'r1', '#16'),
            comment='count -> DEPTH field [22:16] (row length, UNITS=1)')
    k.alu(a=('mov', 'r0', 'rf23'), comment='vdw setup0 base')
    k.alu(a=('or', 'r0', 'r0', 'r1'),
            comment='vdw_setup_0(UNITS=1, DEPTH=count, HORIZ | qid<<7)')
    k.alu(a=('mov', 'r3', 'rf24'), comment='gx (acc staging)')
    k.alu(a=('sub', 'r1', 'rf14', 'r3'),
            comment='hstart = xstart - gx (VPM X is 4 bits, group-local)')
    k.alu(a=('shl', 'r2', 'r1', '#3'), comment='hstart -> dma X field')
    k.alu(a=('or', 'r0', 'r0', 'r2'), comment='+ dma_h32(qid, hstart)')
    k.k.alu(a=('mov', 'vpmvcd', 'r0'), comment='vw_setup: basic (ID=2)')
    k.alu(a=('mov', 'vpmvcd', '#0xC0000000'),
            comment='vw_setup: stride (ID=3, moot with UNITS=1)')
    k.alu(a=('shl', 'r2', 'r1', '#2'), comment='hstart * 4')
    k.alu(a=('shl', 'r1', 'rf24', '#2'), comment='gx * 4')
    k.alu(a=('add', 'r1', 'r1', 'rf4'), comment='row base + gx*4')
    k.alu(a=('add', 'r1', 'r1', 'r2'),
            comment='DMA addr = row base + gx*4 + hstart*4')
    k.k.alu(a=('mov', 'vpmaddr', 'r1'), comment='vw_addr fires the DMA')


def vc4_clip_width(k):
    """count / hstart of the lanes of the current group inside the x
    clip rect.  Reads: rf24 = gx, rf5 = x0, rf6 = x1; writes rf11 =
    count (>= 1 - the VDW UNITS field encodes 1..16 directly) and
    rf14 = xstart = max(gx, x0) (ABSOLUTE x; vc4_store turns it into
    the group-local VPM X origin by subtracting gx)."""
    k.alu(a=('add', 'r0', 'rf24', '#16'), comment='gx + 16')
    k.alu(a=('min', 'r0', 'r0', 'rf6'), comment='up = min(gx+16, x1)')
    k.alu(a=('mov', 'r3', 'rf5'), comment='x0 (acc staging)')
    k.alu(a=('max', 'rf14', 'rf24', 'r3'), comment='x start = max(gx, x0)')
    k.alu(a=('sub', 'rf11', 'r0', 'rf14'), comment='count = up - x start')


def vc4_bookkeep(k, products=True):
    """group/row counters + gx/addr (and product) stepping as one
    flag-chain (no branches): the group-counter sub pushes N at row
    wrap and every wrap fix-up runs under ifn.  Live regs: rf13 = ctr,
    rf12 = L-1, rf24 = gx, rf25 = gx0, rf4 = row base, rf7 = rowjump,
    rf10 = rows; products: rf0/1 = u/v products, rf17/18 = pu16/pv16,
    rf19/20 = uxwrap/vxwrap.  ALL of it runs on the ADD pipe: the VC4
    MUL ALU has NO integer add/sub (op_mul 0/1 = NOP/FMUL - the
    decrement would silently compute a float product), and a second
    rf operand would read register file B (never-written garbage), so
    every second source is staged through r3."""
    k.alu(a=('add', 'rf4', 'rf4', '#64'), comment='row base += 64 (= one 16px group)')
    k.alu(a=('add', 'rf24', 'rf24', '#16'), comment='gx += 16')
    if products:
        k.alu(a=('mov', 'r3', 'rf17'), comment='pu16 (acc staging)')
        k.alu(a=('add', 'rf0', 'rf0', 'r3'), comment='u_prod += pu16')
        k.alu(a=('mov', 'r3', 'rf18'), comment='pv16 (acc staging)')
        k.alu(a=('add', 'rf1', 'rf1', 'r3'), comment='v_prod += pv16')
    k.alu(a=('sub', 'rf13', 'rf13', '#1'), flags={'sf': True},
          comment='group ctr -- (ADD pipe: MUL has no integer sub)')
    k.alu(a=('mov', 'rf13', 'rf12'), flags={'ac': 'ifn'},
          comment='reload L-1 at row wrap')
    k.alu(a=('mov', 'rf24', 'rf25'), flags={'ac': 'ifn'},
          comment='gx = gx0 at row wrap')
    k.alu(a=('mov', 'r3', 'rf7'), flags={'ac': 'ifn'},
          comment='rowjump (acc staging)')
    k.alu(a=('sub', 'rf4', 'rf4', 'r3'), flags={'ac': 'ifn'},
          comment='row base -= rowjump at wrap')
    if products:
        k.alu(a=('mov', 'r3', 'rf19'), flags={'ac': 'ifn'},
              comment='uxwrap (acc staging)')
        k.alu(a=('sub', 'rf0', 'rf0', 'r3'), flags={'ac': 'ifn'},
              comment='u_prod -= uxwrap at wrap')
        k.alu(a=('mov', 'r3', 'rf20'), flags={'ac': 'ifn'},
              comment='vxwrap (acc staging)')
        k.alu(a=('sub', 'rf1', 'rf1', 'r3'), flags={'ac': 'ifn'},
              comment='v_prod -= vxwrap at wrap')
    k.alu(a=('sub', 'rf10', 'rf10', '#1'), flags={'ac': 'ifn'},
          comment='rows -- at row wrap (ADD pipe)')


def vc4_rows_guard(k):
    """rows_q == 0 -> this QPU has nothing to do"""
    k.alu(a=('sub', 'r2', 'rf10', '#0'), flags={'sf': True},
          comment='rows_q == 0 ?')
    k.branch('gexit', 'allz', 'no rows for this QPU')
    k.nop(3, 'branch delay slots')


def vc4_lane_init(k):
    """rf31 = lane (elem_num, raddr 38 - a magic read, no residency)"""
    k.k.alu(a=('mov', 'rf31', 'elem'), comment='lane = elem_num')
    k.live = frozenset({31})


def vc4_prod_init(k, abs_reg, sign_imm, prod_reg, comment):
    """prod_reg += sign * mul24(|coef|, lane) - VC4's mul24 is UNSIGNED
    (uint24 x uint24 -> uint32), so negative coefficients arrive as
    |coef| plus a packed sign word (bit0 = u, bit1 = v) and the lane
    product gets a two's-complement flip when its sign bit is set.
    Staging: abs_reg = |coef|, rf30 = signs, rf31 = lane."""
    k.alu(a=('mov', 'r3', 'rf31'),
          comment='lane (acc staging: a 2nd rf operand would read file B)')
    k.alu(m=('mul24', 'r0', abs_reg, 'r3'), comment='%s: |coef|*lane' % comment)
    k.alu(a=('and', 'r2', 'rf30', sign_imm), flags={'sf': True},
          comment='%s: sign bit ?' % comment)
    k.alu(a=('not', 'r0', 'r0'), flags={'ac': 'ifnz'}, comment='-x - 1')
    k.alu(a=('add', 'r0', 'r0', '#1'), flags={'ac': 'ifnz'}, comment='-x')
    k.alu(a=('add', prod_reg, prod_reg, 'r0'),
          comment='%s: base + lane product' % comment)


def vc4_store_consts(k):
    """rf22 = 0x101A00|qid (VPM write setup, Table 32: ID=0, STRIDE=1,
    HORIZ, SIZE=2 (32-bit), ADDR=Y=qid), rf23 = 0x80804000 | qid<<7
    (VDW basic setup base, Table 34: ID=2, UNITS=1 (0 would mean 128
    rows of garbage!), DEPTH filled per group, HORIZ, VPM origin
    y=qid, MODEW=0 32-bit) - read from rf21 (qid, masked to 6 bits)"""
    k.alu(a=('mov', 'r2', '#0x101A00'), comment='vpm wr setup: h32(Y=0)')
    k.alu(a=('or', 'rf22', 'rf21', 'r2'), comment='vpm write setup word')
    k.alu(a=('shl', 'rf23', 'rf21', '#7'), comment='qid << 7 (dma Y)')
    k.alu(a=('mov', 'r2', '#0x80804000'), comment='')
    k.alu(a=('or', 'rf23', 'rf23', 'r2'), comment='vdw setup0 base')


def vc4_prologue(k, tg):
    """common prologue: uniform stream, rows guard, lane, store setup
    constants, gx = gx0 (both rf24/rf25), group ctr = L-1"""
    vc4_ldunif(k, tg)
    vc4_rows_guard(k)
    vc4_lane_init(k)
    vc4_store_consts(k)
    k.alu(a=('mov', 'rf24', 'rf25'), comment='gx = gx0')
    k.alu(a=('mov', 'rf13', 'rf12'),
          comment='group ctr = L-1 (MUST init: rf is power-on garbage,'
          ' a large value wedges the row loop forever)')


def vc4_fill():
    k = V21('argb_fill_vc4')
    # u: 0=color 1=qid 2=L-1 3=dst_row0 (band row 0, x=0) 4,5=x0,x1
    #    6=rowjump (L*64 - stride) 7=rows_q 8=gx0
    # rf0=color rf4=row base rf5/6=x0,x1 rf7=rowjump rf10=rows rf11=count
    # rf12=L-1 rf13=ctr rf14=hstart rf21=qid rf22/23=store constants
    # rf24=gx rf25=gx0
    vc4_prologue(k, ['rf0', 'rf21', 'rf12', 'rf4', 'rf5', 'rf6', 'rf7',
                     'rf10', 'rf25'])
    k.label('loop')
    vc4_clip_width(k)
    vc4_store(k, 'rf0', 'fill ')
    vc4_bookkeep(k, products=False)
    k.alu(a=('sub', 'r2', 'rf10', '#0'), flags={'sf': True},
          comment='rows == 0 ?')
    k.branch('loop', 'anynz', 'while rows != 0')
    k.nop(3, 'branch delay slots')
    k.k.alu(a=('or', None, 'vw_wait', 'vw_wait'),
            comment='wait for the last VDW DMA before exiting')
    k.exit()
    k.label('gexit')
    k.exit()
    return k.k


def vc4_affine_map(k):
    """rf26 = u = (u_prod >> 15) + cu, rf27 = v = (v_prod >> 15) + cv
    (the UN-shifted products live in rf0/rf1; rf2/rf3 = cu/cv)"""
    k.alu(a=('mov', 'r0', 'rf0'), comment='u_prod')
    k.alu(a=('asr', 'r0', 'r0', '#15'), comment='>> 15')
    k.alu(a=('add', 'rf26', 'r0', 'rf2'), comment='u')
    k.alu(a=('mov', 'r0', 'rf1'), comment='v_prod')
    k.alu(a=('asr', 'r0', 'r0', '#15'), comment='>> 15')
    k.alu(a=('add', 'rf27', 'r0', 'rf3'), comment='v')


def vc4_src_load(k):
    """TMU read of src[(v,u)] into rf26 with the clamped coordinates;
    rf27 keeps the ORIGINAL (pre-clamp) v for the rotate out-of-bounds
    mask, and the pre-clamp u is STASHED into rf28 before the load:
    the pixel landing in rf26 clobbers it (rf28 = |pu| is dead after
    vc4_prod_init).  The clamped pair sits in r0/r1."""
    k.alu(a=('max', 'r0', 'rf26', '#0'), comment='u clamp ...')
    k.alu(a=('min', 'r0', 'r0', 'rf15'), comment='... min(src_w-1)')
    k.alu(a=('max', 'r1', 'rf27', '#0'), comment='v clamp ...')
    k.alu(a=('min', 'r1', 'r1', 'rf16'), comment='... min(src_h-1)')
    k.alu(m=('mul24', 'r2', 'r1', 'rf8'), comment='v * src_w')
    k.alu(a=('add', 'r2', 'r2', 'r0'), comment='+ u')
    k.alu(a=('shl', 'r2', 'r2', '#2'), comment='* 4')
    k.alu(a=('add', 'r2', 'r2', 'rf9'), comment='+ src phys')
    k.alu(a=('mov', 'rf28', 'rf26'),
          comment='save u orig (the TMU pixel below clobbers rf26)')
    vc4_tmu_load(k, 'r2', 'rf26', 'src pixel')


def vc4_oob_mask(k):
    """rotate: zero the pixel when its PRE-clamp coordinate left the
    source surface.  Operand orientation matters: `src1 - u` goes
    negative exactly when u > src1 (equality stays in-bounds).  u orig
    lives in rf28 (stashed by vc4_src_load: rf26 now holds the pixel,
    v orig stays in rf27)."""
    k.alu(a=('mov', 'r2', 'r4'), comment='pixel')
    k.alu(a=('sub', 'r3', 'rf28', '#0'), flags={'sf': True}, comment='u < 0 ?')
    k.alu(a=('sub', 'r2', 'r2', 'r2'), flags={'ac': 'ifn'}, comment='-> 0')
    k.alu(a=('mov', 'r0', 'rf28'), comment='u orig (acc staging)')
    k.alu(a=('sub', 'r3', 'rf15', 'r0'), flags={'sf': True},
          comment='u > src_w-1 ?')
    k.alu(a=('sub', 'r2', 'r2', 'r2'), flags={'ac': 'ifn'}, comment='-> 0')
    k.alu(a=('sub', 'r3', 'rf27', '#0'), flags={'sf': True}, comment='v < 0 ?')
    k.alu(a=('sub', 'r2', 'r2', 'r2'), flags={'ac': 'ifn'}, comment='-> 0')
    k.alu(a=('mov', 'r0', 'rf27'), comment='v orig (acc staging)')
    k.alu(a=('sub', 'r3', 'rf16', 'r0'), flags={'sf': True},
          comment='v > src_h-1 ?')
    k.alu(a=('sub', 'r2', 'r2', 'r2'), flags={'ac': 'ifn'}, comment='-> 0')
    k.alu(a=('mov', 'rf26', 'r2'), comment='pixel (masked)')


def vc4_affine(rotate):
    k = V21('argb_rotate_vc4' if rotate else 'argb_blit_vc4')
    # u: 0,1=cu,cv 2,3=src_w-1,src_h-1 4=src_w 5=src_phys
    #    6=dst_row0 (band row 0, x=0) 7=L-1 8=rows_q 9,10=x0,x1 11=gx0
    #    12=qid 13=rowjump (L*64 - dst_stride)
    #    14,15=u_base,v_base (UN-shifted product at (gx0, y_start))
    #    16,17=|pu|,|pv| 18=packed signs (bit0 pu, bit1 pv)
    #    19,20=pu16,pv16 21,22=uxwrap,vxwrap 23=trailing (unused)
    # rf0/1=u/v products rf2/3=cu,cv rf4=row base rf5/6=x0,x1 rf7=rowjump
    # rf8=src_w rf9=src phys rf10=rows rf11=count rf12=L-1 rf13=ctr
    # rf14=hstart rf15/16=src_w-1,src_h-1 rf17..20=steps rf21=qid
    # rf22/23=store constants rf24=gx rf25=gx0 rf26/27=u/v orig (pixel
    # lands in rf26; u orig is stashed to rf28 = |pu|, dead after
    # vc4_prod_init)
    # u23 is a trailing unused word: loading it into rf28 here would
    # CLOBBER |pu| (u16) before vc4_prod_init reads it - consume and
    # drop it instead.
    tg = ['rf2', 'rf3', 'rf15', 'rf16', 'rf8', 'rf9', 'rf4', 'rf12',
          'rf10', 'rf5', 'rf6', 'rf25', 'rf21', 'rf7', 'rf0', 'rf1',
          'rf28', 'rf29', 'rf30', 'rf17', 'rf18', 'rf19', 'rf20', None]
    vc4_ldunif(k, tg)
    vc4_rows_guard(k)
    vc4_lane_init(k)
    # products: start from the host-computed base at (gx0, y_start) and
    # add sign(|coef| * lane) - mul24 is unsigned on VC4
    vc4_prod_init(k, 'rf28', '#1', 'rf0', 'u_prod')
    vc4_prod_init(k, 'rf29', '#2', 'rf1', 'v_prod')
    vc4_store_consts(k)
    k.alu(a=('mov', 'rf24', 'rf25'), comment='gx = gx0')
    k.alu(a=('mov', 'rf13', 'rf12'),
          comment='group ctr = L-1 (MUST init: rf is power-on garbage)')
    k.label('loop')
    vc4_affine_map(k)
    vc4_src_load(k)
    if rotate:
        vc4_oob_mask(k)
    vc4_clip_width(k)
    vc4_store(k, 'rf26', 'blit ')
    vc4_bookkeep(k)
    k.alu(a=('sub', 'r2', 'rf10', '#0'), flags={'sf': True},
          comment='rows == 0 ?')
    k.branch('loop', 'anynz', 'while rows != 0')
    k.nop(3, 'branch delay slots')
    k.k.alu(a=('or', None, 'vw_wait', 'vw_wait'),
            comment='wait for the last VDW DMA before exiting')
    k.exit()
    k.label('gexit')
    k.exit()
    return k.k


def vc4_alpha():
    k = V21('argb_alpha_vc4')
    # u: 0,1=cu,cv 2,3=src_w-1,src_h-1 4=src_w 5=src_phys 6=scratch
    #    7=dst_row0 (band row 0, x=0) 8=L-1 9=rows_q 10,11=x0,x1 12=gx0
    #    13=qid 14=global alpha 15,16=pu,qv (both >= 0: the alpha op is
    #    ROT_0 only) 17,18=u_base,v_base (product bases) 19=pu16
    #    20=uxwrap 21=vxwrap (= -qv, so "-=" advances v per row)
    #    22=rowjump
    # rf0/1=u/v products rf2/3=cu,cv rf4=row base rf5/6=x0,x1 rf7=rowjump
    # rf8=src_w rf9=src phys rf10=rows rf11=count rf12=L-1 rf13=ctr
    # rf14=hstart rf15/16=src_w-1,src_h-1 rf17=pu16 rf18=0 (pv16)
    # rf19/20=wraps rf21=qid rf22/23=store constants rf24=gx rf25=gx0
    # rf26=u (then out pixel) rf27=scratch lane addr rf28=alpha rf29=255
    # rf31=lane (then S)
    tg = ['rf2', 'rf3', 'rf15', 'rf16', 'rf8', 'rf9', 'rf27', 'rf4',
          'rf12', 'rf10', 'rf5', 'rf6', 'rf25', 'rf21', 'rf28', 'rf29',
          'rf30', 'rf0', 'rf1', 'rf17', 'rf19', 'rf20', 'rf7']
    vc4_ldunif(k, tg)
    vc4_rows_guard(k)
    vc4_lane_init(k)
    # u_prod(lane) = u_base + pu*lane (pu >= 0: no sign machinery);
    # v depends only on y - rf1 already holds the per-QPU v_base
    k.alu(a=('mov', 'r3', 'rf31'), comment='lane (acc staging)')
    k.alu(m=('mul24', 'r0', 'rf29', 'r3'), comment='u_prod: pu * lane')
    k.alu(a=('add', 'rf0', 'rf0', 'r0'), comment='u_prod = base + pu*lane')
    k.alu(a=('mov', 'rf18', '#0'), comment='pv16 = 0 (v is y-only)')
    # lane-private scratch word: scratch + qid*64 + lane*4
    k.alu(a=('shl', 'r0', 'rf21', '#6'), comment='qid * 64')
    k.alu(a=('add', 'rf27', 'rf27', 'r0'), comment='')
    k.alu(a=('shl', 'r0', 'rf31', '#2'), comment='lane * 4')
    k.alu(a=('add', 'rf27', 'rf27', 'r0'), comment='scratch lane addr')
    vc4_store_consts(k)
    k.alu(a=('mov', 'rf24', 'rf25'), comment='gx = gx0')
    k.alu(a=('mov', 'rf13', 'rf12'),
          comment='group ctr = L-1 (MUST init: rf is power-on garbage)')
    k.label('loop')
    # ---- src sample: u = (u_prod >> 15) + cu, v = broadcast rf1 ------
    k.alu(a=('mov', 'r0', 'rf0'), comment='u_prod')
    k.alu(a=('asr', 'r0', 'r0', '#15'), comment='>> 15')
    k.alu(a=('add', 'rf26', 'r0', 'rf2'), comment='u')
    k.alu(a=('mov', 'r1', 'rf1'), comment='v_prod (y only)')
    k.alu(a=('asr', 'r1', 'r1', '#15'), comment='>> 15')
    k.alu(a=('add', 'r1', 'r1', 'rf3'), comment='v')
    k.alu(a=('max', 'rf26', 'rf26', '#0'), comment='u clamp ...')
    k.alu(a=('mov', 'r0', 'rf15'), comment='src_w-1 (acc staging)')
    k.alu(a=('min', 'rf26', 'rf26', 'r0'), comment='... min(src_w-1)')
    k.alu(a=('max', 'r1', 'r1', '#0'), comment='v clamp ...')
    k.alu(a=('min', 'r1', 'r1', 'rf16'), comment='... min(src_h-1)')
    k.alu(m=('mul24', 'r0', 'r1', 'rf8'), comment='v * src_w')
    k.alu(a=('add', 'r0', 'r0', 'rf26'), comment='+ u')
    k.alu(a=('shl', 'r0', 'r0', '#2'), comment='* 4')
    k.alu(a=('add', 'r0', 'r0', 'rf9'), comment='+ src phys')
    k.alu(a=('add', 'r1', 'rf24', 'elem'),
          comment='x = gx + lane (elem: rf31 gets clobbered by S)')
    vc4_tmu_load(k, 'r0', 'rf31', 'S')
    # ---- dst sample: real dst pixel in-rect, scratch word otherwise --
    k.alu(a=('mov', 'r0', 'elem'), comment='lane (rf31 holds S)')
    k.alu(a=('shl', 'r0', 'r0', '#2'), comment='lane * 4')
    k.alu(a=('shl', 'r2', 'rf24', '#2'), comment='gx * 4')
    k.alu(a=('add', 'r0', 'r0', 'r2'), comment='(gx + lane) * 4')
    k.alu(a=('add', 'r0', 'r0', 'rf4'),
          comment='read addr = dst row base + x*4 (x = gx + lane)')
    k.alu(a=('sub', 'r2', 'r1', 'rf5'), flags={'sf': True},
          comment='x < x0 ?')
    k.alu(a=('mov', 'r0', 'rf27'), flags={'ac': 'ifn'}, comment='-> scratch')
    k.alu(a=('sub', 'r2', 'r1', 'rf6'), flags={'sf': True},
          comment='x >= x1 ?')
    k.alu(a=('mov', 'r0', 'rf27'), flags={'ac': 'ifnn'}, comment='-> scratch')
    vc4_tmu_load(k, 'r0', 'r3', 'D')
    # ---- blend in lerp form: out.c = D.c + ((S.c - D.c) * sa') >> 8.
    # No 255-mask register and no inv term: VC4 can't read two rf
    # registers in one instruction (the 2nd would hit file B) and
    # r4/r5 are not writable accumulators.  mul24 is unsigned 24-bit,
    # so the (S-D) difference wraps mod 2^24 - the product's low 32
    # bits are still the correct two's-complement value and asr>>8
    # keeps the sign.  sa' in r0, D in r3, pixel accumulates in r2.
    k.alu(a=('mov', 'r0', 'rf31'), comment='S')
    k.alu(a=('shr', 'r0', 'r0', '#16'), comment='S >> 16')
    k.alu(a=('shr', 'r0', 'r0', '#8'), comment='S >> 24')
    k.alu(m=('mul24', 'r0', 'r0', 'rf28'), comment='sa * global alpha')
    k.alu(a=('shr', 'r0', 'r0', '#8'), comment="sa'")
    k.alu(a=('mov', 'rf29', '#0xFF'), comment='255')
    # blue -> r2 (no #24 small-imm on VC4: 24-bit shifts go via #16+#8)
    k.alu(a=('shl', 'r2', 'rf31', '#16'), comment='S << 16 ...')
    k.alu(a=('shl', 'r2', 'r2', '#8'), comment='... << 8 (S << 24)')
    k.alu(a=('shr', 'r2', 'r2', '#16'), comment='S >> 16 ...')
    k.alu(a=('shr', 'r2', 'r2', '#8'), comment='... >> 8 (S.b)')
    k.alu(a=('shl', 'r1', 'r3', '#16'), comment='D << 16 ...')
    k.alu(a=('shl', 'r1', 'r1', '#8'), comment='... << 8 (D << 24)')
    k.alu(a=('shr', 'r1', 'r1', '#16'), comment='D >> 16 ...')
    k.alu(a=('shr', 'r1', 'r1', '#8'), comment='... >> 8 (D.b)')
    k.alu(a=('sub', 'r2', 'r2', 'r1'), comment='S.b - D.b')
    k.alu(m=('mul24', 'r2', 'r2', 'r0'), comment="* sa'")
    k.alu(a=('asr', 'r2', 'r2', '#8'), comment='>> 8')
    k.alu(a=('add', 'r2', 'r2', 'r1'), comment='out.b')
    # green -> r1 (folded into r2 << 8)
    k.alu(a=('shl', 'r1', 'rf31', '#16'), comment='S << 16')
    k.alu(a=('shr', 'r1', 'r1', '#16'), comment='S >> 16 ...')
    k.alu(a=('shr', 'r1', 'r1', '#8'), comment='... >> 8 (S.g)')
    k.alu(a=('shl', 'rf26', 'r3', '#16'), comment='D << 16')
    k.alu(a=('shr', 'rf26', 'rf26', '#16'), comment='D >> 16 ...')
    k.alu(a=('shr', 'rf26', 'rf26', '#8'), comment='... >> 8 (D.g)')
    k.alu(a=('sub', 'r1', 'r1', 'rf26'), comment='S.g - D.g')
    k.alu(m=('mul24', 'r1', 'r1', 'r0'), comment="* sa'")
    k.alu(a=('asr', 'r1', 'r1', '#8'), comment='>> 8')
    k.alu(a=('add', 'r1', 'r1', 'rf26'), comment='out.g')
    k.alu(a=('shl', 'r1', 'r1', '#8'), comment='<< 8')
    k.alu(a=('add', 'r2', 'r2', 'r1'), comment='b | g<<8')
    # red -> r1
    k.alu(a=('shl', 'r1', 'rf31', '#8'), comment='S << 8')
    k.alu(a=('shr', 'r1', 'r1', '#16'), comment='S >> 16 ...')
    k.alu(a=('shr', 'r1', 'r1', '#8'), comment='... >> 8 (S.r)')
    k.alu(a=('shr', 'rf26', 'r3', '#16'), comment='D >> 16')
    k.alu(a=('shl', 'rf26', 'rf26', '#16'), comment='D << 16 ...')
    k.alu(a=('shl', 'rf26', 'rf26', '#8'), comment='... << 8 (D.r << 24)')
    k.alu(a=('shr', 'rf26', 'rf26', '#16'), comment='D >> 16 ...')
    k.alu(a=('shr', 'rf26', 'rf26', '#8'), comment='... >> 8 (D.r)')
    k.alu(a=('sub', 'r1', 'r1', 'rf26'), comment='S.r - D.r')
    k.alu(m=('mul24', 'r1', 'r1', 'r0'), comment="* sa'")
    k.alu(a=('asr', 'r1', 'r1', '#8'), comment='>> 8')
    k.alu(a=('add', 'r1', 'r1', 'rf26'), comment='out.r')
    k.alu(a=('shl', 'r1', 'r1', '#16'), comment='<< 16')
    k.alu(a=('add', 'r2', 'r2', 'r1'), comment='b | g<<8 | r<<16')
    # alpha -> r3: da + ((255-da)*sa') >> 8
    k.alu(a=('shr', 'r3', 'r3', '#16'), comment='D >> 16 ...')
    k.alu(a=('shr', 'r3', 'r3', '#8'), comment='... >> 8 (da, <= 255)')
    k.alu(a=('sub', 'r1', 'rf29', 'r3'), comment='255 - da')
    k.alu(m=('mul24', 'r1', 'r1', 'r0'), comment="(255-da) * sa'")
    k.alu(a=('shr', 'r1', 'r1', '#8'), comment='>> 8')
    k.alu(a=('add', 'r3', 'r3', 'r1'), comment='out.a')
    k.alu(a=('shl', 'r3', 'r3', '#16'), comment='a << 16 ...')
    k.alu(a=('shl', 'r3', 'r3', '#8'), comment='... << 8 (a << 24)')
    k.alu(a=('add', 'r2', 'r2', 'r3'), comment='out pixel')
    k.alu(a=('mov', 'rf26', 'r2'), comment='')
    # ---- store + bookkeeping -------------------------------------------
    vc4_clip_width(k)
    vc4_store(k, 'rf26', 'alpha ')
    vc4_bookkeep(k)
    k.alu(a=('sub', 'r2', 'rf10', '#0'), flags={'sf': True},
          comment='rows == 0 ?')
    k.branch('loop', 'anynz', 'while rows != 0')
    k.nop(3, 'branch delay slots')
    k.k.alu(a=('or', None, 'vw_wait', 'vw_wait'),
            comment='wait for the last VDW DMA before exiting')
    k.exit()
    k.label('gexit')
    k.exit()
    return k.k


# --------------------------------------------------------------------------
# VC4 wedge-bisection diagnostics (the init-time probe block in v3d_g2d.c
# launches them one at a time; each isolates exactly one suspect feature
# of the real kernels against the known-good nop)
# --------------------------------------------------------------------------

DIAG_UNIF = ['rf0', 'rf1', 'rf2', 'rf3', 'rf4', 'rf5', 'rf6', 'rf7', 'rf8']


def vc4_diag_unif():
    """D1: uniform stream reads ONLY + pure thrend exit (no branch, no
    host write, no VPM).  Hangs -> the uniform fetch itself wedges."""
    k = V21('diag_unif_vc4')
    vc4_ldunif(k, DIAG_UNIF)
    k.nop(1, 'settle')
    k.k.raw(0x300009e7009e7000, 'nop; nop; thrend')
    k.k.nop(2, 'exit delay slots')
    return k.k


def vc4_diag_branch():
    """D2: setf + always-taken branch + thrend exit; no uniforms, no
    host write.  Hangs -> the branch/flag machinery wedges."""
    k = V21('diag_branch_vc4')
    k.alu(a=('mov', 'r0', '#0'), comment='r0 = 0')
    k.alu(a=('sub', 'r1', 'r0', 'r0'), flags={'sf': True},
          comment='setf: 0 - 0 (all lanes Z)')
    k.branch('done', 'allz', 'must be taken')
    k.nop(3, 'branch delay slots')
    k.alu(a=('mov', 'r1', '#1'), comment='SHOULD NEVER execute')
    k.label('done')
    k.k.raw(0x300009e7009e7000, 'nop; nop; thrend')
    k.k.nop(2, 'exit delay slots')
    return k.k


def vc4_diag_host():
    """D3: uniform reads + the production host-interrupt exit
    (mov r0,#1; mov host,r0; prog_end); no branch, no VPM.  Hangs ->
    the exit path (host write / prog_end) wedges."""
    k = V21('diag_host_vc4')
    vc4_ldunif(k, DIAG_UNIF)
    k.nop(1, 'settle')
    k.exit()
    return k.k


FILL_UNIF = ['rf0', 'rf21', 'rf12', 'rf4', 'rf5', 'rf6', 'rf7',
             'rf10', 'rf25']


def vc4_diag_fillfront():
    """D4: exact copy of the fill kernel front - uniform reads into
    the fill rf targets + the rows guard (sub.setf rf+small-imm, the
    LONG allz branch) + thrend fallback + gexit host exit.  Launched
    with rows_q == 0 it takes the branch into gexit; rows_q != 0
    falls through to the thrend fallback.  Hangs with rows_q == 0 ->
    the rf+smimm setf / long branch / branch-into-host-exit
    combination wedges (D1-D3 passed all three parts separately)."""
    k = V21('diag_fillfront_vc4')
    vc4_ldunif(k, FILL_UNIF)
    vc4_rows_guard(k)
    k.k.raw(0x300009e7009e7000, 'nop; nop; thrend (rows != 0 fallback)')
    k.k.nop(2, 'fallback exit delay slots')
    k.label('gexit')
    k.exit()
    return k.k


def vc4_diag_vdw():
    """D5: minimal VPM write + VDW DMA: fill front + lane init + store
    constants + ONE clipped 16px group store + vw_wait + thrend.  No
    loop, no bookkeeping, no host exit.  Launch with rows_q == 1 (the
    guard never takes).  Hangs -> the VPM/VDW setup words or vw_wait
    wedge (P2/P3 hang in exactly this region)."""
    k = V21('diag_vdw_vc4')
    vc4_ldunif(k, FILL_UNIF)
    vc4_rows_guard(k)          # never taken at rows_q=1
    vc4_lane_init(k)
    vc4_store_consts(k)
    k.alu(a=('mov', 'rf24', 'rf25'), comment='gx = gx0')
    vc4_clip_width(k)
    vc4_store(k, 'rf0', 'diag ')
    k.k.alu(a=('or', None, 'vw_wait', 'vw_wait'),
            comment='wait for the final VDW DMA')
    k.k.raw(0x300009e7009e7000, 'nop; nop; thrend')
    k.k.nop(2, 'exit delay slots')
    k.label('gexit')           # guard target, unreachable at rows_q=1
    k.k.raw(0x300009e7009e7000, 'nop; nop; thrend')
    k.k.nop(2, 'gexit delay slots')
    return k.k


def _diag_spin_tail(k):
    """shared tail for the W probes: fall-through == pass (exit),
    branch target == fail (deliberate spin).  The answer is encoded
    in COMPLETION because no memory observation channel is proven."""
    k.nop(3, 'branch delay slots')
    k.exit()
    k.label('spin')
    k.branch('spin', 'always', 'FAIL: deliberate wedge')
    k.nop(3, 'spin delay slots')


def vc4_diag_unifacc():
    """W1: does uniform word 0 (color) reach an ACCUMULATOR intact?
    mov r0, unif + acc-acc compare (D2-proven setf/branch form) -
    no register file involved at all."""
    k = V21('diag_unifacc_vc4')
    k.k.alu(a=('mov', 'r0', 'unif'), comment='u0 (color) -> r0')
    k.live = frozenset()
    k.alu(a=('mov', 'r1', '#0x12345678'), comment='expected (loadimm)')
    k.alu(a=('sub', 'r2', 'r0', 'r1'), flags={'sf': True},
          comment='acc-acc compare')
    k.branch('spin', 'anynz', 'mismatch -> wedge')
    _diag_spin_tail(k)
    return k.k


def vc4_diag_rfacca():
    """W2: register file round trip WITHOUT uniforms: loadimm -> r0
    -> rf20 (add-pipe write) -> back to r1 (single-operand mux-A
    read) -> acc-acc compare."""
    k = V21('diag_rfacca_vc4')
    k.alu(a=('mov', 'r0', '#0x12345678'), comment='known value (loadimm)')
    k.alu(a=('mov', 'rf20', 'r0'), comment='acc -> rf20 (add-pipe write)')
    k.nop(1, 'rf write -> read distance')
    k.alu(a=('mov', 'r1', 'rf20'), comment='rf20 -> acc (mux A)')
    k.alu(a=('sub', 'r2', 'r0', 'r1'), flags={'sf': True},
          comment='compare')
    k.branch('spin', 'anynz', 'mismatch -> wedge')
    _diag_spin_tail(k)
    return k.k


def vc4_diag_rfaccb():
    """W2b: file-split probe.  ra20 AND ra21 are both written (add
    pipe -> file A); the compare reads rf21 via mux A (file A) but
    rf20 via mux B (FILE B = rb20, never written).  TIMEOUT is the
    EXPECTED result on real VC4 (split regfile, like Mesa's model);
    completion would mean raddr_b aliases file A."""
    k = V21('diag_rfaccb_vc4')
    k.alu(a=('mov', 'r0', '#0x12345678'), comment='known value (loadimm)')
    k.alu(a=('mov', 'rf20', 'r0'), comment='ra20 = value')
    k.alu(a=('mov', 'rf21', 'r0'), comment='ra21 = value')
    k.nop(1, 'rf write -> read distance')
    k.alu(a=('sub', 'r2', 'rf21', 'rf20'), flags={'sf': True},
          comment='ra21 - rb20 (2nd rf operand reads file B)')
    k.branch('spin', 'anynz', 'file split confirmed -> wedge')
    _diag_spin_tail(k)
    return k.k


def vc4_diag_unifadv():
    """W3: uniform STREAM ADVANCE: consume 7 words, the 8th must be
    rows_q (the probe passes rows_q = 5).  Mismatch -> the stream
    pointer did not advance (or the values are wrong)."""
    k = V21('diag_unifadv_vc4')
    for i in range(7):
        k.k.alu(a=('mov', 'r3', 'unif'), comment='u%d -' % i)
    k.k.alu(a=('mov', 'r0', 'unif'), comment='u7 (rows_q) -> r0')
    k.live = frozenset()
    k.alu(a=('mov', 'r1', '#5'), comment='expected rows_q (loadimm)')
    k.alu(a=('sub', 'r2', 'r0', 'r1'), flags={'sf': True},
          comment='compare')
    k.branch('spin', 'anynz', 'mismatch -> wedge')
    _diag_spin_tail(k)
    return k.k


def vc4_diag_unifrf():
    """W4: the real 9-word FILL_UNIF front (uniform -> rf writes),
    then rf0 (color) back to an accumulator and compare."""
    k = V21('diag_unifrf_vc4')
    vc4_ldunif(k, FILL_UNIF)
    k.alu(a=('mov', 'r0', 'rf0'), comment='color back out of rf0')
    k.alu(a=('mov', 'r1', '#0x12345678'), comment='expected (loadimm)')
    k.alu(a=('sub', 'r2', 'r0', 'r1'), flags={'sf': True},
          comment='compare')
    k.branch('spin', 'anynz', 'mismatch -> wedge')
    _diag_spin_tail(k)
    return k.k


def vc4_diag_rowsrf():
    """W5: FILL_UNIF front, then rf10 (rows_q, the exact value the
    rows guard tests) back to an accumulator; compare against 1
    (the probe passes rows_q = 1)."""
    k = V21('diag_rowsrf_vc4')
    vc4_ldunif(k, FILL_UNIF)
    k.alu(a=('mov', 'r0', 'rf10'), comment='rows_q back out of rf10')
    k.alu(a=('mov', 'r1', '#1'), comment='expected rows_q (loadimm)')
    k.alu(a=('sub', 'r2', 'r0', 'r1'), flags={'sf': True},
          comment='compare')
    k.branch('spin', 'anynz', 'mismatch -> wedge')
    _diag_spin_tail(k)
    return k.k


def vc4_diag_ifn():
    """W6: conditional execution on the N flag (the bookkeep wrap
    mechanism).  5-6 sets N: a mov under ifn MUST fire (r3 = 9).
    6-5 clears N: a second ifn mov must NOT fire (r3 stays 7)."""
    k = V21('diag_ifn_vc4')
    k.alu(a=('mov', 'r0', '#5'), comment='')
    k.alu(a=('mov', 'r1', '#6'), comment='')
    k.alu(a=('sub', 'r2', 'r0', 'r1'), flags={'sf': True},
          comment='5-6 -> N set')
    k.alu(a=('mov', 'r3', '#7'), comment='marker (loadimm keeps flags)')
    k.alu(a=('mov', 'r3', '#9'), flags={'ac': 'ifn'},
          comment='MUST fire (N set)')
    k.alu(a=('mov', 'r0', '#9'), comment='')
    k.alu(a=('sub', 'r2', 'r3', 'r0'), flags={'sf': True},
          comment='r3 == 9 ?')
    k.branch('spin', 'anynz', 'ifn did not fire -> wedge')
    k.nop(3, 'branch delay slots')
    k.alu(a=('mov', 'r0', '#5'), comment='')
    k.alu(a=('sub', 'r2', 'r1', 'r0'), flags={'sf': True},
          comment='6-5 -> N clear')
    k.alu(a=('mov', 'r3', '#7'), comment='marker')
    k.alu(a=('mov', 'r3', '#9'), flags={'ac': 'ifn'},
          comment='must NOT fire (N clear)')
    k.alu(a=('mov', 'r0', '#7'), comment='')
    k.alu(a=('sub', 'r2', 'r3', 'r0'), flags={'sf': True},
          comment='r3 == 7 ?')
    k.branch('spin', 'anynz', 'ifn wrongly fired -> wedge')
    _diag_spin_tail(k)
    return k.k


# VPM store path bisection (the S probes stay all-sentinel with a
# clean ERRSTAT, so the store instructions are accepted but nothing
# reaches DRAM): RT proves or kills the QPU->VPM write by reading the
# written vector back; RTB controls the read path alone (two reads of
# the same vector must agree); RTD keeps the full VDW store but polls
# VPM_ST_BUSY to zero before exiting so a teardown-before-DMA race
# cannot hide a working store.  NOTE: the Round-4 RT/RTB compares
# used TWO rf operands - the assembler routes the second rf operand
# through raddr_b/register file B (never written), so both wedged on
# garbage regardless of the VPM path.  Every compare now stages the
# second operand through an accumulator first.  RTN/RTBN are the
# INVERSE-polarity twins (wedge on EQUAL): exactly one of each pair
# must complete, so two TIMEOUTs prove a true stall (not a wedge) and
# a wrong-polarity completion proves the compared data.

VPM_RW_SETUP = '#0x1A00'     # ID=0, NUM=0 (=>16), STRIDE=1, HORIZ, 32-bit,
                             # Y=0.  Round 5 used 0x101A00: bits 23:20 are
                             # NUM on the READ setup (Table 33), so the DMA
                             # delivered ONE vector while each probe makes
                             # four raddr-48 reads -> reads past the first
                             # never get data = the Round-5 RT timeouts.


def vc4_diag_rt():
    """RT: VPM write -> VPM read round trip (no VDW involved).
    Writes the color into VPM Y=0 through the ws=1 write setup, then
    reads the same vector back through a ws=0 read setup (regfile-A
    VPMVCD_RD_SETUP) + raddr 48 and compares.  Completes -> both VPM
    write and read work and the S-probe failure is VDW-side; TIMEOUT
    -> the VPM write is masked or the read path is broken (RTB/RTN
    disambiguate)."""
    k = V21('diag_rt_vc4')
    k.k.alu(a=('mov', 'rf0', 'unif'), comment='u0 color')
    k.k.alu(a=('mov', 'rf21', 'unif'), comment='u1 qid (stream shape)')
    k.live = frozenset()
    k.alu(a=('mov', 'vpmvcd', VPM_RW_SETUP),
          comment='VPM write setup h32(Y=0) [ws=1, VPMVCD_WR_SETUP]')
    k.alu(a=('mov', 'vpm', 'rf0'), comment='16 lanes -> VPM')
    k.nop(3, 'VPM write FIFO settle')
    k.alu(a=('mov', 'vr_setup', VPM_RW_SETUP),
          comment='VPM read setup NUM=16 h32(Y=0) [ws=0, RD_SETUP]')
    k.nop(3, 'read setup latency (guide: min 3 instructions)')
    k.k.alu(a=('mov', 'rf10', 'vpm'), comment='read vector 0 back')
    k.live = frozenset()
    k.alu(a=('mov', 'r1', 'rf0'),
          comment='color -> acc (a 2nd rf operand would read file B)')
    k.alu(a=('sub', 'r0', 'rf10', 'r1'), flags={'sf': True},
          comment='readback == color ?')
    k.branch('spin', 'anynz', 'mismatch -> wedge')
    _diag_spin_tail(k)
    return k.k


def vc4_diag_rtn():
    """RTN: inverse-polarity twin of RT - identical VPM write -> read
    round trip but wedges when the values ARE EQUAL (branch on allz).
    RT ok + RTN TIMEOUT -> round trip works, the S failure is
    VDW-side.  RT TIMEOUT + RTN ok -> reads complete but return the
    WRONG data (masked VPM write or stale window).  Both TIMEOUT ->
    a true VPM-read stall."""
    k = V21('diag_rtn_vc4')
    k.k.alu(a=('mov', 'rf0', 'unif'), comment='u0 color')
    k.k.alu(a=('mov', 'rf21', 'unif'), comment='u1 qid (stream shape)')
    k.live = frozenset()
    k.alu(a=('mov', 'vpmvcd', VPM_RW_SETUP),
          comment='VPM write setup h32(Y=0) [ws=1, VPMVCD_WR_SETUP]')
    k.alu(a=('mov', 'vpm', 'rf0'), comment='16 lanes -> VPM')
    k.nop(3, 'VPM write FIFO settle')
    k.alu(a=('mov', 'vr_setup', VPM_RW_SETUP),
          comment='VPM read setup NUM=16 h32(Y=0) [ws=0, RD_SETUP]')
    k.nop(3, 'read setup latency (guide: min 3 instructions)')
    k.k.alu(a=('mov', 'rf10', 'vpm'), comment='read vector 0 back')
    k.live = frozenset()
    k.alu(a=('mov', 'r1', 'rf0'),
          comment='color -> acc (a 2nd rf operand would read file B)')
    k.alu(a=('sub', 'r0', 'rf10', 'r1'), flags={'sf': True},
          comment='readback == color ?')
    k.branch('spin', 'allz', 'EQUAL -> wedge (inverse polarity)')
    _diag_spin_tail(k)
    return k.k


def vc4_diag_rtb():
    """RTB: control for RT - two consecutive reads of the SAME (never
    written) VPM vector must return identical data.  Completes -> the
    VPM read path is sane; TIMEOUT -> the read path itself is broken.
    RT fail + RTB pass isolates a masked VPM WRITE."""
    k = V21('diag_rtb_vc4')
    k.k.alu(a=('mov', 'rf0', 'unif'), comment='u0 color (stream shape)')
    k.k.alu(a=('mov', 'rf21', 'unif'), comment='u1 qid (stream shape)')
    k.live = frozenset()
    k.alu(a=('mov', 'vr_setup', VPM_RW_SETUP), comment='read setup 1')
    k.nop(3, 'read setup latency (guide: min 3 instructions)')
    k.k.alu(a=('mov', 'rf10', 'vpm'), comment='read 1')
    k.live = frozenset()
    k.alu(a=('mov', 'vr_setup', VPM_RW_SETUP), comment='read setup 2')
    k.nop(3, 'read setup latency')
    k.k.alu(a=('mov', 'rf11', 'vpm'), comment='read 2')
    k.live = frozenset()
    k.alu(a=('mov', 'r1', 'rf11'),
          comment='read2 -> acc (a 2nd rf operand would read file B)')
    k.alu(a=('sub', 'r0', 'rf10', 'r1'), flags={'sf': True},
          comment='read1 == read2 ?')
    k.branch('spin', 'anynz', 'unstable reads -> wedge')
    _diag_spin_tail(k)
    return k.k


def vc4_diag_rtbn():
    """RTBN: inverse-polarity twin of RTB - wedges when the two reads
    ARE EQUAL.  RTB ok + RTBN TIMEOUT -> reads are stable; RTB
    TIMEOUT + RTBN ok -> reads complete but unstable; both TIMEOUT ->
    a true VPM-read stall."""
    k = V21('diag_rtbn_vc4')
    k.k.alu(a=('mov', 'rf0', 'unif'), comment='u0 color (stream shape)')
    k.k.alu(a=('mov', 'rf21', 'unif'), comment='u1 qid (stream shape)')
    k.live = frozenset()
    k.alu(a=('mov', 'vr_setup', VPM_RW_SETUP), comment='read setup 1')
    k.nop(3, 'read setup latency (guide: min 3 instructions)')
    k.k.alu(a=('mov', 'rf10', 'vpm'), comment='read 1')
    k.live = frozenset()
    k.alu(a=('mov', 'vr_setup', VPM_RW_SETUP), comment='read setup 2')
    k.nop(3, 'read setup latency')
    k.k.alu(a=('mov', 'rf11', 'vpm'), comment='read 2')
    k.live = frozenset()
    k.alu(a=('mov', 'r1', 'rf11'),
          comment='read2 -> acc (a 2nd rf operand would read file B)')
    k.alu(a=('sub', 'r0', 'rf10', 'r1'), flags={'sf': True},
          comment='read1 == read2 ?')
    k.branch('spin', 'allz', 'EQUAL -> wedge (inverse polarity)')
    _diag_spin_tail(k)
    return k.k


def vc4_diag_rtd():
    """RTD: the diag_vdw store plus a VPM_ST_BUSY poll before exit.
    After vw_addr fires the DMA, raddr_b 49 read through regfile B
    (Table 31 'B rd' = VPM_ST_BUSY) is polled until zero - an
    unbounded loop: a HANG here is itself the answer (DMA started but
    never completes).  Completes with pixels still missing -> busy
    read 0 from the outset, the DMA never started; completes WITH
    pixels -> the vw_wait-only tail of the S probe exited before the
    store landed."""
    k = V21('diag_rtd_vc4')
    vc4_ldunif(k, FILL_UNIF)
    vc4_rows_guard(k)          # never taken at rows_q=1
    vc4_lane_init(k)
    vc4_store_consts(k)
    k.alu(a=('mov', 'rf24', 'rf25'), comment='gx = gx0')
    vc4_clip_width(k)
    vc4_store(k, 'rf0', 'diag ')
    # poll VPM_ST_BUSY until the store DMA goes idle
    k.label('poll')
    k.k.nop_reads(39, 49, comment='raddr_b=49 residency')
    k.k.alu(a=('mov', 'r0', 'vw_busy'), comment='VPM_ST_BUSY -> r0')
    k.alu(a=('sub', 'r2', 'r0', '#0'), flags={'sf': True},
          comment='busy == 0 ?')
    k.branch('poll', 'anynz', 'still busy -> keep polling')
    k.nop(3, 'poll exit delay slots')
    k.k.alu(a=('or', None, 'vw_wait', 'vw_wait'),
            comment='wait for the final VDW DMA')
    k.k.raw(0x300009e7009e7000, 'nop; nop; thrend')
    k.k.nop(2, 'exit delay slots')
    k.label('gexit')           # guard target, unreachable at rows_q=1
    k.k.raw(0x300009e7009e7000, 'nop; nop; thrend')
    k.k.nop(2, 'gexit delay slots')
    return k.k


def _vc4_rtg_body(k, vdw_basic):
    """Shared GPU_FFT-shape store body for RTG/RT2: 16 vertical VPM
    vector writes (column i gets color+i so every transferred word is
    DISTINCT - the driver scan then shows exactly which VPM words
    landed where) + one VDW kick with `vdw_basic` + stride 0xc0000040.
    Round 5's RTG wrote only row 0 visible at w[0..15] - but Table 35
    defines STRIDE as the DISTANCE BETWEEN THE LAST BYTE OF A ROW AND
    THE START OF THE NEXT: with 64-byte rows 0xc0000040 may be a 128B
    row PITCH (rows at w0, w32, w64...) and w[16] was padding.  The
    distinct colors + the driver's full-buffer scan settle rows-vs-
    pitch-vs-VPM-mapping in one boot."""
    vc4_ldunif(k, FILL_UNIF)
    k.k.alu(a=('or', None, 'vw_wait', 'vw_wait'),
            comment='drain any prior VDW DMA')
    k.alu(a=('mov', 'vpmvcd', '#0x00101200'),
          comment='GPU_FFT vpm wr setup: v32 vertical stride 1, col 0')
    k.alu(a=('mov', 'vpm', 'rf0'), comment='column 0: color+0')
    for i in range(1, 16):
        k.alu(a=('add', 'r0', 'rf0', '#%d' % i),
              comment='color + %d (single rf operand)' % i)
        k.alu(a=('mov', 'vpm', 'r0'), comment='column %d' % i)
    k.k.alu(a=('or', None, 'vw_wait', 'vw_wait'),
            comment='QPU->VPM writes must complete before the VDW setup'
                    ' (GPU_FFT store protocol)')
    k.alu(a=('mov', 'vpmvcd', vdw_basic),
          comment='vdw_setup_0 basic (ID=2)')
    k.alu(a=('mov', 'vpmvcd', '#0xC0000040'),
          comment='vdw stride 0x40 (ID=3): pitch 64 or 128?')
    k.alu(a=('mov', 'r1', 'rf4'), comment='dst base (acc staging)')
    k.k.alu(a=('mov', 'vpmaddr', 'r1'), comment='vw_addr fires the DMA')
    k.k.alu(a=('or', None, 'vw_wait', 'vw_wait'),
            comment='wait for the VDW DMA')
    k.k.raw(0x300009e7009e7000, 'nop; nop; thrend')
    k.k.nop(2, 'exit delay slots')


def vc4_diag_rtg():
    """RTG: the GPU_FFT store shape verbatim (known-good silicon
    protocol): VPM wr setup 0x00101200 (vertical stride 1) + 16
    back-to-back vector writes (column i = color+i) + vdw_setup_0
    0x88104000 (ID=2, UNITS=16, DEPTH=16, HORIZ, VPM origin 0) +
    stride 0xc0000040 + vw_addr = dst.  The driver scans all 256
    words: runs at w0/w16/w32... = pitch 64 (16 rows), runs at
    w0/w32/w64... = pitch 128 (stride is a GAP, Table 35), all
    sentinel = VDW dead."""
    k = V21('diag_rtg_vc4')
    _vc4_rtg_body(k, '#0x88104000')
    return k.k


def vc4_diag_rt2():
    """RT2: RTG with UNITS=1 (0x80904000: DEPTH=16, HORIZ, VPM origin
    0) - the ONLY field difference vs RTG, bisecting the production
    store's UNITS=1 word (RTD wrote ZERO bytes).  Row 0 of the buffer
    must show color+0..color+15: RT2 ok -> UNITS=1 works and RTD's
    failure is its runtime-computed word or horiz VPM write setup;
    RT2 all-sentinel -> UNITS=1 itself never fires a DMA here."""
    k = V21('diag_rt2_vc4')
    _vc4_rtg_body(k, '#0x80904000')
    return k.k


# --------------------------------------------------------------------------
# header emission
# --------------------------------------------------------------------------

HEADER_DOC = """\
 *
 * V3D 4.2 CSD kernels (argb_fill4, argb_copy, argb_scale_pow2,
 * argb_alpha, argb_rot90, argb_rotate, argb_blit, argb_fill): 1:1
 * binary translations of the hardware-proven raspi5 (V3D 7.1)
 * kernels.  Semantics, register usage and the uniform contracts are
 * identical to raspi5 - see the dispatch sites in bsp_g2d.c (banded
 * launch: qid = (tidx>>2)&15 selects the row band, lanes outside the
 * clip rect store to the scratch sink, thread end via
 * barrierid/syncb + tmuwt + double thrsw).
 *
 * VC4 (V3D 2.1) SRQ kernels - one thread per QPU, launched with the
 * GPU_FFT SRQ protocol (no CSD).  qid selects the row band
 * (rows_q = h / nq, y_start = qid*rows_q); gx0 = x0 & ~15 anchors the
 * 16-lane groups, L = (x1-gx0+15)/16 groups per row.  Uniforms are
 * PER-QPU (each QPU gets its own stream at unifs + 32*qid words;
 * every word beyond the contract is unused).  Products u_prod/v_prod
 * carry pu*X + qu*Y UN-shifted; the host pre-computes the band base
 * (u_base = pu*gx0 + qu*y_start) and the 16-lane step (pu16 = pu*16);
 * vc4_lane_init adds mul24(|pu|, lane) with the host-supplied absolute
 * value and sign word (mul24 is unsigned).
 *
 * Uniform contract per QPU (uint32 words, see bsp_g2d.c):
 *   argb_fill_vc4 (9): 0=color 1=qid 2=L-1 3=dst_row0 4,5=x0,x1
 *     6=rowjump (L*64-stride) 7=rows_q 8=gx0
 *   argb_blit_vc4 / argb_rotate_vc4 (24): 0,1=cu,cv 2,3=src_w-1,src_h-1
 *     4=src_w 5=src_phys 6=dst_row0 7=L-1 8=rows_q 9,10=x0,x1 11=gx0
 *     12=qid 13=rowjump 14,15=u_base,v_base (UN-shifted)
 *     16,17=|pu|,|pv| 18=signs (bit0 pu, bit1 pv) 19,20=pu16,pv16
 *     21,22=uxwrap (pu*16*L-qu), vxwrap (pv*16*L-qv) 23=unused
 *   argb_alpha_vc4 (23): 0,1=cu,cv 2,3=src_w-1,src_h-1 4=src_w
 *     5=src_phys 6=scratch 7=dst_row0 8=L-1 9=rows_q 10,11=x0,x1
 *     12=gx0 13=qid 14=alpha 15,16=pu,qv (>= 0) 17,18=u_base,v_base
 *     19=pu16 20=uxwrap 21=vxwrap (=-qv) 22=rowjump
"""


def emit(kernels, path):
    lines = []
    lines.append('/*')
    lines.append(' * g2d_qpu_kernels.h - ARGB8888 QPU kernels for the raspix'
                 ' bsp_g2d')
    lines.append(' * back end: eight CSD kernels translated 1:1 from the'
                 ' raspi5')
    lines.append(' * (V3D 7.1) binaries to the V3D 4.2 (BCM2711) QPU ISA,'
                 ' plus')
    lines.append(' * four SRQ kernels for the VC4 (V3D 2.1, BCM2837) QPU ISA'
                 ' and')
    lines.append(' * nineteen tiny VC4 wedge-bisection diagnostics'
                 ' (diag_*_vc4).')
    lines.append(' * GENERATED by tools/gen_kernels.py - edit that script,'
                 ' not this file.')
    for ln in HEADER_DOC.split('\n'):
        lines.append(ln if ln.startswith(' *') else ' *')
    lines.append(' */')
    lines.append('')
    lines.append('#ifndef G2D_QPU_KERNELS_H')
    lines.append('#define G2D_QPU_KERNELS_H')
    lines.append('')
    lines.append('#include <stdint.h>')
    lines.append('')
    for name, words in kernels:
        lines.append('/* %s: %d instructions */' % (name, len(words)))
        lines.append('const uint64_t g2d_qpu_%s[] = {' % name)
        for i in range(0, len(words), 4):
            chunk = words[i:i + 4]
            lines.append('    ' + ', '.join('0x%016xULL' % w
                                            for w in chunk) + ',')
        lines.append('};')
        lines.append('const unsigned g2d_qpu_%s_n = %d;' % (name, len(words)))
        lines.append('')
    lines.append('#endif /* G2D_QPU_KERNELS_H */')
    lines.append('')
    with open(path, 'w') as f:
        f.write('\n'.join(lines))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), '..',
        'g2d_qpu_kernels.h')
    kernels = []
    # V3D 4.2 CSD kernels: 1:1 binary translations of the raspi5
    # (V3D 7.1) hardware-proven kernels
    for name, words in translate_71_to_42.translate_all(RASPI5_KERNELS_H):
        if len(words) > MAX_WORDS:
            raise SystemExit('%s: %d words > CSD_CODE_WORDS' %
                             (name, len(words)))
        kernels.append((name, words))
        print('=== %s: %d instructions (translated from raspi5) ==='
              % (name, len(words)))
        for i, w in enumerate(words):
            print('%4d: %016x  %s' % (i, w, decode(w, i, 42)))
        print('')
    for build in (vc4_fill,
                  lambda: vc4_affine(False),
                  lambda: vc4_affine(True),
                  vc4_alpha,
                  vc4_diag_unif,
                  vc4_diag_branch,
                  vc4_diag_host,
                  vc4_diag_fillfront,
                  vc4_diag_vdw,
                  vc4_diag_unifacc,
                  vc4_diag_rfacca,
                  vc4_diag_rfaccb,
                  vc4_diag_unifadv,
                  vc4_diag_unifrf,
                  vc4_diag_rowsrf,
                  vc4_diag_ifn,
                  vc4_diag_rt,
                  vc4_diag_rtb,
                  vc4_diag_rtd,
                  vc4_diag_rtn,
                  vc4_diag_rtbn,
                  vc4_diag_rtg,
                  vc4_diag_rt2):
        k = build()
        rows = k.finish()
        words = [w for w, c in rows]
        if len(words) > MAX_WORDS:
            raise SystemExit('%s: %d words > CSD_CODE_WORDS' %
                             (k.name, len(words)))
        kernels.append((k.name, words))
        print('=== %s: %d instructions ===' % (k.name, len(words)))
        is21 = k.name.endswith('_vc4')
        for i, (w, c) in enumerate(rows):
            d = decode21(w, i) if is21 else decode(w, i, 42)
            print('%4d: %016x  %-46s | %s' % (i, w, d, c))
        print('')
    emit(kernels, os.path.normpath(out))
    print('wrote %s' % os.path.normpath(out))


if __name__ == '__main__':
    main()
