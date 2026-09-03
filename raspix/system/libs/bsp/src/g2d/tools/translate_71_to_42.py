#!/usr/bin/env python3
# translate_71_to_42.py - translate the hardware-proven raspi5 (V3D 7.1,
# BCM2712) g2d QPU kernel binaries into V3D 4.2 (BCM2711, Pi 4) encodings.
#
#   python3 translate_71_to_42.py <raspi5 g2d_qpu_kernels.h>   report
#   from translate_71_to_42 import translate_all                for gen_kernels
#
# Translation rules (mechanical - no behavior is invented):
#   - ALU op numbers are shared between 7.1 and 4.2 for every op these
#     kernels use (add/sub/shl/shr/asr/ror/and/or/min/max on the add pipe;
#     madd/msub/umul24/smul24 on the mul pipe); only the operand encoding
#     differs (7.1: four raddr slots + smimm_a/b/c/d sigs; 4.2: shared
#     raddr_a/raddr_b + mux fields, small immediates only via raddr_b).
#   - the 7.1 add-pipe MOV (op 249) becomes the 4.2 mul-pipe mov (op 15,
#     mux_b 7); its flags move ac->mc / apf->mpf / auf->muf.  Both pipes
#     push the same A flag, so branch/cond consumers are unaffected
#     (proven by the existing raspix 4.2 kernels, which branch on
#     mul-pipe pushes).
#   - op 187 selectors (nop/tidx/eidx/barrierid/tmuwt) translate from the
#     7.1 raddr_b encoding to the 4.2 mux encoding (K42's ADD0 table).
#   - VPACK (op 247) / V8PACK (op 248) exist only on 7.1 (Mesa qpu_pack.c
#     first_ver=71); argb_alpha's pack trio is rewritten with shifted
#     intermediates (see rewrite_packs below).
#   - a 7.1 word whose two halves no longer fit one 4.2 word (>2 rf
#     reads, small imm + 2 rf, both halves on the mul pipe, unencodable
#     flag combos) is SPLIT into two sequential instructions.  Both
#     halves of a dual-issue word fetch operands in the same cycle, so
#     the split is safe only when neither half reads the other half's
#     destination - the splitter asserts this and orders the halves
#     (cond consumer before flag pusher, tmud before tmua).
#   - branches: raspi5 encodes ub=1/bdu=1 (uniform pointer += 0, a
#     no-op: every kernel drains its whole uniform stream through
#     ldunifrf BEFORE the first branch - asserted below); K42 re-emits
#     the raspix-proven ub=0 form.  Branch delay slots must stay exactly
#     3 instructions:
#       . a slot instruction that would split is hoisted in front of
#         the branch when that is safe (it pushes no flags and no slot
#         is a branch target), and its slot is refilled with a nop;
#       . otherwise (the software-pipeline idiom: the slots double as
#         the loop top, are branch targets and push flags) the branch
#         is redirected to a STUB at the end of the kernel that runs a
#         copy of the (freely split) slot instructions and then jumps
#         to the original target; the branch's own slots become plain
#         nops and the original slot instructions stay in place as
#         ordinary fall-through body instructions.  Both paths execute
#         the same instruction sequence with the same flag state.

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qpuasm import (fld, SMALL_IMM, SIG71, MAGIC42, SIG_WRITES_ADDR,
                    flags_unpack, K42, decode, extract_kernels, AsmError)

COND_N = ['', 'ifa', 'ifb', 'ifna', 'ifnb']
PF_N = ['', 'pushz', 'pushn', 'pushc']
UF_N = ['', 'andz', 'andnz', 'nornz', 'norz', 'andn', 'andnn',
        'norzn', 'norn', 'andc', 'andnc', 'nornc', 'norc']

ADD2_REV = {56: 'add', 60: 'sub', 120: 'min', 121: 'max', 122: 'umin',
            123: 'umax', 124: 'shl', 125: 'shr', 126: 'asr', 127: 'ror',
            181: 'and', 182: 'or', 183: 'xor'}
MUL2_REV = {1: 'madd', 2: 'msub', 3: 'umul24', 9: 'smul24'}
# 7.1 op-187 selector = raddr_b (Mesa qpu_pack.c v71 table)
SEL71 = {0: 'nop', 1: 'tidx', 2: 'eidx', 14: 'barrierid', 15: 'tmuwt',
         16: 'vpmwt'}
BR_COND = {0: 'always', 2: 'a0', 3: 'na0', 4: 'alla', 5: 'anyna',
           6: 'anya', 7: 'allna'}

# argb_alpha keeps 16 in rf17 for its cross-byte shifts (uniform u17 = 16
# per the kernel's contract); the pack rewrite reuses it for the <<16.
PACK_SHIFT16 = {'argb_alpha': 'rf17'}


class XlatError(Exception):
    pass


# --------------------------------------------------------------------------
# 7.1 decode -> IR
# --------------------------------------------------------------------------

def is_branch(w):
    return fld(w, 63, 58) == 0 and (fld(w, 57, 53) & 24) == 16


def dec_branch(w, ip):
    cond = fld(w, 34, 32)
    off = (fld(w, 55, 35) << 3) | (fld(w, 31, 24) << 24)
    if off >= (1 << 31):
        off -= (1 << 32)
    tgt = ip + 4 + off // 8
    if cond not in BR_COND:
        raise XlatError('branch cond %d at %d' % (cond, ip))
    if fld(w, 13, 12) != 1:
        raise XlatError('branch bdi != REL at %d' % ip)
    # ub=1/bdu=1 = "uniform pointer += 0": harmless, dropped (see header)
    return {'kind': 'branch', 'cond': BR_COND[cond], 'tgt': tgt, 'ip': ip}


def waddr_name(w, magic):
    if not magic:
        return 'rf%d' % w
    n = MAGIC42.get(w)
    if n is None:
        raise XlatError('unknown magic waddr %d' % w)
    return n


def dec_alu(w, ip):
    signame = SIG71.get(fld(w, 57, 53))
    if signame is None:
        raise XlatError('sig %d at %d' % (fld(w, 57, 53), ip))
    sigs = [s for s in signame.split('+') if s]
    smA = 'smimm_a' in sigs
    smB = 'smimm_b' in sigs
    smC = 'smimm_c' in sigs
    smD = 'smimm_d' in sigs
    realsigs = tuple(s for s in sigs if not s.startswith('smimm'))

    pc = fld(w, 52, 46)
    sigdst = None
    fl = (0, 0, 0, 0, 0, 0)
    if any(s in SIG_WRITES_ADDR for s in realsigs):
        sigdst = waddr_name(pc & ~64, bool(pc & 64))
    elif pc:
        fl = flags_unpack(pc)
        if fl is None:
            raise XlatError('bad flag pack %d at %d' % (pc, ip))
    ac, mc, apf, mpf, auf, muf = fl

    ra, rb = fld(w, 11, 6), fld(w, 5, 0)
    rc, rd = fld(w, 23, 18), fld(w, 17, 12)

    def src(r, sm):
        return ('#%d' % SMALL_IMM[r]) if sm else 'rf%d' % r

    # ---- add half ----
    opa = fld(w, 31, 24)
    dsta = waddr_name(fld(w, 37, 32), bool(w & (1 << 44)))
    a = None
    if opa == 187:
        if smB:
            raise XlatError('op187 with smimm_b at %d' % ip)
        sel = SEL71.get(rb)
        if sel is None:
            raise XlatError('op187 selector %d at %d' % (rb, ip))
        if sel != 'nop':
            a = {'name': sel, 'dst': None if dsta == 'nop' else dsta,
                 'src': []}
    elif opa == 249:                     # 7.1-only add-pipe MOV
        if rb != 3:                      # 3 = int mov, unpack NONE
            raise XlatError('mov unpack rb=%d at %d' % (rb, ip))
        a = {'name': 'mov', 'dst': dsta, 'src': [src(ra, smA)]}
    elif opa in (247, 248):              # 7.1-only VPACK / V8PACK
        a = {'name': 'vpack' if opa == 247 else 'v8pack', 'dst': dsta,
             'src': [src(ra, smA), src(rb, smB)]}
    elif opa in ADD2_REV:
        a = {'name': ADD2_REV[opa], 'dst': dsta,
             'src': [src(ra, smA), src(rb, smB)]}
    else:
        raise XlatError('7.1 add op %d at %d' % (opa, ip))
    if a:
        a.update(cond=COND_N[ac], pf=PF_N[apf], uf=UF_N[auf])

    # ---- mul half ----
    opm = fld(w, 63, 58)
    dstm = waddr_name(fld(w, 43, 38), bool(w & (1 << 45)))
    m = None
    if opm == 14:                        # 7.1 mov/nop family
        if rd == 63:
            m = None
        elif rd == 3:                    # int mov, unpack NONE
            m = {'name': 'mov', 'dst': dstm, 'src': [src(rc, smC)]}
        else:
            raise XlatError('mul op14 rd=%d at %d' % (rd, ip))
    elif opm in MUL2_REV:
        m = {'name': MUL2_REV[opm], 'dst': dstm,
             'src': [src(rc, smC), src(rd, smD)]}
    else:
        raise XlatError('7.1 mul op %d at %d' % (opm, ip))
    if m:
        m.update(cond=COND_N[mc], pf=PF_N[mpf], uf=UF_N[muf])

    return {'kind': 'alu', 'sig': realsigs, 'sigdst': sigdst,
            'a': a, 'm': m, 'ip': ip}


def decode71_ir(words):
    return [dec_branch(w, i) if is_branch(w) else dec_alu(w, i)
            for i, w in enumerate(words)]


# --------------------------------------------------------------------------
# IR helpers
# --------------------------------------------------------------------------

def h_reads(h):
    return set(s for s in (h.get('src') or ()) if s.startswith('rf'))


def h_wr(h):
    return h.get('dst')


def ir_reads(ins):
    r = set()
    for h in (ins.get('a'), ins.get('m')):
        if h:
            r |= h_reads(h)
    return r


def ir_writes(ins):
    ws = set()
    for h in (ins.get('a'), ins.get('m')):
        if h and h.get('dst'):
            ws.add(h['dst'])
    if ins.get('sigdst'):
        ws.add(ins['sigdst'])
    return ws


def h_pushes(h):
    return bool(h and (h.get('pf') or h.get('uf')))


def dead_before_read(irs, start, reg):
    """True when `reg` is written before any read on EVERY path from
    irs[start] (branch delay slots and both branch outcomes followed;
    thread end = dead).  Used to prove the pack-rewrite temporaries
    are dead."""
    seen = set()
    work = [(start, None)]               # (pos, pending (slots, tgt))
    while work:
        pos, pend = work.pop()
        while True:
            key = (pos, pend)
            if key in seen:
                break
            seen.add(key)
            if pos >= len(irs):
                break                     # fell off the end: dead
            ins = irs[pos]
            if ins['kind'] == 'branch':
                if pend is not None:
                    raise XlatError('branch inside delay slots @%d' % pos)
                pend = (3, ins['tgt'])
                pos += 1
                continue
            if reg in ir_reads(ins) or reg == ins.get('sigdst_read', ''):
                return False
            if reg in ir_writes(ins):
                break                     # overwritten: this path is fine
            if pend is not None:
                slots, tgt = pend
                slots -= 1
                if slots == 0:
                    work.append((tgt, None))       # taken path
                    pend = None                    # fall-through path
                else:
                    pend = (slots, tgt)
            pos += 1
    return True


# --------------------------------------------------------------------------
# VPACK/V8PACK rewrite (argb_alpha only)
# --------------------------------------------------------------------------
# Original trio (all four copies have this exact shape):
#     vpack  a, a, g        # a = blue | green<<16       (all values <=255,
#     ...alpha math (touches g and d, not a/c)...         guaranteed by the
#     vpack  c, c, d        # c = red  | alpha<<16        blend >>8 clamps)
#     v8pack a, a, c        # a = ARGB8888
# Rewrite with the SHIFTED convention (each value <=255, so shl+or is
# exactly the masked pack):
#     site i: shl g, g, #8         ; or a, a, g     -> a = blue | green<<8
#     site j: shl c, c, rf17(=16)  ; ror d, d, #8   -> c = red<<16, d = A<<24
#             or c, c, d                            -> c = red<<16 | A<<24
#     site k: or a, a, c                            -> a = ARGB8888
# g and d are dead after their sites (asserted via dead_before_read).

def rewrite_packs(name, irs):
    over = {}
    sh16 = PACK_SHIFT16.get(name)
    packs = [(i, ins) for i, ins in enumerate(irs)
             if ins['kind'] == 'alu' and ins.get('a') and
             ins['a']['name'] in ('vpack', 'v8pack')]
    if not packs:
        return over
    if sh16 is None:
        raise XlatError('%s: vpack present but no shift-16 register' % name)
    used = set()
    for k, ins in packs:
        A = ins['a']
        if A['name'] != 'v8pack':
            continue
        a, c = A['src'][0], A['src'][1]
        if A['dst'] != a or A['cond'] or h_pushes(A) or ins['m'] or \
                ins['sig']:
            raise XlatError('v8pack shape at %d' % k)
        # locate the two feeding vpacks
        j = i = None
        for p, pins in reversed([pp for pp in packs if pp[0] < k]):
            nm, dst = pins['a']['name'], pins['a']['dst']
            if nm == 'vpack' and dst == c and j is None:
                j = p
            elif nm == 'vpack' and dst == a and i is None and \
                    j is not None:
                i = p
        if i is None or j is None:
            raise XlatError('unpaired v8pack at %d' % k)
        for p in (i, j):
            pi = irs[p]['a']
            if pi['dst'] != pi['src'][0] or pi['cond'] or h_pushes(pi) \
                    or irs[p]['m'] or irs[p]['sig']:
                raise XlatError('vpack shape at %d' % p)
        g, d = irs[i]['a']['src'][1], irs[j]['a']['src'][1]
        # a untouched between i..k, c untouched between j..k
        for p in range(i + 1, k):
            if p in (j,):
                continue
            if a in ir_reads(irs[p]) or a in ir_writes(irs[p]):
                raise XlatError('%s live across pack trio @%d' % (a, p))
        for p in range(j + 1, k):
            if c in ir_reads(irs[p]) or c in ir_writes(irs[p]):
                raise XlatError('%s live across pack pair @%d' % (c, p))
        # g dead after i's pack, d dead after j's pack
        if not dead_before_read(irs, i + 1, g):
            raise XlatError('%s not dead after vpack @%d' % (g, i))
        if not dead_before_read(irs, k + 1, d):
            raise XlatError('%s not dead after v8pack @%d' % (d, k))
        if i in used or j in used or k in used:
            raise XlatError('pack site reused')
        used.update((i, j, k))
        over[i] = [dict(a=('shl', g, g, '#8'), comment='pack: g <<= 8'),
                   dict(a=('or', a, a, g), comment='pack: a = b|g<<8')]
        over[j] = [dict(a=('shl', c, c, sh16), comment='pack: r <<= 16'),
                   dict(a=('ror', d, d, '#8'), comment='pack: A <<= 24'),
                   dict(a=('or', c, c, d), comment='pack: c = r|A<<24')]
        over[k] = [dict(a=('or', a, a, c), comment='pack: ARGB8888')]
    for p, ins in packs:
        if p not in used:
            raise XlatError('vpack without v8pack at %d' % p)
    return over


# --------------------------------------------------------------------------
# half -> K42 emission
# --------------------------------------------------------------------------

def half_pipe(h):
    """the 4.2 pipe this half must land on"""
    if h['name'] in ('mov', 'madd', 'msub', 'umul24', 'smul24'):
        return 'm'
    return 'a'


def half_op(h):
    if h['src']:
        return tuple([h['name'], h['dst']] + h['src'])
    return (h['name'], h['dst'])


def half_flags(h, pipe):
    f = {}
    if h.get('cond'):
        f['mc' if pipe == 'm' else 'ac'] = h['cond']
    if h.get('pf'):
        f['mpf' if pipe == 'm' else 'apf'] = h['pf']
    if h.get('uf'):
        f['muf' if pipe == 'm' else 'auf'] = h['uf']
    return f


def emit_single(h, sig=(), sigdst=None, comment=''):
    pipe = half_pipe(h)
    e = dict(sig=sig, sigdst=sigdst, flags=half_flags(h, pipe),
             comment=comment)
    e['m' if pipe == 'm' else 'a'] = half_op(h)
    return e


def can_fuse(A, M):
    """try both halves in one 4.2 word (mutually exclusive pipes,
    <=2 rf, <=1 small imm, encodable flag combo)"""
    pa, pm = half_pipe(A), half_pipe(M)
    if pa == pm:
        return None
    a, m = (A, M) if pa == 'a' else (M, A)
    e = dict(a=half_op(a), m=half_op(m), sig=(), sigdst=None,
             flags=dict(half_flags(a, 'a'), **half_flags(m, 'm')))
    try:
        t = K42('probe')
        t.alu(a=e['a'], m=e['m'], flags=e['flags'])
    except AsmError:
        return None
    return e


def order_halves(A, M, ip):
    """sequential order for a split dual-issue word.  Original
    semantics: both halves fetch operands (and sample cond flags) in
    the same cycle."""
    first, second = A, M                    # textual order by default
    reasons = []
    wa, wm = h_wr(A), h_wr(M)
    if wm and wm in h_reads(A):
        reasons.append(('A', 'A reads old %s' % wm))
    if wa and wa in h_reads(M):
        reasons.append(('M', 'M reads old %s' % wa))
    if h_pushes(A) and (M.get('cond')):
        reasons.append(('M', 'M cond samples pre-push flags'))
    if h_pushes(M) and (A.get('cond')):
        reasons.append(('A', 'A cond samples pre-push flags'))
    if wa == 'tmud' or wm == 'tmua':
        reasons.append(('A', 'tmud stages before tmua fires'))
    if wm == 'tmud' or wa == 'tmua':
        reasons.append(('M', 'tmud stages before tmua fires'))
    firsts = set(r[0] for r in reasons)
    if len(firsts) > 1:
        raise XlatError('split order conflict at %d: %s' % (ip, reasons))
    if firsts == {'M'}:
        first, second = M, A
    if h_pushes(A) and h_pushes(M):
        raise XlatError('both halves push flags at %d' % ip)
    return first, second


def xlat_alu(ins):
    """one 7.1 ALU IR instruction -> list of 4.2 emissions"""
    A, M, sig, sigdst = ins['a'], ins['m'], ins['sig'], ins['sigdst']
    cmt = 'ip%d' % ins['ip']
    if A is None and M is None:
        return [dict(sig=sig, sigdst=sigdst, comment=cmt or 'nop')]
    if A is None or M is None:
        return [emit_single(A or M, sig=sig, sigdst=sigdst, comment=cmt)]
    fused = can_fuse(A, M)
    if fused is not None:
        if sig or sigdst:
            fused.update(sig=sig, sigdst=sigdst)
        fused['comment'] = cmt
        return [fused]
    if sig or sigdst:
        raise XlatError('split with sig %s at %d' % (sig, ins['ip']))
    first, second = order_halves(A, M, ins['ip'])
    return [emit_single(first, comment=cmt + ' (split 1/2)'),
            emit_single(second, comment=cmt + ' (split 2/2)')]


# --------------------------------------------------------------------------
# whole-kernel translation
# --------------------------------------------------------------------------

def translate_kernel(name, words):
    irs = decode71_ir(words)
    over = rewrite_packs(name, irs)

    targets = set(i['tgt'] for i in irs if i['kind'] == 'branch')
    for t in targets:
        if not 0 <= t < len(irs):
            raise XlatError('branch target %d out of range' % t)

    # every ldunifrf must precede the first branch (justifies ub=0)
    first_br = next((i for i, x in enumerate(irs)
                     if x['kind'] == 'branch'), len(irs))
    for i, x in enumerate(irs):
        if x['kind'] == 'alu' and 'ldunifrf' in x['sig'] and i > first_br:
            raise XlatError('%s: ldunifrf after first branch @%d'
                            % (name, i))

    # expansions per original ip
    exps = {}
    for i, x in enumerate(irs):
        if x['kind'] == 'branch':
            exps[i] = [dict(branch=(x['cond'], x['tgt']),
                            comment='ip%d' % i)]
        elif i in over:
            exps[i] = over[i]
        else:
            exps[i] = xlat_alu(x)

    # branch delay slots: keep exactly 3 output instructions.
    # Plan A (hoist): move grown slot expansions in front of the branch.
    # Plan B (stub): for the software-pipeline idiom (slots are branch
    # targets / push flags), redirect the branch to an end-of-kernel
    # stub holding a copy of the slot expansions + `balways` to the
    # original target; the branch gets 3 fresh nop slots and the
    # original slots stay in place as fall-through body instructions.
    hoists = {}
    stubs = []                 # (label, [slot expansions], orig tgt)
    stubbed = set()
    for i, x in enumerate(irs):
        if x['kind'] != 'branch':
            continue
        slots = list(range(i + 1, i + 4))
        if slots[-1] >= len(irs):
            raise XlatError('branch @%d lacks delay slots' % i)
        grow = [s for s in slots if len(exps[s]) != 1]
        if any(irs[s]['kind'] == 'branch' for s in slots):
            # proven back-to-back conditional branch idiom (argb_fill
            # ip53 anya / ip56 anyna are mutually exclusive); keep it
            # verbatim -- legal only while every slot stays 1:1 so the
            # relative layout is untouched
            if grow:
                raise XlatError('branch @%d: slot branch + expansion'
                                % i)
            continue
        if not grow:
            continue
        upto = max(grow)
        hoistable = i not in targets and all(
            s not in targets and
            not h_pushes(irs[s].get('a')) and
            not h_pushes(irs[s].get('m'))
            for s in slots if s <= upto)
        if hoistable:
            moved = []
            for s in slots:
                if s > upto:
                    break
                moved.extend(exps[s])
                exps[s] = [dict(comment='hoisted delay slot ip%d' % s)]
            hoists[i] = moved
        else:
            lbl = 'S%d' % i
            stubs.append((lbl, [e for s in slots for e in exps[s]],
                          x['tgt']))
            stubbed.add(i)
            exps[i] = ([dict(branch=(x['cond'], lbl),
                             comment='ip%d (via slot stub)' % i)] +
                       [dict(comment='stub branch slot')
                        for _ in range(3)])

    # assemble
    k = K42(name)
    n_out = {}
    for i, x in enumerate(irs):
        if i in targets:
            k.label('L%d' % i)
        start = len(k.rows)
        for e in hoists.get(i, ()):
            emit(k, e)
        for e in exps[i]:
            emit(k, e)
        n_out[i] = len(k.rows) - start
    for lbl, ses, tgt in stubs:
        k.label(lbl)
        for e in ses:
            emit(k, dict(e, comment=e.get('comment', '') + ' (stub)'))
        k.branch('L%d' % tgt, 'always', 'stub -> original target')
        k.nop(3, 'stub branch delay slots')
    rows = k.finish()
    words42 = [w for w, c in rows]
    verify(name, irs, words42, n_out, stubbed, len(stubs))
    return words42, rows


def emit(k, e):
    if 'branch' in e:
        cond, tgt = e['branch']
        lbl = tgt if isinstance(tgt, str) else 'L%d' % tgt
        k.branch(lbl, cond, e.get('comment', ''))
        return
    k.alu(a=e.get('a'), m=e.get('m'), sig=e.get('sig', ()),
          sigdst=e.get('sigdst'), flags=e.get('flags'),
          comment=e.get('comment', ''))


# --------------------------------------------------------------------------
# structural verification
# --------------------------------------------------------------------------

def verify(name, irs, words42, n_out, stubbed, n_stubs):
    # 1. every output word decodes cleanly as 4.2
    for i, w in enumerate(words42):
        txt = decode(w, i, 42)
        if '?' in txt:
            raise XlatError('%s: undecodable 4.2 word @%d: %s'
                            % (name, i, txt))
    # 2. every branch keeps 3 delay slots; a branch inside a slot is
    # allowed only where the proven 7.1 source had one (nested count
    # must match exactly)
    def nested(flags):
        return sum(1 for i, b in enumerate(flags) if b
                   for s in range(i + 1, min(i + 4, len(flags)))
                   if flags[s])
    br42 = [is_branch(w) for w in words42]
    for i, b in enumerate(br42):
        if b and i + 3 >= len(words42):
            raise XlatError('%s: branch @%d lacks slots' % (name, i))
    br71 = [x['kind'] == 'branch' for x in irs]
    if nested(br42) != nested(br71):
        raise XlatError('%s: nested branch count %d -> %d'
                        % (name, nested(br71), nested(br42)))
    # 3. branch count preserved (+1 balways per stub)
    nb71 = sum(1 for x in irs if x['kind'] == 'branch')
    nb42 = sum(1 for w in words42 if is_branch(w))
    if nb42 != nb71 + n_stubs:
        raise XlatError('%s: branch count %d -> %d (%d stubs)'
                        % (name, nb71, nb42, n_stubs))
    # 4. delay slots of non-stubbed branches stayed 1:1
    for i, x in enumerate(irs):
        if x['kind'] == 'branch' and i not in stubbed:
            for s in range(i + 1, i + 4):
                if n_out.get(s) != 1:
                    raise XlatError('%s: delay slot ip%d emitted %s'
                                    % (name, s, n_out.get(s)))


# --------------------------------------------------------------------------
# API + report
# --------------------------------------------------------------------------

KERNELS = ('argb_fill4', 'argb_copy', 'argb_scale_pow2', 'argb_alpha',
           'argb_rot90', 'argb_rotate', 'argb_blit', 'argb_fill')


def translate_all(header_path):
    """-> list of (name, [4.2 words]) in the raspi5 header order"""
    kerns = extract_kernels(header_path)
    out = []
    for name in KERNELS:
        if name not in kerns:
            raise XlatError('kernel %s missing from %s'
                            % (name, header_path))
        words42, _ = translate_kernel(name, kerns[name])
        out.append((name, words42))
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__ or 'usage: translate_71_to_42.py <kernels.h>')
        return
    kerns = extract_kernels(sys.argv[1])
    show = '-v' in sys.argv
    for name in KERNELS:
        words71 = kerns[name]
        words42, rows = translate_kernel(name, words71)
        print('=== %s: %d -> %d instructions ===' %
              (name, len(words71), len(words42)))
        if show:
            for i, (w, c) in enumerate(rows):
                print('%4d: %016x  %-46s | %s'
                      % (i, w, decode(w, i, 42), c))
            print('')


if __name__ == '__main__':
    main()
