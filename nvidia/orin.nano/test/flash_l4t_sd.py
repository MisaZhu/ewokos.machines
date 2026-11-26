#!/usr/bin/env python3
"""Stream-flash the JP6.2.1 L4T sd-blob.img from its zip onto an SD card.

Runs INSIDE the OrbStack debian VM as root (macOS TCC blocks host dd).
The zip is read from the Mac via /mnt/mac; no 24GB intermediate file is
needed (host/VM disks have <12GB free).  Streams the zip entry straight
to the block device in 4MB chunks.

Usage:
  python3 flash_l4t_sd.py /dev/sdX          # flash (asks confirmation)
  python3 flash_l4t_sd.py --list            # list removable candidates

SAFETY: refuses non-removable devices and anything <28GB.  The target
must be the SD card destined for the Orin, NOT the host system disk.
"""
import os, sys, zipfile, time

ZIP = "/mnt/mac/Users/tony/work/jetson/jetson-orin-nano-devkit-super-SD-image_JP6.2.1.zip"
ENTRY = "sd-blob.img"
CHUNK = 4 * 1024 * 1024

def removable_disks():
    out = []
    for name in sorted(os.listdir("/sys/block")):
        if name.startswith(("loop", "nbd", "ram", "zram")):
            continue
        base = f"/sys/block/{name}"
        try:
            rm = open(f"{base}/removable").read().strip()
            size = int(open(f"{base}/size").read().strip()) * 512
            model = open(f"{base}/device/model").read().strip() if os.path.exists(f"{base}/device/model") else "?"
        except OSError:
            continue
        out.append((name, rm, size, model))
    return out

def main():
    if "--list" in sys.argv:
        for name, rm, size, model in removable_disks():
            print(f"/dev/{name}  removable={rm}  {size/1e9:.1f}GB  {model}")
        return
    if len(sys.argv) != 2:
        print(__doc__); sys.exit(1)
    dev = sys.argv[1]
    name = os.path.basename(dev)
    cands = {n: (rm, sz, md) for n, rm, sz, md in removable_disks()}
    if name not in cands:
        print(f"ERROR: {dev} not a known block device"); sys.exit(1)
    rm, sz, md = cands[name]
    if rm != "1":
        print(f"ERROR: {dev} is NOT removable (model={md}); refusing"); sys.exit(1)
    if sz < 28e9:
        print(f"ERROR: {dev} only {sz/1e9:.1f}GB (<28GB); image needs 24GB"); sys.exit(1)
    print(f"Target: {dev}  removable={rm}  {sz/1e9:.1f}GB  model={md}")
    print(f"Source: {ZIP} :: {ENTRY}")
    ans = input("Type 'yes' to OVERWRITE this device: ")
    if ans.strip() != "yes":
        print("aborted"); sys.exit(1)
    zf = zipfile.ZipFile(ZIP)
    info = zf.getinfo(ENTRY)
    total = info.file_size
    print(f"Streaming {total/1e9:.2f}GB ...")
    t0 = time.time()
    written = 0
    with zf.open(ENTRY) as src, open(dev, "wb", buffering=0) as dst:
        while True:
            buf = src.read(CHUNK)
            if not buf:
                break
            dst.write(buf)
            written += len(buf)
            if written % (256 * 1024 * 1024) < CHUNK:
                el = time.time() - t0
                print(f"  {written/1e9:.2f}/{total/1e9:.2f}GB  "
                      f"{written/1e6/el:.1f}MB/s", flush=True)
        dst.flush()
        os.fsync(dst.fileno())
    el = time.time() - t0
    print(f"DONE {written/1e9:.2f}GB in {el:.0f}s ({written/1e6/el:.1f}MB/s)")
    os.system("sync")
    print("Verify: re-read first 512B should show MBR/GPT, run --list to confirm.")

if __name__ == "__main__":
    main()