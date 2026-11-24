#!/usr/bin/env python3
# roundtrip_nir.py - verify the NIR->SASS compiler output (probe_nir.bin from
# test_nir_sass) is valid sm_87 SASS by disassembling with NVIDIA nvdisasm.
# Checks the expected instruction sequence for the tid->i2f->fadd->f2i->sts
# kernel appears, and that nvdisasm reports no illegal instruction.
import struct, subprocess, sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
NV = "$HOME/cuda-nvcc-full/nvidia/cu13/bin/nvdisasm"
ENV = "LD_LIBRARY_PATH=$HOME/amd64-libs/usr/lib/x86_64-linux-gnu"

def main():
    probe = open(os.path.join(HERE, "probe_nir.bin"), "rb").read()
    elf = bytearray(open("/tmp/nvcc-golden/vadd.cubin", "rb").read())
    nop = struct.pack("<IIII", 0x00007918, 0, 0, 0x000fc000)
    body = probe + nop * ((0x300 - len(probe)) // 16)
    elf[0x700:0x700 + 0x300] = body[:0x300]
    open("/tmp/nvcc-golden/probe_nir.cubin", "wb").write(elf)

    cmd = f"orb -m debian bash -c '{ENV} {NV} -c /mnt/mac/tmp/nvcc-golden/probe_nir.cubin'"
    res = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    dis = res.stdout
    if "Illegal instruction" in res.stderr or "Unrecognized operation" in res.stderr:
        print("FAIL: nvdisasm rejected the NIR-generated SASS:\n", res.stderr)
        return 1
    # the kernel must contain these ops in order
    want = ["S2R", "I2F", "FADD", "F2I", "STS", "EXIT"]
    pos = 0
    for op in want:
        idx = dis.find(op, pos)
        if idx < 0:
            print(f"FAIL: expected {op} not found in disassembly")
            return 1
        pos = idx + len(op)
    print(f"PASS: NIR->SASS round-trip valid ({len(probe)//16} instrs; "
          f"{' -> '.join(want)})")
    return 0

if __name__ == "__main__":
    sys.exit(main())