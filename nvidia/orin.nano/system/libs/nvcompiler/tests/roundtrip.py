#!/usr/bin/env python3
# roundtrip.py - functional verification of nv_sass via NVIDIA nvdisasm.
# Splices the probe bytes into a valid sm_87 cubin container (the golden vadd
# ELF), disassembles with nvdisasm in the OrbStack VM, and checks each probe
# instruction decodes to the intended mnemonic.  This is the authoritative
# correctness check: NVIDIA's own disassembler validates the encoding.
import struct, subprocess, sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
GOLDEN = "/tmp/nvcc-golden/vadd.cubin"
TEXT_OFF = 0x700
TEXT_SIZE = 0x300
NV = "$HOME/cuda-nvcc-full/nvidia/cu13/bin/nvdisasm"
ENV = "LD_LIBRARY_PATH=$HOME/amd64-libs/usr/lib/x86_64-linux-gnu"

# expected mnemonic per probe instruction (order matches emit_probe.c)
EXPECT = [
    "S2R", "S2R", "IMAD", "MOV", "MOV", "MOV", "UMOV",
    "FADD", "FMUL", "FFMA", "FADD", "FMNMX",
    "IADD3", "IMAD", "IMAD.WIDE", "IMNMX", "LOP3", "SHF", "SEL",
    "ISETP", "ISETP", "FSETP",
    "F2F", "F2I", "I2F", "MUFU",
    "LDG", "STG", "LDS", "STS", "LDC", "ULDC",
    "CS2R", "R2UR",
    "BAR", "MEMBAR", "BMOV", "BSYNC", "WARPSYNC", "BRA", "EXIT", "NOP",
]

def main():
    probe = open(os.path.join(HERE, "probe.bin"), "rb").read()
    n = len(probe) // 16
    assert n == len(EXPECT), f"probe {n} instrs vs {len(EXPECT)} expected"
    elf = bytearray(open(GOLDEN, "rb").read())
    # NOP sled (sm_87 NOP = 0x...7918 low word, control 0x000fc000...)
    nop = struct.pack("<IIII", 0x00007918, 0, 0, 0x000fc000)
    body = probe + nop * ((TEXT_SIZE - len(probe)) // 16)
    elf[TEXT_OFF:TEXT_OFF + TEXT_SIZE] = body[:TEXT_SIZE]
    out = "/tmp/nvcc-golden/probe.cubin"
    open(out, "wb").write(elf)

    cmd = f"orb -m debian bash -c '{ENV} {NV} -c /mnt/mac/tmp/nvcc-golden/probe.cubin'"
    res = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    dis = res.stdout
    if not dis:
        print("nvdisasm produced no output:\n", res.stderr)
        return 2

    # collect /*offset*/ lines within .text.vadd
    lines = {}
    for ln in dis.splitlines():
        ln = ln.strip()
        if ln.startswith("/*") and "*/" in ln:
            off = int(ln[2:ln.index("*/")], 16)
            body = ln[ln.index("*/") + 2:].strip()
            lines[off] = body

    npass = nfail = 0
    for i, exp in enumerate(EXPECT):
        off = i * 16
        got = lines.get(off, "<missing>")
        # mnemonic = first token, strip leading @P guard
        toks = got.replace(";", "").split()
        if toks and toks[0].startswith("@"):
            toks = toks[1:]
        mn = toks[0] if toks else ""
        ok = mn == exp or mn.startswith(exp + ".") or mn == exp.split(".")[0]
        # IMAD.WIDE must keep .WIDE; plain IMAD must not be .WIDE
        if exp == "IMAD":
            ok = mn == "IMAD"
        if exp == "IMAD.WIDE":
            ok = mn.startswith("IMAD.WIDE")
        if ok:
            npass += 1
            print(f"  PASS {off:04x} {got}")
        else:
            nfail += 1
            print(f"  FAIL {off:04x} want {exp:10s} got: {got}")
    print(f"\n{npass} passed, {nfail} failed (nvdisasm round-trip)")
    return 1 if nfail else 0

if __name__ == "__main__":
    sys.exit(main())