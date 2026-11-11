# Adreno 690 (SC8280XP / Q8B) firmware

These are the upstream linux-firmware blobs the Adreno 690 needs.  They are
installed into the Q8B rootfs as `/etc/firmware/adreno/` by `make basic`
(`system/Makefile`, target `firmware_install`).

| file | role | source |
|---|---|---|
| `a660_gmu.bin` | GMU (CM3) firmware v3.01.0B, 55252 bytes | `qcom/a660_gmu.bin` |
| `a660_sqe.fw` | CP/SQE microcode v1.12, 43292 bytes | `qcom/a660_sqe.fw` |
| `sc8280xp/LENOVO/21BX/qcdxkmsuc8280.mbn` | board-selected signed zap, 14392 bytes | same path under `qcom/` |
| `LICENSE.qcom` | redistribution licence | `LICENSES/LICENSE.qcom` |

## Why a660_* on an Adreno 690

The A690 is chip id `0x06090000`, family `ADRENO_6XX_GEN4`.  Mainline Linux
(`drivers/gpu/drm/msm/adreno/a6xx_catalog.c`, the a690 entry) uses:

```c
.chip_ids = ADRENO_CHIP_IDS(0x06090000),
.family   = ADRENO_6XX_GEN4,
.fw = {
        [ADRENO_FW_SQE] = "a660_sqe.fw",
        [ADRENO_FW_GMU] = "a660_gmu.bin",
},
.zapfw = "a690_zap.mdt",
```

There is **no** `a690_gmu.bin` in linux-firmware; an earlier revision of the
EwokOS port guessed that name and the GMU loader failed with `-2` (ENOENT)
before touching the hardware.  The loader accepts only the two catalog names
above and rejects files whose sizes do not match this pinned upstream release.

Pinned linux-firmware commit: `2aded3b604ae42d3f6b1c5a5873db021ec4e4207`
(`qcom: update gpu firmwares for qcm6490 chipset`, 2024-09-26).

```text
8acab7b417d9ebde89a1de9ae1e2c261d352fcab122e31ecd580cec9fe2ae5e7  a660_gmu.bin
d222f3fe290ef0516ee0ec43082596bad2df0fcbc2e0bbb26987623cef90cf76  a660_sqe.fw
```

## a660_gmu.bin layout (worth knowing before changing the parser)

The image starts with a 0xb0-byte table of 11 `(addr, 0, type, length)`
records.  Mainline walks the file as a chain of 16-byte `block_header`s and
`continue`s on `size == 0` (`NEXT_BLK` still advances by 16 bytes), so the
table is skipped one record at a time.  Only four real blocks follow:

```
addr=0x00000000 size=0x01dc   -> CM3 ITCM
addr=0x00004000 size=0xd4c8   -> GMU icache window (iova 0x4000)
addr=0x10007f98 size=0x0028   -> CM3 DTCM
addr=0x10007fdc size=0x0018   -> CM3 DTCM
```

The table itself is the a660 firmware's preallocation list (icache 0x4000 /
0x100000 / 0x200000 / 0x300000, DTCM 0x10004000 / 0x10007000, dummy
0x60000000 / 0x60001000).  It is covered by the `icache`, `dtcm` and `dummy`
allocations in `adreno_gmu.c`; do not "fix" the parser to treat every size==0
record as a payload.

## Updating

Q8B's board DT overrides the catalog's generic zap name with
`qcom/sc8280xp/LENOVO/21BX/qcdxkmsuc8280.mbn`. This is a monolithic signed
ELF32 MBN, not `a690_zap.mdt` plus `.bNN` segments. The current loader accepts
only that board-selected image (14392 bytes), with `/tmp/qcdxkmsuc8280.mbn`
as the temporary board-test location. Download it unchanged from
`linux-firmware/qcom/sc8280xp/LENOVO/21BX/qcdxkmsuc8280.mbn` and verify:

```text
d7b8cc648c2f020c0fb7770b2d844b40688e99fb541e45514533049611b34d5c  qcdxkmsuc8280.mbn
```

TrustZone authenticates its signature through PAS 13. The DT's `unfused`
comment permits reuse of the X13s image; it does not establish a zap-free
initialization path. This exact unmodified signed image is bundled and installed
under `/etc/firmware/adreno/sc8280xp/LENOVO/21BX/`; normal startup no longer
depends on the ephemeral `/tmp` copy.

On 2026-10-22 the unchanged board-selected MBN authenticated successfully
through PAS INIT_IMAGE, MEM_SETUP and AUTH_AND_RESET on Q8B. CP then exited
secure mode and passed its IB/fence memory-write selftest. The CCU-corrected
userspace driver subsequently passed pixel readback for fill/copy/180-degree
rotation and uniform downscale through 720x720 surfaces. See
`../../test/HANDOVER.md` for exact artifacts and remaining validation limits.
The same unmodified images also supported successful GLSL/NIR/ir3 compute
pixel readback, changing per-draw uniforms and A2D/compute transitions through
720x720 on that date. These are userspace rendering tests; no firmware edits
or additional kernel/PXE changes were required.

```sh
BASE=https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain
curl -o a660_gmu.bin "$BASE/qcom/a660_gmu.bin"
curl -o a660_sqe.fw  "$BASE/qcom/a660_sqe.fw"
curl -o LICENSE.qcom "$BASE/LICENSES/LICENSE.qcom"
```
