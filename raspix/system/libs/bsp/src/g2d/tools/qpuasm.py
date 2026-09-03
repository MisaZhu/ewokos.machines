#!/usr/bin/env python3
# qpuasm.py - QPU (dis)assembler for the ewokos g2d back end.
#
#   decode71 : decodes V3D 7.1 (BCM2712, raspi5) kernel binaries into text
#   decode42 : decodes V3D 4.2 (BCM2711; V3D 3.3-7.0 family) binaries
#   asm42    : assembles V3D 4.2 kernels from the python DSL below
#   decode21 : decodes VC4 / V3D 2.1 (BCM2837, Pi3) binaries
#   K21      : assembles VC4 kernels (SRQ dispatch flavor)
#
# The encoding rules are transcribed 1:1 from Mesa
# src/broadcom/qpu/qpu_pack.c (v3d_qpu_instr_pack / _unpack); constraint
# notes from src/broadcom/compiler/qpu_validate.c:
#   - 4.2 raddr_a/raddr_b belong to the instruction that uses mux A/B
#     (no one-instruction delay); one instruction may reference at most
#     2 distinct rf addresses; small immediates only in the mux B slot
#     (sig.small_imm_b, value index in that instruction's raddr_b).
#   - a branch occupies 3 delay slots (no branch/thrsw inside them).
#   - TMU loads: sig ldtmu -> r4; TMU stores: ALU write of r5 with sig
#     wrtmuc; ldunif writes r5 one instruction later.
#
# Usage:
#   python3 qpuasm.py decode71 <header.h>        print disassembly
#   python3 qpuasm.py check42                    self-test the encoder

import re
import sys

MASK64 = (1 << 64) - 1


def fld(word, hi, lo):
    return (word >> lo) & ((1 << (hi - lo + 1)) - 1)


def fld_set(word, val, hi, lo):
    mask = (1 << (hi - lo + 1)) - 1
    return (word & ~(mask << lo)) | ((val & mask) << lo)


SMALL_IMM = ([i for i in range(16)] +
             [i - 32 for i in range(16, 32)] +
             [0x3b800000, 0x3c000000, 0x3c800000, 0x3d000000,
              0x3d800000, 0x3e000000, 0x3e800000, 0x3f000000,
              0x3f800000, 0x40000000, 0x40800000, 0x41000000,
              0x41800000, 0x42000000, 0x42800000, 0x43000000])

MAGIC42 = {0: 'r0', 1: 'r1', 2: 'r2', 3: 'r3', 4: 'r4', 5: 'r5',
           6: 'nop', 7: 'tlb', 8: 'tlbu', 9: 'unifa', 10: 'tmul',
           11: 'tmud', 12: 'tmua', 13: 'tmuau', 14: 'vpm', 15: 'vpmu',
           16: 'sync', 17: 'syncu', 18: 'syncb', 19: 'recip',
           20: 'rsqrt', 21: 'exp', 22: 'log', 23: 'sin', 24: 'rsqrt2',
           32: 'tmuc', 33: 'tmus', 34: 'tmut', 35: 'tmur', 36: 'tmui',
           37: 'tmub', 38: 'tmudref', 39: 'tmuoff', 40: 'tmuscm',
           41: 'tmusf', 42: 'tmuslod', 43: 'tmuhs', 44: 'tmuhscm',
           45: 'tmuhsf', 46: 'tmuhsf', 55: 'r5rep'}
MAGIC42_INV = {v: k for k, v in MAGIC42.items()}

SIG42 = {0: '', 1: 'thrsw', 2: 'ldunif', 3: 'thrsw+ldunif', 4: 'ldtmu',
         5: 'thrsw+ldtmu', 6: 'ldtmu+ldunif', 7: 'thrsw+ldtmu+ldunif',
         8: 'ldvary', 9: 'thrsw+ldvary', 10: 'ldvary+ldunif',
         11: 'thrsw+ldvary+ldunif', 12: 'ldunifrf', 13: 'thrsw+ldunifrf',
         14: 'smimm_b+ldvary', 15: 'smimm_b', 16: 'ldtlb', 17: 'ldtlbu',
         18: 'wrtmuc', 19: 'thrsw+wrtmuc', 20: 'ldvary+wrtmuc',
         21: 'thrsw+ldvary+wrtmuc', 22: 'ucb', 23: 'rot', 24: 'ldunifa',
         25: 'ldunifarf', 31: 'smimm_b+ldtmu'}
SIG42_INV = {v: k for k, v in SIG42.items()}

SIG71 = dict(SIG42)
SIG71.update({14: 'smimm_a', 15: 'smimm_b', 23: '', 26: 'ldtmu+wrtmuc',
              27: 'thrsw+ldtmu+wrtmuc', 30: 'smimm_c', 31: 'smimm_d',
              22: 'ucb'})

SIG_WRITES_ADDR = ('ldunifrf', 'ldunifarf', 'ldvary', 'ldtmu', 'ldtlb',
                   'ldtlbu')

COND_NAMES = ['', 'ifa', 'ifb', 'ifna', 'ifnb']
PF_NAMES = ['', '.pushz', '.pushn', '.pushc']
UF_NAMES = ['', '.andz', '.andnz', '.nornz', '.norz', '.andn', '.andnn',
            '.norzn', '.norn', '.andc', '.andnc', '.nornc', '.norc']
BRANCH_COND = ['always', 'a0', 'na0', 'alla', 'anyna', 'anya', 'allna']
F32U = ['.abs', '', '.l', '.h', '.sat', '.nsat', '.max0']
I32U = ['.abs', '', '.ul', '.uh', '.il', '.ih']


def flags_unpack(pc):
    ac = mc = apf = mpf = auf = muf = 0
    if pc == 0:
        pass
    elif pc >> 2 == 0:
        apf = pc & 3
    elif pc >> 4 == 0:
        auf = (pc & 15) - 4 + 1
    elif pc == 0x10:
        return None
    elif pc >> 2 == 0x4:
        mpf = pc & 3
    elif pc >> 4 == 0x1:
        muf = (pc & 15) - 4 + 1
    elif pc >> 4 == 0x2:
        ac = ((pc >> 2) & 3) + 1
        mpf = pc & 3
    elif pc >> 4 == 0x3:
        mc = ((pc >> 2) & 3) + 1
        apf = pc & 3
    elif pc >> 6:
        cm = {0: 1, 1: 2, 2: 3, 3: 4}
        mc = cm[(pc >> 4) & 3]
        if ((pc >> 2) & 3) == 0:
            ac = cm[pc & 3]
        else:
            auf = (pc & 15) - 4 + 1
    return ac, mc, apf, mpf, auf, muf


def flags_text(fl, which):
    ac, mc, apf, mpf, auf, muf = fl
    if which == 'a':
        return COND_NAMES[ac] + PF_NAMES[apf] + UF_NAMES[auf]
    return COND_NAMES[mc] + PF_NAMES[mpf] + UF_NAMES[muf]


def smimm_txt(idx):
    v = SMALL_IMM[idx]
    if -16 <= (v if isinstance(v, int) else 0) <= 15:
        return str(v)
    return '0x%08x' % v


def waddr_txt(w, magic):
    if not magic:
        return 'rf%d' % w
    return MAGIC42.get(w, 'wunk%d' % w)


def decode_branch(word, ip):
    cond = fld(word, 34, 32)
    cname = BRANCH_COND[0] if cond == 0 else BRANCH_COND[cond - 1]
    off = (fld(word, 55, 35) << 3) | (fld(word, 31, 24) << 24)
    if off >= (1 << 31):
        off -= (1 << 32)
    ub = (word >> 14) & 1
    # Mesa convention: target_ip = branch_ip + 4 + offset/8
    tgt = ip + 4 + off // 8
    extra = (' ubdu=%d' % fld(word, 17, 15)) if ub else ''
    return 'b%s %+d   (-> %d)%s' % (cname, off, tgt, extra)


def decode(word, ip=0, ver=71):
    """decode one instruction word -> text"""
    if fld(word, 63, 58) == 0 and (fld(word, 57, 53) & 24) == 16:
        return decode_branch(word, ip)

    sig = fld(word, 57, 53)
    sigmap = SIG71 if ver >= 71 else SIG42
    sigtxt = sigmap.get(sig, 'sig?%d' % sig)
    pc = fld(word, 52, 46)
    writes_addr = any(s in sigtxt for s in SIG_WRITES_ADDR)
    if writes_addr:
        fl = (0, 0, 0, 0, 0, 0)
        addr = pc & ~64
        mag = bool(pc & 64)
        sigaddr = ('.' + MAGIC42.get(addr, 'unk%d' % addr)) if mag \
            else '.rf%d' % addr
    else:
        fl = flags_unpack(pc) or (0, 0, 0, 0, 0, 0)
        sigaddr = ''
    smimm_b = 'smimm_b' in sigtxt

    ra = fld(word, 11, 6)
    rb = fld(word, 5, 0)

    # ---- add side
    op_add = fld(word, 31, 24)
    wa = fld(word, 37, 32)
    ma = bool(word & (1 << 44))
    atxt = dis_add(word, op_add, wa, ma, ra, rb, smimm_b, fl, writes_addr,
                   ver)

    # ---- mul side
    op_mul = fld(word, 63, 58)
    wm = fld(word, 43, 38)
    mm = bool(word & (1 << 45))
    mtxt = dis_mul(word, op_mul, wm, mm, ra, rb, smimm_b, fl, writes_addr,
                   ver)

    out = '%-44s ; %s' % (atxt, mtxt)
    if sigtxt:
        out += '  ; ' + sigtxt.replace('+', ' ') + sigaddr
    return out


def _src42(mux, ra, rb, smimm_b):
    if mux <= 5:
        return 'r%d' % mux
    if mux == 6:
        return 'rf%d' % ra
    return smimm_txt(rb) if smimm_b else 'rf%d' % rb


def _src71(r, smimm):
    return smimm_txt(r) if smimm else 'rf%d' % r


def dis_add(word, op, wa, ma, ra, rb, smimm_b, fl, wa_mode, ver):
    dst = waddr_txt(wa, ma)
    if ver >= 71:
        smimm_a = 'smimm_a' in SIG71.get(fld(word, 57, 53), '')
        A = _src71(ra, smimm_a)
        B = _src71(rb, smimm_b)
    else:
        ma_, mb_ = fld(word, 14, 12), fld(word, 17, 15)
        A = _src42(ma_, ra, rb, smimm_b)
        B = _src42(mb_, ra, rb, smimm_b)
    flt = '' if wa_mode else flags_text(fl, 'a')

    def two(name):
        return '%s%s %s, %s, %s' % (name, flt, dst, A, B)

    def one(name):
        return '%s%s %s, %s' % (name, flt, dst, A)

    def zero(name):
        return '%s%s %s' % (name, flt, dst)

    if op <= 47:
        name = 'fadd' if (((op >> 2) & 3) * 8 +
                          (fld(word, 14, 12) if ver < 71 else ra)) <= \
            ((op & 3) * 8 + (fld(word, 17, 15) if ver < 71 else rb)) \
            else 'faddnf'
        return two(name)
    if op == 56:
        return two('add')
    if op == 60:
        return two('sub')
    if 64 <= op <= 111:
        return two('fsub')
    simple2 = {120: 'min', 121: 'max', 122: 'umin', 123: 'umax',
               124: 'shl', 125: 'shr', 126: 'asr', 127: 'ror',
               181: 'and', 182: 'or', 183: 'xor', 184: 'vadd',
               185: 'vsub'}
    if op in simple2:
        return two(simple2[op])
    if 128 <= op <= 175:
        name = 'fmin' if (((op >> 2) & 3) * 8 +
                          (fld(word, 14, 12) if ver < 71 else ra)) <= \
            ((op & 3) * 8 + (fld(word, 17, 15) if ver < 71 else rb)) \
            else 'fmax'
        return two(name)
    if op == 186:
        v = rb if ver >= 71 else fld(word, 17, 15)
        name = {0: 'not', 1: 'neg', 2: 'flapush', 3: 'flbpush',
                4: 'flpop', 6: 'setmsf', 7: 'setrevf'}.get(
                    v & 7, 'recip' if v & 7 == 5 else '?186')
        if ver < 71 and v & 7 == 5:
            name = 'recip'
        return one(name)
    if op == 187:
        v = rb if ver >= 71 else fld(word, 17, 15)
        va = ra if ver >= 71 else fld(word, 14, 12)
        if ver >= 71:
            name = {0: 'nop', 1: 'tidx', 2: 'eidx', 3: 'lr', 4: 'vfla',
                    5: 'vflna', 6: 'vflb', 7: 'vflnb', 8: 'xcd', 9: 'ycd',
                    10: 'msf', 11: 'revf', 12: 'iid', 13: 'sampid',
                    14: 'barrierid', 15: 'tmuwt', 16: 'vpmwt',
                    17: 'flafirst', 18: 'flnafirst'}.get(v, '?187')
        else:
            if v == 0:
                name = {0: 'nop', 1: 'tidx', 2: 'eidx', 3: 'lr',
                        4: 'vfla', 5: 'vflna', 6: 'vflb',
                        7: 'vflnb'}.get(va, '?187')
            elif v == 1:
                name = 'fxcd' if va <= 2 else 'xcd' if va == 3 else \
                    'fycd' if va <= 6 else 'ycd'
            elif v == 2:
                name = {0: 'msf', 1: 'revf', 2: 'iid', 3: 'sampid',
                        4: 'barrierid', 5: 'tmuwt', 6: 'vpmwt'}.get(
                            va, '?187')
            elif v == 3 and va == 0:
                name = 'flnafirst'
            else:
                name = '?187'
        return zero(name)
    if 192 <= op <= 239:
        return two('fcmp')
    if op in (245, 246, 247):
        v = rb if ver >= 71 else fld(word, 17, 15)
        if op == 245:
            name = 'ftoin' if v & 3 == 3 else 'fround' if v & 3 <= 2 \
                else 'ftrunc'
        elif op == 246:
            name = 'ftouz' if v & 3 == 3 else 'ffloor' if v & 3 <= 2 \
                else 'fceil'
        else:
            name = 'fdx' if (v & 7) < 3 else 'fdy'
        return one(name)
    if op == 252:
        v = rb if ver >= 71 else fld(word, 17, 15)
        name = 'clz' if v & 7 == 3 else 'itof' if v & 7 < 3 else 'utof'
        return one(name)
    if op == 249 and ver >= 71:
        u = I32U[(rb >> 2) & 7] if rb % 4 == 3 else F32U[(rb >> 2) & 3]
        name = 'mov' if rb % 4 == 3 else 'fmov'
        return '%s%s %s, %s%s' % (name, flt, dst, A, u)
    return 'add?%d%s %s, %s, %s' % (op, flt, dst, A, B)


def dis_mul(word, op, wm, mm, ra, rb, smimm_b, fl, wa_mode, ver):
    dst = waddr_txt(wm, mm)
    if ver >= 71:
        rc = fld(word, 23, 18)
        rd = fld(word, 17, 12)
        A, B = _src71(rc, False), _src71(rd, 'smimm_d' in
                                         SIG71.get(fld(word, 57, 53), ''))
    else:
        ma_, mb_ = fld(word, 20, 18), fld(word, 23, 21)
        A = _src42(ma_, ra, rb, smimm_b)
        B = _src42(mb_, ra, rb, smimm_b)
    flt = '' if wa_mode else flags_text(fl, 'm')

    if op == 0:
        return 'nop?(mul0)'
    if op in (1, 2, 3, 9, 10):
        name = {1: 'madd', 2: 'msub', 3: 'umul24', 9: 'smul24',
                10: 'multop'}[op]
        return '%s%s %s, %s, %s' % (name, flt, dst, A, B)
    if 16 <= op <= 63:
        return 'fmul%s %s, %s, %s' % (flt, dst, A, B)
    if ver >= 71:
        if op == 14:
            rd = fld(word, 17, 12)
            if rd == 63:
                return 'nop'
            if rd in (3, 7, 11, 15, 19):
                return 'mov%s %s, %s%s' % (flt, dst, A,
                                           I32U[(rd >> 2) & 7])
            return 'fmov%s %s, %s%s' % (flt, dst, A, F32U[(rd >> 2) & 3])
        if 4 <= op <= 8:
            return 'vfmul%s %s, %s, %s' % (flt, dst, A, B)
        return 'mul?%d' % op
    # 4.2
    ma_, mb_ = fld(word, 20, 18), fld(word, 23, 21)
    if op == 15:
        if mb_ == 4 and ma_ == 0:
            return 'nop'
        if mb_ == 7:
            return 'mov%s %s, %s' % (flt, dst, A)
        return 'fmov%s %s, %s' % (flt, dst, A)
    if op == 14:
        return 'fmov%s %s, %s' % (flt, dst, A)
    if 4 <= op <= 8:
        return 'vfmul%s %s, %s, %s' % (flt, dst, A, B)
    return 'mul?%d' % op


# --------------------------------------------------------------------------
# V3D 4.2 assembler
# --------------------------------------------------------------------------

class AsmError(Exception):
    pass


ADD2 = {'fadd': 0, 'fsub': 64, 'fmin': 128, 'add': 56, 'sub': 60,
        'min': 120, 'max': 121, 'umin': 122, 'umax': 123, 'shl': 124,
        'shr': 125, 'asr': 126, 'ror': 127, 'and': 181, 'or': 182,
        'xor': 183, 'vadd': 184, 'vsub': 185, 'fcmp': 192}
ADD1 = {'not': (186, 0), 'neg': (186, 1), 'flapush': (186, 2),
        'flbpush': (186, 3), 'flpop': (186, 4), 'recip': (186, 5),
        'setmsf': (186, 6), 'setrevf': (186, 7),
        'fround': (245, 0), 'ftoin': (245, 3), 'ftrunc': (245, 4),
        'ftoiz': (245, 7), 'ffloor': (246, 0), 'ftouz': (246, 3),
        'fceil': (246, 4), 'ftoc': (246, 7), 'fdx': (247, 0),
        'fdy': (247, 4), 'itof': (252, 0), 'clz': (252, 3),
        'utof': (252, 4)}
ADD0 = {'nop': (187, 0, 0), 'tidx': (187, 0, 1), 'eidx': (187, 0, 2),
        'lr': (187, 0, 3), 'vfla': (187, 0, 4), 'vflna': (187, 0, 5),
        'vflb': (187, 0, 6), 'vflnb': (187, 0, 7),
        'fxcd': (187, 1, 0), 'xcd': (187, 1, 3), 'fycd': (187, 1, 4),
        'ycd': (187, 1, 7), 'msf': (187, 2, 0), 'revf': (187, 2, 1),
        'iid': (187, 2, 2), 'sampid': (187, 2, 3),
        'barrierid': (187, 2, 4), 'tmuwt': (187, 2, 5),
        'vpmwt': (187, 2, 6), 'flnafirst': (187, 3, 0)}
MUL2 = {'madd': 1, 'msub': 2, 'umul24': 3, 'smul24': 9, 'multop': 10,
        'fmul': 16}


class K42:
    """V3D 4.2 kernel assembler."""

    def __init__(self, name='kernel'):
        self.name = name
        self.rows = []          # (word, comment)
        self.labels = {}

    # ---- low level -------------------------------------------------------
    def label(self, name):
        if name in self.labels:
            raise AsmError('duplicate label %s' % name)
        self.labels[name] = len(self.rows)

    def raw(self, word, comment=''):
        self.rows.append((word, comment))

    # ---- operand parsing ---------------------------------------------------
    @staticmethod
    def src(s):
        """-> dict: {kind: 'mux'|'rf'|'smimm', mux|rf|val}"""
        if s in ('r0', 'r1', 'r2', 'r3', 'r4', 'r5'):
            return {'kind': 'mux', 'mux': int(s[1])}
        if s == 'a' or s == 'b':
            return {'kind': 'mux', 'mux': 6 if s == 'a' else 7}
        if s.startswith('rf'):
            return {'kind': 'rf', 'rf': int(s[2:])}
        if s.startswith('#'):
            v = int(s[1:], 0)
            if v not in SMALL_IMM:
                raise AsmError('no small immediate encoding for %d' % v)
            return {'kind': 'smimm', 'val': v,
                    'idx': SMALL_IMM.index(v)}
        raise AsmError('bad operand %r' % s)

    @staticmethod
    def dst(s):
        """-> (waddr, magic)"""
        if s.startswith('rf'):
            return int(s[2:]), False
        if s in MAGIC42_INV:
            return MAGIC42_INV[s], True
        raise AsmError('bad dst %r' % s)

    # ---- flag packing -------------------------------------------------------
    @staticmethod
    def flags_pack(ac=0, mc=0, apf=0, mpf=0, auf=0, muf=0):
        names = {'none': 0, 'ifa': 1, 'ifb': 2, 'ifna': 3, 'ifnb': 4}
        ac, mc = names[ac] if isinstance(ac, str) else ac, \
            names[mc] if isinstance(mc, str) else mc
        pfs = {'': 0, 'pushz': 1, 'pushn': 2, 'pushc': 3}
        ufs = {'': 0, 'andz': 1, 'andnz': 2, 'nornz': 3, 'norz': 4,
               'andn': 5, 'andnn': 6, 'norzn': 7, 'norn': 8, 'andc': 9,
               'andnc': 10, 'nornc': 11, 'norc': 12}
        apf = pfs[apf] if isinstance(apf, str) else apf
        mpf = pfs[mpf] if isinstance(mpf, str) else mpf
        auf = ufs[auf] if isinstance(auf, str) else auf
        muf = ufs[muf] if isinstance(muf, str) else muf

        AC, MC, APF, MPF, AUF, MUF = 1, 2, 4, 8, 16, 32
        present = ((AC if ac else 0) | (MC if mc else 0) |
                   (APF if apf else 0) | (MPF if mpf else 0) |
                   (AUF if auf else 0) | (MUF if muf else 0))
        table = {0: 0, APF: 0, AUF: 0, MPF: 1 << 4, MUF: 1 << 4,
                 AC: 1 << 5, AC | MPF: 1 << 5, MC: (1 << 5) | (1 << 4),
                 MC | APF: (1 << 5) | (1 << 4), MC | AC: 1 << 6,
                 MC | AUF: 1 << 6}
        if present not in table:
            raise AsmError('bad flag combination')
        pc = table[present] | apf | mpf
        if present & AUF:
            pc |= auf - 1 + 4
        if present & MUF:
            pc |= muf - 1 + 4
        if present & AC:
            pc |= (ac - 1) if pc & (1 << 6) else (ac - 1) << 2
        if present & MC:
            pc |= ((mc - 1) << 4) if pc & (1 << 6) else (mc - 1) << 2
        return pc

    # ---- the general ALU emit ---------------------------------------------
    def alu(self, a=None, m=None, sig=(), sigdst=None, flags=None,
            comment=''):
        """a/m: ('op', dst, srcA[, srcB[, kw...]]) or dict; sig: tuple of
        signal names; sigdst: destination for sig-writes-address signals;
        flags: dict for flags_pack."""
        a = self._norm_op(a)
        m = self._norm_op(m)
        flags = flags or {}

        # collect rf operands
        raddrs = []   # in encounter order
        smimm = None
        operands = []  # (slot, parsed) with slot = ('a','a') etc.
        for side, d in (('a', a), ('m', m)):
            if not d or d['nsrc'] == 0:
                continue
            for i in range(d['nsrc']):
                p = self.src(d['src'][i])
                operands.append(((side, i), p))
                if p['kind'] == 'rf':
                    if p['rf'] not in raddrs:
                        raddrs.append(p['rf'])
                elif p['kind'] == 'smimm':
                    if smimm is not None and smimm != p['val']:
                        raise AsmError('two different small immediates')
                    smimm = p['val']
        if len(raddrs) > 2:
            raise AsmError('more than 2 rf addresses in one instruction')
        if smimm is not None and len(raddrs) > 1:
            raise AsmError('small immediate + 2 rf addresses')

        raddr_a = raddrs[0] if len(raddrs) >= 1 else 0
        raddr_b = raddrs[1] if len(raddrs) >= 2 else \
            (SMALL_IMM.index(smimm) if smimm is not None else 0)

        def mux_of(p):
            if p['kind'] == 'mux':
                return p['mux']
            if p['kind'] == 'smimm':
                return 7
            if p['rf'] == raddr_a:
                return 6
            return 7

        for (side, i), p in operands:
            d = a if side == 'a' else m
            d['mux'][i] = mux_of(p)

        # assemble the two halves
        word = 0
        word = fld_set(word, raddr_a, 11, 6)
        word = fld_set(word, raddr_b, 5, 0)
        word = self._pack_add(word, a)
        word = self._pack_mul(word, m)

        # signals
        sigflags = list(sig)
        smimm_sig = smimm is not None
        if smimm_sig:
            sigflags.append('smimm_b')
        key = '+'.join(sigflags) if sigflags else ''
        if key not in SIG42_INV:
            raise AsmError('no sig encoding for %r' % key)
        signum = SIG42_INV[key]
        word = fld_set(word, signum, 57, 53)

        if any(s in SIG_WRITES_ADDR for s in sigflags):
            if sigdst is None:
                raise AsmError('sig %s needs sigdst' % key)
            w, magic = self.dst(sigdst)
            word = fld_set(word, w | (64 if magic else 0), 52, 46)
        else:
            if flags:
                word = fld_set(word, self.flags_pack(**flags), 52, 46)
        self.raw(word, comment)

    def _norm_op(self, op):
        """op tuple: (name, dst, srcs...) -> dict"""
        if op is None:
            return None
        name = op[0]
        d = {'name': name, 'dst': op[1], 'src': list(op[2:])}
        if name in ADD2 or name in MUL2:
            d['nsrc'] = 2
        elif name in ADD1:
            d['nsrc'] = 1
        elif name in ADD0 or name in ('nop',):
            d['nsrc'] = 0
        elif name in ('mov', 'fmov'):
            d['nsrc'] = 1
        else:
            raise AsmError('unknown op %s' % name)
        if len(d['src']) != d['nsrc']:
            raise AsmError('op %s needs %d srcs, got %d' %
                           (name, d['nsrc'], len(d['src'])))
        d['mux'] = [0] * max(d['nsrc'], 1)
        return d

    def _pack_add(self, word, a):
        if a is None:
            a = {'name': 'nop', 'dst': None, 'src': [], 'nsrc': 0,
                 'mux': [0]}
        name = a['name']
        waddr, magic = self.dst(a['dst']) if a['dst'] else \
            (MAGIC42_INV['nop'], True)
        word = fld_set(word, waddr, 37, 32)
        if magic:
            word |= (1 << 44)

        if name in ADD2:
            opcode = ADD2[name]
            ma, mb = a['mux'][0], a['mux'][1]
            if name in ('fadd', 'fmin', 'fcmp', 'fsub'):
                # float ops: unpacks NONE=1; ordering rule for fadd/fmin
                au, bu = 1, 1
                if name == 'fadd':
                    if au * 8 + ma > bu * 8 + mb:
                        ma, mb = mb, ma
                if name == 'fmin':
                    if not (au * 8 + ma > bu * 8 + mb):
                        ma, mb = mb, ma
                    opcode = 128
                opcode |= au << 2 | bu
            word = fld_set(word, ma, 14, 12)
            word = fld_set(word, mb, 17, 15)
            word = fld_set(word, opcode, 31, 24)
            return word

        if name in ADD1:
            opcode, mb = ADD1[name]
            if name in ('fround', 'ftoin', 'ftrunc', 'ftoiz', 'ffloor',
                        'ftouz', 'fceil', 'ftoc', 'fdx', 'fdy'):
                opcode |= 1 << 2   # a.unpack = NONE
            word = fld_set(word, a['mux'][0], 14, 12)
            word = fld_set(word, mb, 17, 15)
            word = fld_set(word, opcode, 31, 24)
            return word

        if name in ADD0:
            opcode, mb, ma = ADD0[name]
            word = fld_set(word, ma, 14, 12)
            word = fld_set(word, mb, 17, 15)
            word = fld_set(word, opcode, 31, 24)
            return word

        raise AsmError('add op %s not supported' % name)

    def _pack_mul(self, word, m):
        if m is None:
            # canonical M_NOP: op 15, mux_b 4, mux_a 0, waddr nop(magic)
            word = fld_set(word, MAGIC42_INV['nop'], 43, 38)
            word |= (1 << 45)
            word = fld_set(word, 0, 20, 18)
            word = fld_set(word, 4, 23, 21)
            word = fld_set(word, 15, 63, 58)
            return word
        name = m['name']
        waddr, magic = self.dst(m['dst']) if m['dst'] else (6, False)
        word = fld_set(word, waddr, 43, 38)
        if magic:
            word |= (1 << 45)

        if name in MUL2:
            opcode = MUL2[name]
            if name == 'fmul':
                opcode |= 1 << 2 | 1 << 0   # unpacks NONE
            word = fld_set(word, m['mux'][0], 20, 18)
            word = fld_set(word, m['mux'][1], 23, 21)
            word = fld_set(word, opcode, 63, 58)
            return word
        if name == 'mov':
            word = fld_set(word, m['mux'][0], 20, 18)
            word = fld_set(word, 7, 23, 21)
            word = fld_set(word, 15, 63, 58)
            return word
        if name == 'fmov':
            word = fld_set(word, m['mux'][0], 20, 18)
            word = fld_set(word, 1, 23, 21)  # pack NONE, unpack NONE
            word = fld_set(word, 14, 63, 58)
            return word
        if name == 'nop':
            word = fld_set(word, MAGIC42_INV['nop'], 43, 38)
            word |= (1 << 45)
            word = fld_set(word, 0, 20, 18)
            word = fld_set(word, 4, 23, 21)
            word = fld_set(word, 15, 63, 58)
            return word
        raise AsmError('mul op %s not supported' % name)

    # ---- branch ---------------------------------------------------------
    def branch(self, target, cond='always', comment=''):
        """target: label name (resolved in finish()) or int offset"""
        word = fld_set(0, 16, 57, 53)
        condmap = {'always': 0, 'a0': 2, 'na0': 3, 'alla': 4, 'anyna': 5,
                   'anya': 6, 'allna': 7}
        word = fld_set(word, condmap[cond], 34, 32)
        word = fld_set(word, 0, 22, 21)      # msfign
        word = fld_set(word, 1, 13, 12)      # bdi = DEST_REL
        # ub = 0: the uniform stream is left as is
        self.raw(word, 'b%s %s' % (cond, target) +
                 ((' ; ' + comment) if comment else ''))
        self.rows[-1] = (word, self.rows[-1][1], target)  # mark fixup

    def nop(self, n=1, comment=''):
        for _ in range(n):
            self.alu(comment=comment or 'nop')

    def ldunif(self, comment='ldunif'):
        self.alu(sig=('ldunif',), comment=comment)

    # ---- finalize ---------------------------------------------------------
    def finish(self):
        out = []
        for i, row in enumerate(self.rows):
            word, comment = row[0], row[1]
            if len(row) > 2:   # branch fixup
                # Mesa: offset = (target_ip - (branch_ip + 4)) * 8
                off = (self.labels[row[2]] - (i + 4)) * 8
                word = fld_set(word, (off & ~0xff000000) >> 3, 55, 35)
                word = fld_set(word, (off >> 24) & 0xff, 31, 24)
            out.append((word, comment))
        return out


# --------------------------------------------------------------------------
# VC4 (V3D 2.1, BCM2837 / Pi3) assembler
# --------------------------------------------------------------------------
# Field layout and tables transcribed 1:1 from Linux
# drivers/gpu/drm/vc4/vc4_qpu_defines.h + vc4_validate_shaders.c:
#   sig 63:60   unpack 59:57   PM 56   pack 55:52
#   cond_add 51:49  cond_mul 48:46  SF 45  WS 44
#   waddr_add 43:38  waddr_mul 37:32  op_mul 31:29  op_add 28:24
#   raddr_a 23:18  raddr_b/small_imm 17:12
#   add_a 11:9  add_b 8:6  mul_a 5:3  mul_b 2:0
# Constraints (vc4_validate_shaders.c):
#   - raddr_a/raddr_b belong to the instruction that uses them, but a
#     register file READ needs the raddr present on the PREVIOUS
#     instruction as well (one-instruction delay - the generator keeps
#     every live rf address resident across consecutive instructions);
#   - at most 2 distinct rf addresses per instruction;
#   - SF (flag push) and a cond on the same pipe are illegal; uniform
#     reads need sig NONE (small-imm sig would claim raddr_b);
#   - branches are absolute (targets resolved at finish()), the two
#     waddrs must be NOP, 3 delay slots; thread end is thrsw followed
#     by 2 executed delay slots (the GPU_FFT epilogue).

SIG21 = {0: 'swbp', 1: '', 2: 'thrsw', 3: 'prog_end', 4: 'sbwait',
         5: 'sbunlock', 6: 'lthrsw', 7: 'covld', 8: 'colorld',
         9: 'colorlde', 10: 'ldtmu0', 11: 'ldtmu1', 12: 'amskld',
         13: 'smimm', 14: 'loadimm', 15: 'branch'}
SIG21_INV = {v: k for k, v in SIG21.items()}

MAGIC21 = {32: 'r0', 33: 'r1', 34: 'r2', 35: 'r3', 36: 'noswap',
           37: 'r5', 38: 'host', 39: 'nop', 40: 'unifa', 41: 'quadxy',
           42: 'msflags', 43: 'tlbs', 44: 'tlbz', 45: 'tlb', 46: 'tlba',
           48: 'vpm', 49: 'vpmvcd', 50: 'vpmaddr', 51: 'mutex',
           52: 'recip', 53: 'recipsqrt', 54: 'exp', 55: 'log',
           56: 'tmu0_s', 57: 'tmu0_t', 58: 'tmu0_r', 59: 'tmu0_b',
           60: 'tmu1_s', 61: 'tmu1_t', 62: 'tmu1_r', 63: 'tmu1_b'}
MAGIC21_INV = {v: k for k, v in MAGIC21.items()}

ADD21 = {'fadd': 1, 'fsub': 2, 'fmin': 3, 'fmax': 4, 'add': 12,
         'sub': 13, 'shr': 14, 'asr': 15, 'ror': 16, 'shl': 17,
         'min': 18, 'max': 19, 'and': 20, 'or': 21, 'xor': 22,
         'v8adds': 30, 'v8subs': 31}
ADD21_1 = {'ftoi': 7, 'itof': 8, 'not': 23, 'clz': 24}
MUL21 = {'fmul': 1, 'mul24': 2, 'v8muld': 3, 'v8min': 4, 'v8max': 5,
         'v8adds': 6, 'v8subs': 7}

COND21 = {'': 1, 'ifz': 2, 'ifnz': 3, 'ifn': 4, 'ifnn': 5}
# Mesa vc4_qpu_defines.h enum qpu_branch_cond
BR_COND21 = {'allz': 0, 'allnz': 1, 'anyz': 2, 'anynz': 3,
             'alln': 4, 'allnn': 5, 'anyn': 6, 'anynn': 7,
             'always': 15}

# GPU_FFT's nop: op add/mul NOP, conds 0/0, waddrs nop (raw word below).
# VC4 field layout (Mesa vc4_qpu_defines.h, verified against gpu_fft
# shader_256.hex words):
#   sig 63:60, unpack 59:57, pm 56, pack 55:52,
#   cond-add 51:49, cond-mul 48:46, SF 45, bit 44 unused (keep 0),
#   waddr_add 43:38, waddr_mul 37:32, op_add 28:24, op_mul 31:29,
#   raddr_a 23:18, raddr_b/small_imm 17:12 (with sig smimm the field
#   holds the SMALL_IMM21 index, which equals the value for 0..16),
#   add_a 11:9  add_b 8:6  mul_a 5:3  mul_b 2:0.
# Uniform reads use raddr_a 32, elem_num raddr 38; a READ of raddr 50
# (VPM_LD_WAIT) blocks until every pending VPM store has been DMAed.
NOP21_WORD = 0x100009e7009e7000
UNIF_RADDR21 = 32
ELEM_RADDR21 = 38
VPM_WAIT_RADDR21 = 50
# VC4 small immediates: raddr_b field = table index (= value for 0..16,
# verified against GPU_FFT words: #1 -> 1, #4 -> 4, #8 -> 8)
SMALL_IMM21 = list(range(17)) + [32, 64, 128]


class K21:
    """VC4 (V3D 2.1) kernel assembler."""

    def __init__(self, name='kernel'):
        self.name = name
        self.rows = []          # (word, comment)
        self.labels = {}

    def label(self, name):
        if name in self.labels:
            raise AsmError('duplicate label %s' % name)
        self.labels[name] = len(self.rows)

    def raw(self, word, comment=''):
        self.rows.append((word, comment))

    # ---- operand parsing ---------------------------------------------
    @staticmethod
    def src(s):
        if s in ('r0', 'r1', 'r2', 'r3', 'r4', 'r5'):
            return {'kind': 'mux', 'mux': int(s[1])}
        if s == 'a' or s == 'b':
            return {'kind': 'mux', 'mux': 6 if s == 'a' else 7}
        if s == 'unif':
            return {'kind': 'unif', 'addr': UNIF_RADDR21}
        if s == 'elem':
            return {'kind': 'elem', 'addr': ELEM_RADDR21}
        # reading raddr 50 (VPM_LD_WAIT) stalls until all VPM stores
        # finished: GPU_FFT's 'mov -, vw_wait' = or -, b, b with rb=50
        if s == 'vw_wait':
            return {'kind': 'vw_wait'}
        if s.startswith('rf'):
            n = int(s[2:])
            if n > 63:
                raise AsmError('bad rf %s' % s)
            return {'kind': 'rf', 'rf': n, 'addr': n}
        if s.startswith('#'):
            v = int(s[1:], 0)
            if v in SMALL_IMM21:
                return {'kind': 'smimm', 'val': v}
            # arbitrary 32-bit constant: 'mov dst, #imm' becomes a sig
            # loadimm instruction with the value in bits 31:0
            return {'kind': 'imm', 'val': v & 0xFFFFFFFF}
        raise AsmError('bad operand %r' % s)

    @staticmethod
    def dst(s):
        if s.startswith('rf'):
            n = int(s[2:])
            if n > 63:
                raise AsmError('bad dst %r' % s)
            return n, False
        if s in MAGIC21_INV:
            return MAGIC21_INV[s], True
        raise AsmError('bad dst %r' % s)

    # ---- flags: cond-add 51:49, cond-mul 48:46, SF bit 45. GPU_FFT
    #      convention: pipes without a dst get cond NEVER (0) ------------

    # ---- the general ALU emit ------------------------------------------
    def alu(self, a=None, m=None, sig='', flags=None, comment=''):
        a = self._norm_op(a)
        m = self._norm_op(m)
        flags = flags or {}

        raddrs = []
        smimm = None
        big_imm = None
        vw_wait = False
        operands = []
        for side, d in (('a', a), ('m', m)):
            if not d or d['nsrc'] == 0:
                continue
            for i in range(d['nsrc']):
                p = self.src(d['src'][i])
                operands.append(((side, i), p))
                if p['kind'] == 'rf':
                    if p['rf'] > 31:
                        raise AsmError('rf%d not readable on VC4' % p['rf'])
                    if p['rf'] not in raddrs:
                        raddrs.append(p['rf'])
                elif p['kind'] == 'smimm':
                    if smimm is not None and smimm != p['val']:
                        raise AsmError('two different small immediates')
                    smimm = p['val']
                elif p['kind'] == 'imm':
                    if big_imm is not None and big_imm != p['val']:
                        raise AsmError('two different immediates')
                    big_imm = p['val']
                elif p['kind'] in ('unif', 'elem'):
                    if p['kind'] not in raddrs:
                        raddrs.append(p['kind'])
                elif p['kind'] == 'vw_wait':
                    vw_wait = True
        if big_imm is not None:
            # sig loadimm: the constant sits in bits 31:0 and is routed
            # straight to the waddr fields (no op/mux fields at all);
            # one dst per pipe allowed (GPU_FFT 'mov rx, imm')
            if sig not in ('', 'loadimm'):
                raise AsmError('32-bit immediate needs sig loadimm')
            if smimm is not None or raddrs or vw_wait:
                raise AsmError('immediate instruction takes no other src')
            if not (a and a['dst']) and not (m and m['dst']):
                raise AsmError('mov dst, #imm needs a dst')
            wa = self.dst(a['dst'])[0] if (a and a['dst']) else 39
            wm = self.dst(m['dst'])[0] if (m and m['dst']) else 39
            ac = 1 if (a and a['dst']) else 0
            mc = 1 if (m and m['dst']) else 0
            word = big_imm & 0xFFFFFFFF
            word = fld_set(word, (ac << 3) | mc, 51, 46)
            word = fld_set(word, wa, 43, 38)
            word = fld_set(word, wm, 37, 32)
            word = fld_set(word, SIG21_INV['loadimm'], 63, 60)
            self.raw(word, comment)
            return
        if len(raddrs) > (1 if (smimm is not None or vw_wait) else 2):
            raise AsmError('more rf addresses than one instruction carries')
        if smimm is not None and vw_wait:
            raise AsmError('small immediate and vw_wait clash on raddr_b')
        if smimm is not None and sig not in ('', 'smimm'):
            raise AsmError('small immediate needs sig smimm')
        if smimm is not None and sig == '':
            sig = 'smimm'
        if sig == 'smimm' and smimm is None:
            raise AsmError('sig smimm without a small immediate')
        for k in ('unif', 'elem'):
            if k in raddrs and sig not in ('', 'smimm'):
                raise AsmError('%s read needs sig NONE' % k)

        def addr_of(k):
            if isinstance(k, str):
                return {'unif': UNIF_RADDR21, 'elem': ELEM_RADDR21}[k]
            return k

        # GPU_FFT leaves unused raddrs at 39 (nop); a vw_wait read must
        # sit in raddr_b (reading raddr 50 = VPM_LD_WAIT blocks until
        # every pending VPM->VDW DMA finished)
        raddr_a = addr_of(raddrs[0]) if raddrs else 39
        if vw_wait:
            raddr_b = VPM_WAIT_RADDR21
        elif len(raddrs) > 1:
            raddr_b = addr_of(raddrs[1])
        elif smimm is not None:
            raddr_b = SMALL_IMM21.index(smimm)
        else:
            raddr_b = 39

        def mux_of(p):
            if p['kind'] == 'mux':
                return p['mux']
            if p['kind'] in ('smimm', 'vw_wait'):
                return 7
            return 6 if p['addr'] == raddr_a else 7

        for (side, i), p in operands:
            d = a if side == 'a' else m
            d['mux'][i] = mux_of(p)

        # cond defaults follow GPU_FFT: a pipe that writes nothing gets
        # NEVER (0); setf forces the add pipe unconditional
        ac = flags.get('ac')
        mc = flags.get('mc')
        ac = COND21[ac] if isinstance(ac, str) else ac
        mc = COND21[mc] if isinstance(mc, str) else mc
        if ac is None:
            ac = 1 if ((a and a['dst']) or flags.get('sf')) else 0
        if mc is None:
            mc = 1 if (m and m['dst']) else 0
        if flags.get('sf') and (ac != 1 or mc not in (0, 1)):
            raise AsmError('VC4: setf with a cond on the same instruction')

        word = 0
        word = fld_set(word, raddr_a, 23, 18)
        word = fld_set(word, raddr_b, 17, 12)
        word = self._pack_add(word, a)
        word = self._pack_mul(word, m)
        word = fld_set(word, (ac << 3) | mc, 51, 46)
        word = fld_set(word, 1 if flags.get('sf') else 0, 45, 45)
        if sig not in SIG21_INV:
            raise AsmError('no sig encoding for %r' % sig)
        word = fld_set(word, SIG21_INV[sig], 63, 60)
        self.raw(word, comment)

    def _norm_op(self, op):
        if op is None:
            return None
        name = op[0]
        d = {'name': name, 'dst': op[1], 'src': list(op[2:])}
        if name in ADD21 or name in MUL21:
            d['nsrc'] = 2
        elif name in ADD21_1:
            d['nsrc'] = 1
        elif name in ('mov', 'nop'):
            d['nsrc'] = 1 if name == 'mov' else 0
        else:
            raise AsmError('unknown op %s' % name)
        if len(d['src']) != d['nsrc']:
            raise AsmError('op %s needs %d srcs, got %d' %
                           (name, d['nsrc'], len(d['src'])))
        d['mux'] = [0] * max(d['nsrc'], 1)
        return d

    def _pack_add(self, word, a):
        if a is None:
            word = fld_set(word, 39, 43, 38)          # waddr add = nop
            word = fld_set(word, 0, 28, 24)           # op add = NOP
            return word
        name = a['name']
        waddr = self.dst(a['dst'])[0] if a['dst'] else 39
        word = fld_set(word, waddr, 43, 38)
        if name in ADD21:
            word = fld_set(word, a['mux'][0], 11, 9)
            word = fld_set(word, a['mux'][1], 8, 6)
            word = fld_set(word, ADD21[name], 28, 24)
        elif name in ADD21_1:
            word = fld_set(word, a['mux'][0], 11, 9)
            word = fld_set(word, ADD21_1[name], 28, 24)
        elif name == 'mov':
            # VC4 has no mov op: OR src, src on the ADD pipe
            word = fld_set(word, a['mux'][0], 11, 9)
            word = fld_set(word, a['mux'][0], 8, 6)
            word = fld_set(word, 21, 28, 24)
        elif name == 'nop':
            word = fld_set(word, 0, 28, 24)
        else:
            raise AsmError('add op %s not supported' % name)
        return word

    def _pack_mul(self, word, m):
        if m is None:
            word = fld_set(word, 39, 37, 32)          # waddr mul = nop
            word = fld_set(word, 0, 31, 29)           # op mul = NOP
            return word
        name = m['name']
        waddr = self.dst(m['dst'])[0] if m['dst'] else 39
        word = fld_set(word, waddr, 37, 32)
        if name in MUL21:
            word = fld_set(word, m['mux'][0], 5, 3)
            word = fld_set(word, m['mux'][1], 2, 0)
            word = fld_set(word, MUL21[name], 31, 29)
        elif name == 'mov':
            # no mov on the MUL pipe (mul24 src, src = src*src): route
            # the move through the ADD pipe
            raise AsmError('VC4 mov must use the ADD pipe')
        elif name == 'nop':
            word = fld_set(word, 0, 31, 29)
        elif name in ('add', 'sub'):
            # VC4 small-immediate instructions only: the MUL pipe gains
            # add/sub (MUL opcodes 0/1) - GPU_FFT's msub bookkeeping
            word = fld_set(word, m['mux'][0], 5, 3)
            word = fld_set(word, m['mux'][1], 2, 0)
            word = fld_set(word, 0 if name == 'add' else 1, 31, 29)
        else:
            raise AsmError('mul op %s not supported' % name)
        return word

    # ---- branch: RELATIVE, cond in 55:52 (Mesa qpu_branch_cond),
    #      rel bit 51, (target-(ip+4))*8 in 31:0, waddrs nop ------------
    def branch(self, target, cond='always', comment=''):
        if cond not in BR_COND21:
            raise AsmError('bad VC4 branch cond %r' % cond)
        word = fld_set(0, SIG21_INV['branch'], 63, 60)
        word = fld_set(word, BR_COND21[cond], 55, 52)
        word |= (1 << 51)                             # relative
        word = fld_set(word, 39, 43, 38)              # waddr add = nop
        word = fld_set(word, 39, 37, 32)              # waddr mul = nop
        self.raw(word, 'brr.%s %s' % (cond, target) +
                 ((' ; ' + comment) if comment else ''))
        self.rows[-1] = (word, self.rows[-1][1], target)

    def nop(self, n=1, comment=''):
        for _ in range(n):
            self.raw(NOP21_WORD, comment or 'nop')

    def nop_reads(self, ra=39, rb=39, comment=''):
        """nop that keeps raddr_a/raddr_b resident for the NEXT
        instruction's register-file reads (one-instruction delay)"""
        word = fld_set(NOP21_WORD, ra & 0x3F, 23, 18)
        word = fld_set(word, rb & 0x3F, 17, 12)
        self.raw(word, comment or 'nop (raddrs)')

    # ---- scoreboard: GPU_FFT word shape 0xe80009e700000000 | id (sig
    #      loadimm + unpack mode 4, waddrs nop); acquire ids 16+n ------
    def sb_acq(self, n, comment=''):
        self._sb(16 + n, comment or 'sacq(%d)' % n)

    def sb_rel(self, n, comment=''):
        self._sb(n, comment or 'srel(%d)' % n)

    def _sb(self, v, comment):
        self.raw(0xe80009e700000000 | (v & 0xFF), comment)

    # ---- GPU_FFT thread exit: OR a nonzero value into host (waddr 38),
    #      then prog_end nop + 2 delay-slot nops; r0 must be free ------
    def exit(self, comment=''):
        # GPU_FFT uses a dedicated 'flag' rf register; we load r0 := 1
        # (forced through the loadimm path to match GPU_FFT word shape)
        self.raw(0xe002082700000001, 'mov r0, #1   ; exit flag')
        self.alu(a=('mov', 'host', 'r0'),
                 comment=comment or 'mov interrupt, r0')
        self.raw(fld_set(NOP21_WORD, SIG21_INV['prog_end'], 63, 60),
                 'nop; nop; thrend')
        self.nop(2, 'exit delay slots')

    # ---- finalize -------------------------------------------------------
    def finish(self):
        out = []
        for ip, row in enumerate(self.rows):
            word, comment = row[0], row[1]
            if len(row) > 2:   # branch fixup: relative byte offset
                tip = self.labels[row[2]]
                off = (tip - (ip + 4)) * 8
                if off < -(1 << 31) or off > (1 << 31) - 1:
                    raise AsmError('branch offset out of range')
                word = fld_set(word, off & 0xFFFFFFFF, 31, 0)
            out.append((word, comment))
        return out


# --------------------------------------------------------------------------
# VC4 disassembly
# --------------------------------------------------------------------------

MAGIC21_TXT = dict(MAGIC21)
MAGIC21_TXT.update({39: 'nop', 48: 'vpm'})


def _src21(mux, ra, rb, smimm):
    if mux <= 5:
        return 'r%d' % mux
    if mux == 6:
        return {32: 'unif', 35: 'vary', 38: 'elem', 39: 'nop',
                48: 'vpm', 49: 'vpm_ld_busy', 50: 'vpm_ld_wait',
                51: 'mutex'}.get(ra, 'rf%d' % ra)
    if smimm:
        if rb < len(SMALL_IMM21):
            return str(SMALL_IMM21[rb])
        return 'imm?%d' % rb
    return {32: 'unif', 35: 'vary', 38: 'elem', 39: 'nop',
            48: 'vpm', 49: 'vpm_ld_busy', 50: 'vpm_ld_wait',
            51: 'mutex'}.get(rb, 'rf%d' % rb)


def decode21(word, ip=0):
    if fld(word, 63, 60) == 15:
        cond = fld(word, 55, 52)
        cname = {0: 'allz', 1: 'allnz', 2: 'anyz', 3: 'anynz',
                 4: 'alln', 5: 'allnn', 6: 'anyn', 7: 'anynn',
                 15: 'always'}.get(cond, 'cond?%d' % cond)
        reg = (word >> 50) & 1
        rel = (word >> 51) & 1
        tgt = fld(word, 31, 0)
        extra = ''
        if reg:
            extra = ' via rf%d' % fld(word, 49, 45)
        if rel:
            off = (tgt ^ (1 << 31)) - (1 << 31)
            tgt = ip + 4 + off // 8
            extra += ' (rel %d)' % off
        return 'branch %s -> %d%s' % (cname, tgt, extra)

    sig = fld(word, 63, 60)
    sigtxt = SIG21.get(sig, 'sig?%d' % sig)
    wa = fld(word, 43, 38)
    wm = fld(word, 37, 32)
    if sig == 14:                                     # load imm
        v = fld(word, 31, 0)
        if wa == 39 and wm == 39:
            sb = 'sacq(%d)' % (v - 16) if v >= 16 else 'srel(%d)' % v
            return 'mov -, %s   ; loadimm 0x%08x' % (sb, v)
        mode = fld(word, 59, 57)
        dst = MAGIC21_TXT.get(wa, 'rf%d' % wa) if wa != wm else \
            MAGIC21_TXT.get(wm, 'rf%d' % wm)
        return 'mov %s, #0x%08x%s   ; loadimm' % (
            dst, v, {1: ' (i2)', 3: ' (u2)'}.get(mode, ''))

    unpack = fld(word, 59, 57)
    pm = (word >> 56) & 1
    pack = fld(word, 55, 52)
    ac = fld(word, 51, 49)
    mc = fld(word, 48, 46)
    sf = bool(word & (1 << 45))
    ws = bool(word & (1 << 44))
    ra = fld(word, 23, 18)
    rb = fld(word, 17, 12)
    smimm = sig == 13

    def fltxt(cond):
        t = {0: 'never', 1: '', 2: 'ifz', 3: 'ifnz',
             4: 'ifn', 5: 'ifnn', 6: 'cs', 7: 'cc'}.get(
                 cond, '?%d' % cond)
        if sf:
            t += '.setf'
        return t

    parts = []
    op_add = fld(word, 28, 24)
    dst_a = MAGIC21_TXT.get(wa, 'rf%d' % wa) if wa >= 32 else 'rf%d' % wa
    if op_add in (12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 1, 2,
                  3, 4, 5, 6, 30, 31):
        name = {12: 'add', 13: 'sub', 14: 'shr', 15: 'asr', 16: 'ror',
                17: 'shl', 18: 'min', 19: 'max', 20: 'and', 21: 'or',
                22: 'xor', 1: 'fadd', 2: 'fsub', 3: 'fmin', 4: 'fmax',
                5: 'fminabs', 6: 'fmaxabs', 30: 'v8adds',
                31: 'v8subs'}[op_add]
        A = _src21(fld(word, 11, 9), ra, rb, smimm)
        B = _src21(fld(word, 8, 6), ra, rb, smimm)
        if wa == 39:
            parts.append('%s%s -, %s, %s' % (name, fltxt(ac), A, B))
        else:
            parts.append('%s%s %s, %s, %s' %
                         (name, fltxt(ac), dst_a, A, B))
    elif op_add in (23, 24, 7, 8):
        name = {23: 'not', 24: 'clz', 7: 'ftoi', 8: 'itof'}[op_add]
        A = _src21(fld(word, 11, 9), ra, rb, smimm)
        parts.append('%s%s %s, %s' % (name, fltxt(ac), dst_a, A))
    elif op_add == 247:
        parts.append('tmuwt%s %s' % (fltxt(ac), dst_a))
    elif op_add == 0:
        pass                                          # add-side nop
    else:
        parts.append('add?%d' % op_add)

    op_mul = fld(word, 31, 29)
    dst_m = MAGIC21_TXT.get(wm, 'rf%d' % wm) if wm >= 32 else 'rf%d' % wm
    if smimm and wm != 39 and op_mul in (0, 1):
        # small-immediate MUL pipe add/sub (GPU_FFT msub bookkeeping)
        name = 'madd' if op_mul == 0 else 'msub'
        A = _src21(fld(word, 5, 3), ra, rb, smimm)
        B = _src21(fld(word, 2, 0), ra, rb, smimm)
        parts.append('%s%s %s, %s, %s' % (name, fltxt(mc), dst_m, A, B))
    elif op_mul in (1, 2, 3, 4, 5, 6, 7):
        name = {1: 'fmul', 2: 'mul24', 3: 'v8muld', 4: 'v8min',
                5: 'v8max', 6: 'v8adds', 7: 'v8subs'}[op_mul]
        A = _src21(fld(word, 5, 3), ra, rb, smimm)
        B = _src21(fld(word, 2, 0), ra, rb, smimm)
        if wm == 39:
            parts.append('%s%s -, %s, %s' % (name, fltxt(mc), A, B))
        else:
            parts.append('%s%s %s, %s, %s' %
                         (name, fltxt(mc), dst_m, A, B))
    elif op_mul == 0:
        pass                                          # mul-side nop
    else:
        parts.append('mul?%d' % op_mul)

    if not parts:
        parts = ['nop']
    out = '; '.join(parts)
    if sigtxt:
        out += '  ; ' + sigtxt
    if ws:
        out += ' (ws)'
    if unpack:
        out += ' unpk=%d' % unpack
    if pm or pack:
        out += ' pack=%d%s' % (pack, '(pm)' if pm else '')
    return out


# --------------------------------------------------------------------------
# kernel extraction + CLI
# --------------------------------------------------------------------------

def extract_kernels(path):
    """parse a g2d_qpu_kernels.h -> {name: [words]}"""
    src = open(path).read()
    kerns = {}
    for m in re.finditer(
            r'g2d_qpu_(\w+)\[\]\s*=\s*\{(.*?)\};', src, re.S):
        words = [int(x, 16) for x in
                 re.findall(r'0x([0-9a-fA-F]+)ULL', m.group(2))]
        kerns[m.group(1)] = words
    return kerns


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    cmd = sys.argv[1]
    if cmd == 'decode71':
        for name, words in extract_kernels(sys.argv[2]).items():
            print('=== %s (%d instrs) ===' % (name, len(words)))
            for i, w in enumerate(words):
                print('%4d: %016x  %s' % (i, w, decode(w, i, 71)))
    elif cmd == 'decode42':
        for name, words in extract_kernels(sys.argv[2]).items():
            print('=== %s (%d instrs) ===' % (name, len(words)))
            for i, w in enumerate(words):
                print('%4d: %016x  %s' % (i, w, decode(w, i, 42)))
    elif cmd == 'decode21':
        for name, words in extract_kernels(sys.argv[2]).items():
            print('=== %s (%d instrs) ===' % (name, len(words)))
            for i, w in enumerate(words):
                print('%4d: %016x  %s' % (i, w, decode21(w, i)))
    elif cmd == 'check42':
        selftest()
    elif cmd == 'check21':
        selftest21()


def selftest():
    k = K42()
    k.nop(2)
    k.alu(a=('tidx', 'rf4'), comment='tidx rf4')
    k.alu(a=('eidx', 'rf3'), comment='eidx rf3')
    k.alu(a=('add', 'rf5', 'rf4', 'rf3'), comment='add rf5, rf4, rf3')
    k.alu(a=('add', 'rf5', 'rf4', '#3'), comment='add rf5, rf4, 3')
    k.alu(m=('mov', 'rf6', 'r5'), comment='mov rf6, r5')
    k.alu(m=('umul24', 'rf7', 'rf4', 'rf5'), comment='umul24')
    k.alu(sig=('ldunif',), comment='ldunif')
    k.alu(a=('sub', 'r5', 'r5', 'rf0'), sig=('ldtmu',), sigdst='rf8',
          comment='ldtmu .rf8')
    k.alu(m=('mov', 'r5', 'rf10'), sig=('wrtmuc',), comment='wrtmuc')
    k.branch('end', 'anya')
    k.label('end')
    k.alu(a=('tmuwt', None), comment='tmuwt')
    words = k.finish()
    for i, (w, c) in enumerate(words):
        print('%4d: %016x  %-40s | %s' % (i, w, decode(w, i, 42), c))


def selftest21():
    # ---- decode check against known-good GPU_FFT words -----------------
    # (gpu_fft/hex/shader_256.hex: {low, high} pairs, 64-bit word =
    #  (high << 32) | low; expected text tokens from the file's comments)
    pairs = [
        (0x009e7000, 0x100009e7, 'nop'),
        (0x15827d80, 0x10020227, 'unif'),            # mov rx, unif
        (0x14981dc0, 0xd00229e7, 'and.setf'),        # and.setf -, elem, #1
        (0x0c9e7040, 0x10020827, 'add r0'),
        (0x159e7040, 0x10020867, 'or'),
        (0x009e7000, 0xa00009e7, 'ldtmu0'),
        (0xfffff9d0, 0xf0f809e7, 'rel -1584'),       # brr -, r:loop
        (0x00000019, 0xe80009e7, 'sacq(9)'),         # scoreboard acquire
        (0x00000001, 0xe80009e7, 'srel(1)'),         # scoreboard release
        (0x159f2fc0, 0x100009e7, 'vpm_ld_wait'),     # mov -, vw_wait
        (0x159c3fc0, 0x100209a7, 'host'),            # mov interrupt, rf3
        (0x009e7000, 0x300009e7, 'prog_end'),        # nop; nop; thrend
    ]
    ok = True
    for lo, hi, want in pairs:
        txt = decode21((hi << 32) | lo)
        hit = want in txt
        ok = ok and hit
        print('%016x  %-48s %s (want %r)' %
              ((hi << 32) | lo, txt, 'OK' if hit else 'MISS', want))

    # ---- encoder checks: verify the critical fields --------------------
    k = K21()
    k.alu(a=('mov', 'rf25', 'unif'), comment='mov rf25, unif')
    k.alu(a=('mov', 'rf26', 'elem'), comment='mov rf26, elem')
    k.alu(a=('add', 'r0', 'r0', 'r2'), comment='add r0, r0, r2')
    k.alu(a=('and', None, 'elem', '#1'), flags={'sf': True},
          comment='and.setf -, elem, #1')
    k.alu(a=('sub', 'rf21', 'rf21', '#1'), flags={'sf': True},
          comment='sub.setf rf21, rf21, #1')
    k.alu(a=('mov', 'rf19', 'rf25'), comment='mov rf19, rf25')
    k.alu(m=('mul24', 'rf18', 'rf3', '#4'),
          comment='mul24 rf18, rf3, #4')
    k.alu(a=('mov', 'r0', '#0x88104000'),
          comment='mov r0, vdw_setup_0(16,16,dma_h32(0,0))')
    k.alu(a=('mov', 'vpm', 'rf20'), comment='mov vpm, rf20')
    k.alu(a=('mov', 'vpmvcd', 'r0'), comment='mov vw_setup, r0')
    k.alu(a=('mov', 'vpmaddr', 'rf21'), comment='mov vw_addr, rf21')
    k.alu(a=('or', None, 'vw_wait', 'vw_wait'), comment='mov -, vw_wait')
    k.alu(sig='ldtmu0', comment='ldtmu0 -> r4')
    k.branch('loop', 'anynz', comment='loop while any counter != 0')
    k.nop(3, comment='branch delay slots')
    k.label('loop')
    k.sb_acq(9)
    k.sb_rel(1)
    k.exit()
    words = [w for w, c in k.finish()]

    checks = [
        # (idx, description, ok-flag)
        (0, 'mov rf25, unif: ra 32, OR, wa 25, mux 6/6, bit44 0',
         fld(words[0], 63, 60) == 1 and fld(words[0], 23, 18) == 32 and
         fld(words[0], 28, 24) == 21 and fld(words[0], 43, 38) == 25 and
         fld(words[0], 11, 9) == 6 and fld(words[0], 8, 6) == 6 and
         not (words[0] >> 44) & 1),
        (1, 'mov rf26, elem: raddr_a 38 + OR op',
         fld(words[1], 23, 18) == 38 and fld(words[1], 28, 24) == 21 and
         fld(words[1], 43, 38) == 26),
        (3, 'and.setf == GPU_FFT word',
         words[3] == 0xd00229e714981dc0),
        (4, 'sub.setf smimm: rb field = value 1, SF',
         fld(words[4], 23, 18) == 21 and fld(words[4], 17, 12) == 1 and
         fld(words[4], 28, 24) == 13 and bool(words[4] & (1 << 45))),
        (6, 'smimm mul24: rb field = 4, rf3',
         fld(words[6], 17, 12) == 4 and fld(words[6], 31, 29) == 2 and
         fld(words[6], 23, 18) == 3),
        (7, 'loadimm mov r0 == GPU_FFT shape',
         words[7] == 0xe002082788104000),
        (8, 'vpm write: waddr 48', fld(words[8], 43, 38) == 48),
        (9, 'vw_setup: waddr 49', fld(words[9], 43, 38) == 49),
        (10, 'vw_addr: waddr 50', fld(words[10], 43, 38) == 50),
        (11, 'vw_wait == GPU_FFT word',
         words[11] == 0x100009e7159f2fc0),
        (12, 'ldtmu0 == GPU_FFT word',
         words[12] == 0xa00009e7009e7000),
        (13, 'branch anynz: cond 3, rel, waddrs nop, offset 0',
         words[13] == 0xf03809e700000000),
        (14, 'nop == GPU_FFT word', words[14] == NOP21_WORD),
        (17, 'sacq(9) == GPU_FFT word', words[17] == 0xe80009e700000019),
        (18, 'srel(1) == GPU_FFT word', words[18] == 0xe80009e700000001),
        (19, 'exit: mov r0, #1', words[19] == 0xe002082700000001),
        (20, 'exit: mov host, r0 (wa 38, OR, mux 0)',
         words[20] == 0x100209a7159e7000),
        (21, 'prog_end == GPU_FFT word',
         words[21] == 0x300009e7009e7000),
        (22, 'exit delay nop', words[22] == NOP21_WORD),
    ]
    for idx, desc, good in checks:
        ok = ok and good
        print('%4d: %016x  %-50s %s' %
              (idx, words[idx], decode21(words[idx], idx),
               'OK' if good else 'FAIL: ' + desc))
    print('check21: %s' % ('OK' if ok else 'FAILED'))


if __name__ == '__main__':
    main()
