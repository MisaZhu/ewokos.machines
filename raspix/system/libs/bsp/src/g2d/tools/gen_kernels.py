# gen_kernels.py - build the four V3D 4.2 (BCM2711) CSD kernels and the
# four VC4 (V3D 2.1, BCM2837) SRQ kernels for the raspix bsp_g2d back
# end and emit g2d_qpu_kernels.h.
#
# The algorithms are re-implementations of the raspi5 (V3D 7.1) kernels
# decoded with qpuasm.py, adapted to the VC4-generation ISA:
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

MAX_WORDS = 256   # CSD_CODE_WORDS in v3d_g2d.c


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
    """VPM + VDW store of the 16 lane pixels (GPU_FFT protocol).
    Live regs: rf22 = VPM write setup (vpm_setup(1,1,h32(qid)): one
    16-lane row, horizontal, 32-bit), rf23 = vdw_setup_0 base
    (UNITS=1 | HORIZ | qid<<7),
    rf11 = count (1..16), rf14 = xstart (absolute), rf4 = dst row base
    (x=0).  The VPM X field of the VDW setup word is only 4 bits wide
    (word units), so it must be the GROUP-LOCAL xstart - gx, not the
    absolute xstart."""
    k.k.alu(a=('or', None, 'vw_wait', 'vw_wait'),
            comment='wait for the previous VDW DMA')
    k.alu(a=('mov', 'vpmvcd', 'rf22'),
            comment='%sVPM write setup (row qid)' % tag)
    k.alu(a=('mov', 'vpm', pixel),
            comment='%s16 lanes -> VPM row qid' % tag)
    k.k.alu(a=('or', None, 'vw_wait', 'vw_wait'),
            comment='QPU->VPM write must complete before the VDW setup'
                    ' (GPU_FFT store protocol)')
    k.alu(a=('mov', 'r0', 'rf23'), comment='vdw setup0 base')
    k.alu(a=('shl', 'r1', 'rf11', '#16'), comment='count -> DEPTH field')
    k.alu(a=('or', 'r0', 'r0', 'r1'), comment='')
    k.alu(a=('sub', 'r1', 'rf14', 'rf24'),
            comment='hstart = xstart - gx (VPM X is 4 bits, group-local)')
    k.alu(a=('shl', 'r1', 'r1', '#3'), comment='hstart -> dma X field')
    k.alu(a=('or', 'r0', 'r0', 'r1'),
            comment='vdw_setup_0(1, count, dma_h32(qid, hstart))')
    k.k.alu(a=('mov', 'vpmvcd', 'r0'), comment='vw_setup: basic (ID=2)')
    k.alu(a=('mov', 'vpmvcd', '#0xC0000000'),
            comment='vw_setup: stride (ID=3, moot with UNITS=1)')
    k.alu(a=('shl', 'r1', 'rf14', '#2'), comment='hstart * 4')
    k.alu(a=('add', 'r1', 'r1', 'rf4'), comment='DMA addr = row base + x')
    k.k.alu(a=('mov', 'vpmaddr', 'r1'), comment='vw_addr fires the DMA')


def vc4_clip_width(k):
    """count / hstart of the lanes of the current group inside the x
    clip rect.  Reads: rf24 = gx, rf5 = x0, rf6 = x1; writes rf11 =
    count (>= 1 - the VDW depth field encodes 1..16 directly) and
    rf14 = xstart = max(gx, x0) (ABSOLUTE x; vc4_store turns it into
    the group-local VPM X origin by subtracting gx)."""
    k.alu(a=('add', 'r0', 'rf24', '#16'), comment='gx + 16')
    k.alu(a=('min', 'r0', 'r0', 'rf6'), comment='up = min(gx+16, x1)')
    k.alu(a=('max', 'rf14', 'rf24', 'rf5'), comment='x start = max(gx, x0)')
    k.alu(a=('sub', 'rf11', 'r0', 'rf14'), comment='count = up - x start')


def vc4_bookkeep(k, products=True):
    """group/row counters + gx/addr (and product) stepping as one
    flag-chain (no branches): the group-counter sub pushes N at row
    wrap and every wrap fix-up runs under ifn.  Live regs: rf13 = ctr,
    rf12 = L-1, rf24 = gx, rf25 = gx0, rf4 = row base, rf7 = rowjump,
    rf10 = rows; products: rf0/1 = u/v products, rf17/18 = pu16/pv16,
    rf19/20 = uxwrap/vxwrap."""
    k.alu(a=('add', 'rf4', 'rf4', '#64'), comment='row base += 64')
    k.alu(a=('add', 'rf24', 'rf24', '#16'), comment='gx += 16')
    if products:
        k.alu(a=('add', 'rf0', 'rf0', 'rf17'), comment='u_prod += pu16')
        k.alu(a=('add', 'rf1', 'rf1', 'rf18'), comment='v_prod += pv16')
    k.alu(m=('sub', 'rf13', 'rf13', '#1'), flags={'sf': True},
          comment='group ctr --')
    k.alu(a=('mov', 'rf13', 'rf12'), flags={'ac': 'ifn'},
          comment='reload L-1 at row wrap')
    k.alu(a=('mov', 'rf24', 'rf25'), flags={'ac': 'ifn'},
          comment='gx = gx0 at row wrap')
    k.alu(a=('sub', 'rf4', 'rf4', 'rf7'), flags={'ac': 'ifn'},
          comment='row base -= rowjump at wrap')
    if products:
        k.alu(a=('sub', 'rf0', 'rf0', 'rf19'), flags={'ac': 'ifn'},
              comment='u_prod -= uxwrap at wrap')
        k.alu(a=('sub', 'rf1', 'rf1', 'rf20'), flags={'ac': 'ifn'},
              comment='v_prod -= vxwrap at wrap')
    k.alu(m=('sub', 'rf10', 'rf10', '#1'), flags={'mc': 'ifn'},
          comment='rows -- at row wrap')


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
    k.alu(m=('mul24', 'r0', abs_reg, 'rf31'), comment='%s: |coef|*lane' % comment)
    k.alu(a=('and', 'r2', 'rf30', sign_imm), flags={'sf': True},
          comment='%s: sign bit ?' % comment)
    k.alu(a=('not', 'r0', 'r0'), flags={'ac': 'ifnz'}, comment='-x - 1')
    k.alu(a=('add', 'r0', 'r0', '#1'), flags={'ac': 'ifnz'}, comment='-x')
    k.alu(a=('add', prod_reg, prod_reg, 'r0'),
          comment='%s: base + lane product' % comment)


def vc4_store_consts(k):
    """rf22 = 0x101A00|qid (VPM write setup: vpm_setup(1, 1, h32(qid)) -
    vc4.qinc lays out num[23:20]|stride[17:12]|dma[11:0]; num=1 counts
    the single 16-lane vector row, stride=1, h32 = H|Size=2 (32-bit) |
    Y.  num MUST NOT be 0: it is the write-block row count the VPM
    write state machine (and vw_wait) tracks, so a zero wedges any
    vw_wait forever), rf23 = 0x80804000 | qid<<7 (VDW basic setup base:
    ID=2, UNITS=1, DEPTH filled per group, HORIZ, VPM origin y=qid) -
    read from rf21 (qid, already masked to 6 bits)"""
    k.alu(a=('mov', 'r2', '#0x101A00'), comment='vpm_setup(1,1,h32(0))')
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
    rf26/rf27 keep the ORIGINAL (pre-clamp) u/v for the rotate
    out-of-bounds mask, the clamped pair sits in r0/r1"""
    k.alu(a=('max', 'r0', 'rf26', '#0'), comment='u clamp ...')
    k.alu(a=('min', 'r0', 'r0', 'rf15'), comment='... min(src_w-1)')
    k.alu(a=('max', 'r1', 'rf27', '#0'), comment='v clamp ...')
    k.alu(a=('min', 'r1', 'r1', 'rf16'), comment='... min(src_h-1)')
    k.alu(m=('mul24', 'r2', 'r1', 'rf8'), comment='v * src_w')
    k.alu(a=('add', 'r2', 'r2', 'r0'), comment='+ u')
    k.alu(a=('shl', 'r2', 'r2', '#2'), comment='* 4')
    k.alu(a=('add', 'r2', 'r2', 'rf9'), comment='+ src phys')
    vc4_tmu_load(k, 'r2', 'rf26', 'src pixel')


def vc4_oob_mask(k):
    """rotate: zero the pixel when its PRE-clamp coordinate left the
    source surface.  Operand orientation matters: `src1 - u` goes
    negative exactly when u > src1 (equality stays in-bounds)."""
    k.alu(a=('mov', 'r2', 'r4'), comment='pixel')
    k.alu(a=('sub', 'r3', 'rf26', '#0'), flags={'sf': True}, comment='u < 0 ?')
    k.alu(a=('sub', 'r2', 'r2', 'r2'), flags={'ac': 'ifn'}, comment='-> 0')
    k.alu(a=('sub', 'r3', 'rf15', 'rf26'), flags={'sf': True},
          comment='u > src_w-1 ?')
    k.alu(a=('sub', 'r2', 'r2', 'r2'), flags={'ac': 'ifn'}, comment='-> 0')
    k.alu(a=('sub', 'r3', 'rf27', '#0'), flags={'sf': True}, comment='v < 0 ?')
    k.alu(a=('sub', 'r2', 'r2', 'r2'), flags={'ac': 'ifn'}, comment='-> 0')
    k.alu(a=('sub', 'r3', 'rf16', 'rf27'), flags={'sf': True},
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
    # rf22/23=store constants rf24=gx rf25=gx0 rf26/27=u/v orig (pixel)
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
    k.alu(m=('mul24', 'r0', 'rf29', 'rf31'), comment='u_prod: pu * lane')
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
    k.alu(a=('min', 'rf26', 'rf26', 'rf15'), comment='... min(src_w-1)')
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
    k.alu(a=('add', 'r0', 'r0', 'rf4'),
          comment='read addr = dst row base + lane*4')
    k.alu(a=('sub', 'r2', 'r1', 'rf5'), flags={'sf': True},
          comment='x < x0 ?')
    k.alu(a=('mov', 'r0', 'rf27'), flags={'ac': 'ifn'}, comment='-> scratch')
    k.alu(a=('sub', 'r2', 'r1', 'rf6'), flags={'sf': True},
          comment='x >= x1 ?')
    k.alu(a=('mov', 'r0', 'rf27'), flags={'ac': 'ifnn'}, comment='-> scratch')
    vc4_tmu_load(k, 'r0', 'r3', 'D')
    # ---- blend: out.c = (S.c*sa' + D.c*(255-sa')) >> 8 (over-alpha) --
    k.alu(a=('mov', 'r0', 'rf31'), comment='S')
    k.alu(a=('shr', 'r0', 'r0', '#16'), comment='S >> 16')
    k.alu(a=('shr', 'r0', 'r0', '#8'), comment='S >> 24')
    k.alu(m=('mul24', 'r0', 'r0', 'rf28'), comment='sa * global alpha')
    k.alu(a=('shr', 'r0', 'r0', '#8'), comment="sa'")
    k.alu(a=('mov', 'rf29', '#0xFF'), comment='255')
    k.alu(a=('sub', 'r1', 'rf29', 'r0'), comment="inv = 255 - sa'")
    # blue -> r2
    k.alu(a=('and', 'r2', 'rf31', 'rf29'), comment='S.b')
    k.alu(m=('mul24', 'r2', 'r2', 'r0'), comment="S.b * sa'")
    k.alu(a=('and', 'rf26', 'r3', 'rf29'), comment='D.b')
    k.alu(m=('mul24', 'rf26', 'rf26', 'r1'), comment='D.b * inv')
    k.alu(a=('add', 'r2', 'r2', 'rf26'), comment='')
    k.alu(a=('shr', 'r2', 'r2', '#8'), comment='out.b')
    # green -> rf26 (folds into r2 << 8)
    k.alu(a=('shr', 'rf26', 'rf31', '#8'), comment='S >> 8')
    k.alu(a=('and', 'rf26', 'rf26', 'rf29'), comment='S.g')
    k.alu(m=('mul24', 'rf26', 'rf26', 'r0'), comment="S.g * sa'")
    k.alu(a=('shr', 'r3', 'r3', '#8'), comment='D >> 8')
    k.alu(a=('and', 'r3', 'r3', 'rf29'), comment='D.g')
    k.alu(m=('mul24', 'r3', 'r3', 'r1'), comment='D.g * inv')
    k.alu(a=('add', 'rf26', 'rf26', 'r3'), comment='')
    k.alu(a=('shr', 'rf26', 'rf26', '#8'), comment='out.g')
    k.alu(a=('shl', 'rf26', 'rf26', '#8'), comment='<< 8')
    k.alu(a=('add', 'r2', 'r2', 'rf26'), comment='b | g<<8')
    # red -> rf26
    k.alu(a=('shr', 'rf26', 'rf31', '#16'), comment='S >> 16')
    k.alu(a=('and', 'rf26', 'rf26', 'rf29'), comment='S.r')
    k.alu(m=('mul24', 'rf26', 'rf26', 'r0'), comment="S.r * sa'")
    k.alu(a=('shr', 'r3', 'r3', '#8'), comment='D >> 16')
    k.alu(a=('and', 'r3', 'r3', 'rf29'), comment='D.r')
    k.alu(m=('mul24', 'r3', 'r3', 'r1'), comment='D.r * inv')
    k.alu(a=('add', 'rf26', 'rf26', 'r3'), comment='')
    k.alu(a=('shr', 'rf26', 'rf26', '#8'), comment='out.r')
    # alpha -> r3: da + ((255-da)*sa') >> 8
    k.alu(a=('shr', 'r3', 'r3', '#8'), comment='D >> 24')
    k.alu(a=('and', 'r3', 'r3', 'rf29'), comment='da')
    k.alu(a=('sub', 'r1', 'rf29', 'r3'), comment='255 - da')
    k.alu(m=('mul24', 'r1', 'r1', 'r0'), comment="(255-da) * sa'")
    k.alu(a=('shr', 'r1', 'r1', '#8'), comment='>> 8')
    k.alu(a=('add', 'r3', 'r3', 'r1'), comment='out.a')
    # compose -> rf26
    k.alu(a=('shl', 'r3', 'r3', '#16'), comment='a << 16')
    k.alu(a=('shl', 'r3', 'r3', '#8'), comment='a << 24')
    k.alu(a=('shl', 'rf26', 'rf26', '#16'), comment='r << 16')
    k.alu(a=('add', 'r3', 'r3', 'rf26'), comment='a | r')
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
# header emission
# --------------------------------------------------------------------------

HEADER_DOC = """\
 *
 * Every kernel runs on all QPUs at once: lane 0..15 of each QPU covers a
 * 16-pixel group (x = lane .. lane+15), qid = (tidx>>2)&15 selects the
 * row band (rows = h / nq) the QPU processes.  Lanes outside the clip
 * rect store to the scratch sink instead of the surface.  All branches
 * are uniform (ub=0); the driver appends one trailing uniform word
 * (scratch phys) that the kernels never consume.
 *
 * Uniform contract (uint32 words, see bsp_g2d.c):
 *   argb_fill (16): 0=color 1=L-1 2=stride 3=h-1 4=L*64-stride
 *     5=L*rows 6=phys 7,8=x0,x1 9,10=y0,y1 11=full 12=rows
 *     13=rows*stride 14=scratch 15=0
 *   argb_blit / argb_rotate (24): 0-3=pu,qu,pv,qv (>>15) 4,5=cu,cv
 *     6,7=src_w-1,src_h-1 8=src_w 9,10=dst_phys,src_phys 11=L-1
 *     12=dst stride 13=dst_h-1 14=L*64-dst_stride 15=L*rows
 *     16,17=x0,x1 18,19=y0,y1 20=full 21=rows 22=rows_stride 23=scratch
 *   argb_alpha (23): 0=pu 1=cu 2=qv 3=cv 4,5=src_w-1,src_h-1 6=src_w
 *     7,8=dst_phys,src_phys 9=L-1 10=dst_h-1 11=alpha 12=L*dst_h
 *     13-16=x0,x1,y0,y1 17=16 18=L*64-dst_stride 19=255 20=rows
 *     21=rows_stride 22=scratch
 * where L = (w+15)/16 groups per row and rows = h / nq.
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
    lines.append(' * back end: four CSD kernels for the V3D 4.2 (BCM2711) QPU'
                 ' ISA and')
    lines.append(' * four SRQ kernels for the VC4 (V3D 2.1, BCM2837) QPU ISA.')
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
    for build in (kernel_fill,
                  lambda: kernel_affine(False),
                  lambda: kernel_affine(True),
                  kernel_alpha,
                  vc4_fill,
                  lambda: vc4_affine(False),
                  lambda: vc4_affine(True),
                  vc4_alpha):
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
