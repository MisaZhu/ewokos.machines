#!/usr/bin/env python3
"""Cross-check the emulator's decode of every emitted instruction against
NVIDIA's nvdisasm, the independent authority on sm_87 encodings.

  check_disasm.py <emulator .lst> <nvdisasm output>

The emulator listing prints immediates as #XXXXXXXX and branch targets as
@0xADDR; nvdisasm text is normalized to the same shape before comparing.
"""
import re
import struct
import sys

SPECIAL = {'+INF': 0x7f800000, '-INF': 0xff800000,
           '+QNAN': 0x7fffffff, '-QNAN': 0xffffffff}
INT_RE = re.compile(r'^-?0x[0-9a-f]+$')
FLT_RE = re.compile(r'^[-+]?(\d+\.?\d*([eE][-+]?\d+)?|\.\d+)$')


def imm_bits(tok):
    if tok in SPECIAL:
        return SPECIAL[tok]
    if INT_RE.match(tok):
        return int(tok, 16) & 0xffffffff
    if FLT_RE.match(tok):
        return struct.unpack('<I', struct.pack('<f', float(tok)))[0]
    return None


def load_nvdisasm(path):
    labels, instrs, pending = {}, [], []
    for line in open(path):
        m = re.match(r'^(\.L_x_\d+):', line)
        if m:
            pending.append(m.group(1))
            continue
        m = re.search(r'/\*([0-9a-f]+)\*/\s+(.*?)\s*;', line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        for lab in pending:
            labels[lab] = addr
        pending = []
        instrs.append((addr, m.group(2).strip()))
    return labels, instrs


def normalize(text, labels):
    guard = ''
    if text.startswith('@'):
        guard, text = text.split(None, 1)
        guard += ' '
    parts = text.split(None, 1)
    mnem = parts[0]
    ops = [o.strip() for o in parts[1].split(',')] if len(parts) > 1 else []
    base = mnem.split('.')[0]
    if mnem.startswith('IMAD.WIDE'):
        mnem = 'IMAD.WIDE' + ('.U32' if '.U32' in mnem else '')
    elif base == 'IMAD':
        mnem = 'IMAD'           # nvdisasm aliases (.MOV/.SHL/.IADD/.U32)
    out = []
    for i, o in enumerate(ops):
        o = o.replace('.reuse', '')
        m = re.match(r'^`\((\.L_x_\d+)\)$', o)
        if m:
            out.append('@0x%04x' % labels[m.group(1)])
            continue
        if base == 'LOP3' and i == 4:
            out.append(o)       # the LUT byte
            continue
        bits = imm_bits(o)
        out.append('#%08x' % bits if bits is not None else o)
    return guard + mnem + (' ' + ', '.join(out) if out else '')


def main():
    emu = [l.rstrip('\n').split(' ', 1) for l in open(sys.argv[1])]
    labels, nv = load_nvdisasm(sys.argv[2])
    if len(emu) != len(nv):
        print('instruction count differs: emulator %d, nvdisasm %d'
              % (len(emu), len(nv)))
        return 1
    bad, forms = 0, set()
    for (eaddr, etext), (naddr, ntext) in zip(emu, nv):
        norm = normalize(ntext, labels)
        forms.add(re.sub(r'#[0-9a-f]{8}', '#', re.sub(r'\d+', 'N', etext)))
        if int(eaddr, 16) != naddr or etext != norm:
            bad += 1
            if bad <= 20:
                print('MISMATCH %s\n  emulator: %s\n  nvdisasm: %s  (raw: %s)'
                      % (eaddr, etext, norm, ntext))
    print('%d instructions, %d distinct shapes, %d mismatches'
          % (len(emu), len(forms), bad))
    print('DISASM CHECK PASS' if bad == 0 else 'DISASM CHECK FAIL')
    return 0 if bad == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
