# Q8B Adreno user-space test loop

> **Current route (2026-10-22):** use only the verified official
> `a660_gmu.bin` v3.01.0B and `a660_sqe.fw` v1.12, and make the pre-release
> initialization match upstream Linux. Do not patch/disassemble firmware or
> resume the CM3 spin/NOP, forced-fault, broad-SID, host-cache, or forced-GX
> experiments recorded below. The old "missing AOP grant" conclusion was
> disproved: the correct cache handshake pairs are request/ack
> `0x4c05/0x4c0a` and `0x4c04/0x4c09`, and the AP obtains both acknowledgements.
>
> The active issue is SMMU context-bank placement. `GR0_ID1.NUMPAGENDXB`, not
> a hardcoded page 2, selects CB0; `NUMCB` is a direct count. The driver now
> validates the resulting CB0/CB1 offsets against the `0x20000` DT resource
> before writing either bank. The coherent walker TCR expected from Linux is
> `0x80803519`, not the historical `0x80802019` shown below.
>
> Everything after this notice is retained only as a chronological record and
> must not be treated as the current diagnosis or test plan. See `HANDOVER.md`.

Board: `root@192.168.8.50` (password `root`, direct link on `en6`).
Serial: MCP `eud_serial_log` on `/dev/cu.usbserial-0035E5970`
(host-side timestamped cache, `eud/logs/serial-cache.log`).

## Why it is shaped this way

- The board's sshd execs commands **without a shell** (`/bin/sh` absent), so
  `export`, `;`, `&`, `$VAR` and redirection do not work over ssh.  ssh is
  therefore used **only to copy files**.
- Runtime switches are file-based: `/tmp/adreno.cfg` with `NAME=1` lines is
  read by `adreno_flag()` (libadreno).  `/tmp` is ramfs, so it is re-copied
  after every reboot.
- The test is started on the **serial console**, so the MCP records the whole
  run from the first `adreno_*` line to the reset/hang.
- Bulk `scp` wedges the TC956x RX ring; `board_scp.sh` defaults to `-l 200`
  (Kbit/s).  If the network dies anyway, recover from the console with:
  `kill <netd>`, `kill <net>`, `ipcserv /drivers/q8b/net /dev/eth0`,
  `ipcserv /drivers/netd /dev/net0 /dev/eth0`, `bgrun /sbin/sshd`.

## Loop

1. `make -C machines/q8b/system gui`, note the g2dd md5.
2. Write `/tmp/adreno.cfg`, then:
   `board_scp.sh <g2dd> root@192.168.8.50:/tmp/g2dd`
   `board_scp.sh /tmp/adreno.cfg root@192.168.8.50:/tmp/adreno.cfg`
   verify `/bin/md5 /tmp/g2dd` matches.
3. Serial console (115200 8N1): `\r`, `root`, `root`, then
   `/bin/ipcserv /tmp/g2dd /dev/g2d`.
4. `eud_serial_log read since=<T0>` and analyse.

`serial_console.py` sends raw bytes to the console (the MCP keeps reading the
same tty, so nothing is lost from the cache).

## Switches (libadreno)

| flag | effect |
|---|---|
| `ADRENO_ENABLE` | opt in to the bring-up (also `getenv`) |
| `ADRENO_SKIP_CM3` / `_PDC` / `_RPMH` | isolate the GMU stages |
| `ADRENO_GMU_SID_FAULT` | force GMU S2CR to FAULT (no DRAM access) |
| `ADRENO_GX_ON` | de-assert GX GDSC before the CM3 release |
| `ADRENO_GMU_SRC_GPLL0` | clock `gmu_clk_src` from GPLL0/3 instead of GPLL0_DIV |
| `ADRENO_ARM_WDT` | arm APSS WDT (0x17c10000) around the release |
| `ADRENO_GFX_CORNER` | override the RPMh corner |

## Status 2026-10-22 (Linux diff round 3) - decisive

### The fatal is the SYSRESET=0 store/handshake, not the SMMU

`ADRENO_MARK=1` writes one file per breadcrumb to the SD rootfs
(`/m_enter`, `/m_fence`, `/m_cp_resp`, `/m_released`, `/m_polled`), which
survives the reset.  After a run that wedges the board and a MCP reboot:

```
/m_enter     1k
/m_fence     1k
/m_cp_resp   0k
/m_released  absent
```

`m_cp_resp` is written just before the `cm3 [3] release` klog (which did
print); `m_released` is the call immediately after `CM3_SYSRESET = 0`.
So the CPU never gets past that one store: the GMU release handshake
fatals the SoC (no serial, no net, EDL afterwards) within microseconds.

### The SMMU side is fully correct

`CB1 walk iova4000 l1=..03 l2=..03 l3=..03 pa=<icache.pa>` plus a register
readback after programming:

```
sctlr=101 tcr=80802019 ttbr0=<gmu_pgtable|ASID> s2cr2=1
cbar=1f000 cba2r=1 scr0=210400 gfsr=0
```

SCTLR.M=1, TCR=EAE|EPD1|SH_OS|T0SZ=25, CBAR=S1_TRANS_S2_BYPASS|WB,
S2CR(2)=TRANS/CB1, no global fault.  So the first fetch would translate.

### Other probes

- `ADRENO_CM3_NORELEASE=1` (skip the 0 write, leave SYSRESET=1): the board
  survives and the FW_INIT poll times out (result=0) - so the reset
  polarity is normal and the fatal is specific to releasing the reset.
- `ADRENO_CM3_SPIN=1` (Thumb `b .` over the icache entry 0x4000..0x400e)
  still fatals with the corrected SMMU, i.e. the CM3 never reaches the
  icache entry - it dies in the ITCM stub or the release handshake itself.

### Conclusion

Mainline's complete sequence (clocks, GDSC, RPMh/PDC, RSCC cold-boot skip,
fw load, boot config, HFI table, IRQ masks, NC SMMU walk/attrs, verified
CB1, interconnect keepalive) is now implemented and the SMMU is provably
ready, yet deasserting CM3_SYSRESET fatals the SoC.  This is a
board/firmware-specific GMU power/clock handshake that mainline does not
perform (mainline assumes the GMU power domain is already up from the
bootloader/RSCC state).  Next: compare with the Q8B vendor KGSL
`gmu_start`/`gmu_power` sequence (deepin 25 tree).

## Round 4 additions

- **Full cx_gdsc power-on**: Linux's `gdsc_enable()` (cx_gdsc has
  `VOTABLE | RETAIN_FF_ENABLE`) does more than clear SW_COLLAPSE - it sets
  RETAIN_MEM (bit14), RETAIN_PERIPH (bit13) and RETAIN_FF (bit11).  We used
  to skip everything when PWR_ON was already set, leaving GDSCR=0xf8222000.
  Now the GDSCR reads 0xf8226800 (all retain bits set) - still fatal at the
  release, but the fix is correct and kept.
- **EUD SWD AHB-AP attempt**: `eud_usb.py swd-on` + `swd-dpidr` works
  (DPIDR=0x5ba02477), but `swd-power` reports CDBGPWRUPACK=1,
  **CSYSPWRUPACK=0** and every AP IDR reads 0, so the APSS MEM-AP cannot be
  used to peek at GMU registers after a hang.  This needs the full
  OpenOCD/EUD secure-debug bring-up (Linux host only) or a debug-enabled
  boot.
- Marker run repeated with the GDSCR fix: /m_enter, /m_fence, /m_cp_resp
  exist, /m_released still absent - the fatal is unchanged.

## Round 5

- **Clock encoding verified**: `kbase/clk-rcg` uses `D = 2*div - 1` (hid), so
  gmu `0x602` (div 1.5) and hub `0x505` (div 3) both really are 200 MHz - the
  clocks were never wrong.
- **rpmh_stop removed from rpmh_init**: our init used to call
  `rpmh_stop()` (park the GPU RSC) right before the first CM3 boot, while
  Linux never stops at probe (PDC_SLEEP only comes from suspend/failure).
  Now skipped by default (`ADRENO_RPMH_STOP_INIT=1` restores it).  Result:
  with the RSC left awake the forced `ADRENO_RSCC_WAKE=1` request never ACKs
  (nothing to wake) and the driver declines cleanly; the default path still
  fatals at `CM3_SYSRESET=0`.
- **Rootfs-based test flow**: the 200 KB `scp` occasionally wedges the board
  (one run produced a kernel `ec0 far=0x280f8 pc=0x11060` exception storm on
  the scp/sshd pid).  Now the g2dd is copied once to
  `/drivers/q8b/g2dd` (persists across reboots) and tests run that copy;
  only the ~40 byte `/tmp/adreno.cfg` is re-sent per boot.
- PDC/RSCC init already matches mainline's `pdc_in_aop` path for the a650
  family (RSCC registers + PDC enable only, no SEQ_MEM/TCS writes).

## Round 6 - interconnect ordering fix (real)

Linux's resume order is `clk_bulk_prepare_enable` -> `a6xx_gmu_set_initial_bw`
(interconnect) -> `fw_start`.  We used to vote the BCM keepalive/full
bandwidth BEFORE enabling the GPUCC/GCC clocks, which left the
vote-controlled NoC branches halted:

```
before fix: memnoc=80004221 dvm=80000001 tcu=8000000b   (halt bits set)
after  fix: memnoc=0        dvm=1        tcu=180411      (all clear)
```

(`ADRENO_BCM_FULL=1` sends vote_x=vote_y=0x3fff instead of the 1/1
keepalive, and the vote now runs at the top of `gmu_fw_start`, i.e. after
`adreno_gpucc_init()`.)  This is kept: the GPU NoC port is now genuinely up.

Even with the NoC fully on, `CM3_SYSRESET=0` still fatals the SoC, and the
`ADRENO_CM3_SPIN=1` (`b .` at the icache entry) run still fatals, so the CM3
never reaches the icache entry - the fatal is in the release handshake, not
in the memory path.

Board note: a fatal run resets into EDL and sometimes SBL1 prints
`ERROR: Ramdump TableInit` in a loop; re-sending the MCP `eud_board_reboot`
1-2 times recovers it (no manual power cycle needed so far).

## Round 7 - pre-release state is provably identical to mainline

With `ADRENO_CM3_NORELEASE=1` (safe, board survives) the full pre-release
register dump reads:

```
sysreset=1 bootcfg=2 cm3cfg=4052 gen7=1 qtbl=1/1004000
sfr=6090000 cpmsg=1000003 init=0 busy=0
after icc vote memnoc=0 dvm=0 tcu=180411
CB1 readback sctlr=101 tcr=80802019 s2cr2=1 cbar=1f000 gfsr=0
walk iova4000 -> pa == gmu_icache.pa
```

Every value matches `a6xx_gmu_fw_start()` / `a6xx_gmu_start()` exactly, the
NoC clocks are on, the SMMU bank is enabled and fault-free.  The one store
`CM3_SYSRESET = 0` still fatals the SoC, and the `b .` icache-entry spin
still fatals, so the CM3 never executes a single instruction: the fatal is
in the GMU's reset-release handshake, not in anything the OS programs.

Remaining hypothesis: a hardware-level GMU power/clock prerequisite that
mainline gets implicitly from the bootloader/AOP/RSCC state (or from a
vendor-only sequence), not from any register mainline writes.

## Round 8

- **ADRENO_GFX_CORNER was a no-op**: the RPMh code read it with getenv(), but the
  board has no shell and only /tmp/adreno.cfg exists.  Added
  `adreno_cfg_int()` (getenv or "name=<n>" in /tmp/adreno.cfg) and switched the
  corner override to it.  Verified: the log now shows corner=4.
- **Corner 4 vs 8** at the release: both fatal identically, so the rail
  voltage/corner is not the trigger.
- **Full PDC TCS1/TCS3 programming** (`ADRENO_PDC_FULL=1`, the pre-a650 path
  with 0x10108/0x30010/0x30000/0x30080 and data 1/2/3, since sc8280xp does
  not expose the separate gmu_pdc_seq region): still fatal at [3].
- Marker evidence re-checked after the PDC_FULL run: /m_enter, /m_fence,
  /m_cp_resp present, /m_released absent - the fatal point is unchanged.

Current build: ceedbd7a (icc vote after clocks, GDSC retention, PDC full
option, cfg numeric overrides).

## Round 9

- **Bootloader GMU state** (read before we write anything):
  `pre-init g7=0 init=0 sysreset=1 cm3cfg=4052 busy=0 qtbl=0/0`.
  The UEFI/ABL leaves the CM3 held in reset with CM3_CFG already 0x4052 and
  no firmware running - exactly the cold-boot state mainline assumes.
- **OOB (out-of-band) BOOT_SLUMBER**: mainline uses
  `HOST2GMU_INTR_SET` bit22 / `GMU2HOST_INTR_INFO` bit30.  For non-legacy
  GPUs it is only used after the firmware is running (suspend / DCVS /
  GPU_SET); `a6xx_gmu_gfx_rail_on()` (the pre-start OOB) is legacy-only.
  The A690 is non-legacy, so the OOB handshake is not the missing pre-start
  step either.
- `ADRENO_RSCC_WAKE_IGNORE=1` (issue the RSC wake, ignore the missing ACK,
  let it settle during fw-load, then release): still fatal at [3].

Still: the single `CM3_SYSRESET=0` store hangs the SoC with every
mainline-visible register in the exact expected state.

## Round 10 - fresh boot is mandatory

A second g2dd started on the same boot reads the previous run's GMU state
(`pre-init g7=1 qtbl=1/1004000`, a leftover from the earlier NORELEASE run)
and wedges the board.  Every test must therefore start from a fresh boot -
the fatal tests reset the board themselves, and the safe (`ADRENO_CM3_NORELEASE`)
runs leave a live g2dd behind, so reboot via MCP before the next run.

End-to-end verification on a fresh boot (safe NORELEASE path):

```
pre-init g7=0 init=0 sysreset=1 cm3cfg=4052 busy=0 qtbl=0/0
after icc vote memnoc=0 dvm=0 tcu=180411
pre-rel sysreset=1 bootcfg=2 cm3cfg=4052 gen7=1 qtbl=1/1004000 sfr=6090000 cpmsg=1000003 init=0 busy=0
ADRENO_CM3_NORELEASE: SYSRESET left at 1
GMU firmware init timed out (result=0)
q8b g3d: adreno bring-up DECLINED (rc=-1)
[OK]   (board still reachable)
```

So the loop (ssh copy -> serial console start -> MCP timestamped capture ->
analysis -> recovery) is verified end to end; the only unresolved item is
the hardware-level `CM3_SYSRESET=0` handshake.

## Round 11

- `ADRENO_GX_CLAMP=1` (gdsc_deassert_clamp_io on 0x1508 bit0, together with
  ADRENO_GX_ON): gx_gdscr powered on (0x222001 -> 0xf8222000) and the clamp
  was already 0, release still fatal.
- Deployment note: 200 KB scp at -l 200 occasionally wedges the board's
  network/kernel (sshd exception storm); -l 50 (50 Kbit/s, ~35 s) has been
  reliable.  Keep using the slow copy for g2dd.

## Round 12 - fatal is a vendor power handshake in the firmware ITCM stub

Disassembling the ITCM reset handler (the reset vector at 0x04 points to
0x104) showed the real sequence: DTCM boot counter -> MSP/PSP -> VTOR=0x1000
-> `bl 0x84` -> main (0x10) -> icache (0x4008).  `bl 0x84` is:

```
str 0 -> 0x1807d004 ; str 0 -> 0x1807d000 ; dsb ; isb
str 3 -> 0x1807d014 ; poll bit0 of 0x1807d028 ; str 0 -> 0x1807d014
str 3 -> 0x1807d010 ; poll bit0 of 0x1807d024 ; str 0 -> 0x1807d010
```

i.e. two vendor-private power handshakes (request=3, ack=bit0).  Results:

- `ADRENO_CM3_SPIN_ITCM=1` (patch the ITCM reset handler to `b .`): the
  `CM3_SYSRESET=0` store **completes**, the board survives, FW_INIT stays 0.
  So the release itself is fine and the CM3 does execute.
- `ADRENO_CM3_NOP_STUB=1` (NOP the `bl 0x84` call): release completes, board
  survives, but no FW_INIT (the handshake is required for the firmware init).
- `ADRENO_CM3_NOP_ST0=1` (NOP only the first store at 0x96): same - release
  completes, no FW_INIT.  The CM3 then spins in the handshake poll loops.
- With the real stub, the CM3 hammers that block forever and the host's
  SYSRESET store (same/bridged block) never completes -> SoC hang -> EDL.
- The host reads of its own GMU words 0x1f405/0x1f40a/0x1f404/0x1f409 are
  all 0 after the release, so the CM3's 0x1807d000 window is NOT the host's
  GMU register block - it is a separate AO/RSC/power aperture.
- CB1 walks for all five GMU BOs (icache/dummy/debug/log/hfi) match their
  BO PAs exactly, so the DRAM mappings are not the issue.
- GX pre-power (`ADRENO_GX_ON`), full PDC TCS (`ADRENO_PDC_FULL`), full BCM
  (`ADRENO_BCM_FULL`) are all set and still the ack never arrives.

Conclusion: the GMU firmware's own power-up handshake (a vendor-only
sequence mainline never issues) needs a hardware/AOP/PMIC grant that is
missing on this Q8B boot; every software-visible register mainline touches
is already identical.

## Automation updates

- `deploy_retry.sh` (reboot -> scp -l 50 -> md5 compare, up to 5 attempts)
  deploys reliably; a reboot before every test is mandatory (a live g2dd
  from the previous safe run reads stale GMU state and wedges the board).
- If scp ever wedges the board, MCP reboot recovers it; the 200 KB copy is
  the fragile part (16 KB chunks + `concat` and the serial `/bin/rx` XMODEM
  receiver were both investigated as fallbacks).

## Round 13 - GX power is GMU/AOP controlled

- `ADRENO_GX_CLKS=1` enables gpu_cc_gx_gmu_clk (0x1064, the GMU's GX-side AHB
  clock): `80000000 -> 80000001`, i.e. the enable bit sets but the
  `BRANCH_HALT` bit31 never clears.  The GX domain is not actually powered
  even with gx_gdscr SW_COLLAPSE cleared (0x222001 -> 0xf8222000); the host
  cannot power the GX, the GMU firmware must (gdsc_gx_do_nothing_enable).
- The handshake registers still read req/ack = 0 from the host because
  0x1f405 etc. lies outside the mapped GMU region (0x3e01814 > 0x3d9e000),
  so the CM3's 0x1807d000 window is a separate AO/RSC/power aperture.
- The `gfx` rpmhpd domain is not active_only (no flag in rpmhpd.c), so the
  apps RSC drv-2 TCS path used for gfx.lvl/cx.lvl is a valid vote path.

Everything reaches the same conclusion: the firmware's own power-up
handshake needs a hardware/AOP/PMIC grant (GX/GFX rail power) that is not
granted on this Q8B boot; no software-visible register mainline programs
differs any more.

## Round 14

- `GMU_RPMH_CTRL` (0x1f8e8) readback after power_config is **0xf11**, i.e.
  INTERFACE + LLC + DDR + MX + CX + GFX vote enables are all set, so the
  GMU's RPMh client (including the GFX vote) is enabled correctly.  (The
  handshake ack is not this register: RPMH_POWER_STATE lives at 0x1f8ec,
  not at the 0x1f409/0x1f40a the stub polls.)
- gpu_cc_gx_gmu_clk (0x1064) halt never clears with the GX GDSC SW_COLLAPSE
  cleared, so the GX is not really powered by the host; only the GMU
  firmware can (gdsc_gx_do_nothing_enable semantics).

The firmware's private handshake remains the only blocker and needs the
AOP/PMIC GX/GFX rail grant.  Everything the mainline driver programs has
been read back and matches.
