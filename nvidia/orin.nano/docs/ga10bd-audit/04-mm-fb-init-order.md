# ga10bd 审核调研原始记录：Verify MM/FB/init order vs nvgpu

> 来源：本次审核中后台 subagent（general-purpose）的最终输出，**原文照录，未经改写**。
> 启动 2026-11-22 23:28:57，完成 2026-11-22 23:39:02（本地时间）。
> 性质：subagent 对照 nvgpu 源码的阅读结论，属于**待核实线索**，不是已验证事实；
> 各条的采纳/否决情况见同目录 README.md。

## 下发的调研任务（原文）

````text
RESEARCH ONLY — do not edit, create or delete any file. You are auditing a hand-written bare-metal GPU driver against its reference implementation and reporting discrepancies with evidence.

## Context
I am reviewing `ga10bd`, a standalone bring-up driver for the NVIDIA GA10B GPU (Tegra234 / Jetson Orin Nano) written for a hobby OS. It has never run on hardware; it was transcribed from NVIDIA's open-source L4T `nvgpu` driver (rel-36). My job is to find places where the transcription is WRONG before it touches a board (a wrong or out-of-order register access on this board can raise a fatal bus error). The part you are checking is early power-on init, the GPU MMU page tables, and the FB (framebuffer/memory controller block) setup.

- Driver under review: `/Users/tony/work/ewokos/machines/nvidia/orin.nano/system/drivers/ga10bd/` — read `ga10b_power.c`, `ga10b_mm.c`, `ga10b_mm.h`, `ga10bd.c` (the `stage_init`, `stage_acr`, `cmd_ce`, `cmd_gr` flows), `ga10b_regs.h`, and `ga10b_ramin_pdb_lo()` in `ga10b_fifo.h`.
- Reference (ground truth): `/tmp/gpu-research/linux-nvgpu/drivers/gpu/nvgpu/` (branch patches-rel-36). Relevant: `common/init/nvgpu_init.c`, `common/mm/*` (esp. `mm.c`, `gmmu/page_table.c`), `hal/mm/gmmu/*`, `hal/mm/mm_*`, `hal/fb/*`, `hal/ltc/*`, `hal/priv_ring/*`, `hal/bus/*`, `hal/mc/*`, `hal/fuse/*`, `hal/top/*`, `hal/power_features/cg/*`, `hal/fifo/ramin_*`, `os/linux/platform_ga10b_tegra.c` (and the generic tegra platform file it builds on), and headers `include/nvgpu/hw/ga10b/hw_{gmmu,fb,mc,fuse,pri_ringmaster,pri_ringstation*,bus,func,ram,ltc,top}_ga10b.h`. The ga10b HAL reuses older-chip functions — ALWAYS check `hal/init/hal_ga10b.c` to see which function is actually wired before quoting one.

Only trust the reference source. Do not rely on memory of nvgpu or on comments inside the driver (those comments are what is being audited).

## Questions — for each give MATCH / MISMATCH / CANNOT-DETERMINE, reference `path:line` + short quoted snippet, and for mismatches say exactly what the driver should do instead.

M1. Power-on init ORDER. From the finalize_poweron step table in `nvgpu_init.c`, give the ordered list of steps (function names) from the start up to and including the ACR bootstrap step and the first channel-capable state (mm, fifo, ce). Then compare with the driver's order: BPMP power/clocks/reset → map → boot_0 detect → fuse read → priv-ring init → bus init → clock-gating prods ("fifo_engine_reset") → mask interrupts → ACR (load fw, read WPR, boot GSP) → GMMU init → FIFO init → CE init. Which reference steps before ACR does the driver skip entirely, and which of those look functionally required (as opposed to interrupt/ECC/clock-gating housekeeping)? Note specifically: the driver defines `ga10b_fb_ltc_early_init()` and `ga10b_fb_init_hw()` in ga10b_power.c but NOTHING CALLS THEM — where in the reference order do the FB init_hw / fb init_fs_state / ltc init / mm setup_hw steps sit, and are they before or after ACR?

M2. FB init. Quote the wired fb init_hw (and anything it chains to) for ga10b and what mm setup_hw does (e.g. MMU debug buffers, sysmem flush page, bar/instance bindings). Compare with `ga10b_fb_init_hw()`: NISO flush sysmem addr register 0x100c10 = phys>>8; MMU debug write 0x100cc8 and read 0x100ccc = aperture 3 | ((phys>>12)&0xfffffff)<<4. Check offsets, shifts, aperture encodings, and whether both wr and rd point at the SAME page in the reference. Is anything else written (fb_mmu_ctrl page size / vm_pg_size, etc.)?

M3. Page-table format. Using `hw_gmmu_ga10b.h` and the wired ga10b gmmu functions, verify the driver's encoders in `ga10b_mm.h` bit by bit: PDE (aperture value for sys-noncoherent, address field shift/mask/position, vol bit), dual PDE0 small-page half (which of the 4 words, aperture, address shift, vol), PTE (valid, privilege, read-only, vol, aperture value, address field, the high word and kind field). Also verify the level table: number of levels, VA bit ranges per level, entry size per level, and that the driver's index math in `ga10b_gmmu_map()` (lo_bit {47,38,29,21,12}, entries {4,512,512,256,512}, entry words {2,2,2,4,2}) is right. Does ga10b's PTE differ from older chips (e.g. wider address field, 64-bit single-word layout)?

M4. Instance-block page-directory word. Verify `ga10b_ramin_pdb_lo()` = 0x3 | 0x4 | 0x400 | 0x800 | ((pdb>>12)&0xfffff)<<12 and hi = pdb>>32 against the wired ramin init_pdb and the `ram_in_page_dir_base_*` macros: target aperture value for non-coherent sysmem, vol bit, fault-replay bits, ver2 format bit, big-page-size bit. NOTE the driver only ever creates 4KB small-page mappings — is setting the 64KB big-page bit consistent with that, i.e. does big_page_size only select what the big half of a dual PDE means?

M5. TLB invalidate. Compare driver (end of `ga10b_gmmu_map()`: write 0x100cb8 = 0x2 | ((pdb>>12)&0xfffffff)<<4; write 0x100cbc = all_va 0x1 | all_pdb 0x2 | trigger 0x80000000; no polling) with the wired fb tlb_invalidate: register offsets, field values, the pre-wait on the MMU PRI FIFO and the post-wait on the trigger bit.

M6. Priv ring + bus + interrupt tree. Compare `ga10b_priv_ring_init()` (writes 9 SLCG registers to 0, writes ringmaster command 0x4 at 0x12004c, polls command==0, checks start_results bit0) with the wired priv_ring enable_hw / enum functions; `ga10b_bus_init()` (0x1100 W1C and 0x1140 = 0x9C0001FC) with the wired bus init_hw; and `ga10b_mc_intr_enable()` (one write of 0xffffffff to FUNC base 0xB80000 + 0x1610) with the wired MC interrupt mask/enable functions — does one TOP_EN_CLR register cover the whole tree?

M7. Engine reset. Compare `ga10b_engine_reset(reset_id)` — clears bit (reset_id%32) in register 0x600+4*(reset_id/32), polls for 0, sets it, polls for 1 — with the wired mc enable_dev / reset functions: register, polarity (is 1 = enabled/out-of-reset?), and delays.

M8. Chip detect and fuses. Check boot_0 decoding (driver: chip id = (boot_0 >> 20) & 0xfff, expecting 0x17B) and the fuse offsets/logic in `ga10b_fuse_read()` (priv_sec_en 0x820434, src isolation 0x820410, wpr_enabled 0x8205ec bit0, vpr auto-fetch disable 0x820684 bit0) against the wired fuse check function.

M9. Clocks/reset at the platform level. From the Linux platform file(s), which clocks does the driver enable/set-rate before touching the GPU, in what order relative to reset de-assert and power-gate, and are there required delays? Driver: power-gate on → enable 4 clocks (ids named GPUSYS, GPC0, GPC1, FUSE) → reset assert then de-assert → 1 ms delay; it never sets a clock rate.

## Output
Start with a short ranked summary of the most severe mismatches, then M1..M9. Terse, evidence-first, under 1300 words. Say CANNOT-DETERMINE rather than guess.
````

## subagent 返回结果（原文）

Paths: `R` = `/tmp/gpu-research/linux-nvgpu/drivers/gpu/nvgpu`, `D` = `/Users/tony/work/ewokos/machines/nvidia/orin.nano/system/drivers/ga10bd`. No files were modified.

## Ranked summary

1. **Wrong registers: absolute BAR0 0x50 / 0x54.** `ga10b_fifo_engine_reset()` writes them as absolute offsets; in the reference they are relative to each runlist's `runlist_pri_base`.
2. **ACR runs before mm/fifo/FB init.** The reference does FB fs_state, LTC init, `mm.init_mm_support` (which calls `fb.init_hw`), `fifo_init_support` and `gr_enable_hw` before `acr_construct_execute`. The driver's two FB functions are never called.
3. **TLB invalidate.** The driver sets an extra `all_pdb` bit and has neither the pre-wait nor the post-wait on `fb_mmu_ctrl`.
4. **ELPG write to 0x20c.** The reference writes it only when not on silicon.
5. **FB init.** wr and rd debug buffers share one page; BAR binds, `fb_flush` x2, FB interrupt setup and mmu_fault setup are missing.
6. **Clocks.** The reference sets all four clocks to `UINT_MAX` rate and never toggles the reset.
7. Minor: chip-ID mask 0xFFF vs an effective 0x1FF; interrupt mask issued after priv ring/bus instead of before.

M3, M4 and M7 match.

## M1 order — MISMATCH

| Reference (`R/common/init/nvgpu_init.c`) | Driver |
|---|---|
| `nvgpu_detect_chip` :874; fuse check at HAL init (`R/hal/init/hal_ga10b.c:1990`) | done, after power-on |
| `nvgpu_init_interrupt_setup` :812 (mask top) | done last, after priv ring and bus |
| `enable_priv_ring` :818, `bus.init_hw` :820 | done |
| `nvgpu_ptimer_init` :822 (timer SLCG 0x9600) | done |
| `fifo.reset_enable_hw` :839 | partial, with wrong registers |
| `nvgpu_init_fb_support` :840, `init_ltc_support` :841 | skipped |
| `acr.acr_init` :953 | done |
| `mm.init_mm_support` :970, `fifo_init_support` :971, `nvgpu_gr_enable_hw` :975 | run after ACR, or never |
| `acr_construct_execute` :977 | run before mm/fifo |
| `ce_init_support` :1049 | done |

- **Ordering requirement:** `nvgpu_init.c:988-994` says "For acr to boot, nvgpu_init_fb_support and init_mm_support is required", and :979-985 says LTC init "needs to be executed before ACR boot".
- **Fix:** before `ga10b_acr_boot_gsp`, run the FB CG prods plus fs_state, LTC fs_state, then GMMU/instance-block setup plus `fb_init_hw`. Which individual sub-step ACR actually depends on is CANNOT-DETERMINE.
- **Runlist CG registers:** `R/hal/power_features/cg/ga10b_gating_reglist.c:459-462` writes `nvgpu_writel(g, nvgpu_safe_add_u32(reg, runlist_pri_base), val)` for each active runlist. `D/ga10b_power.c:229-230` writes absolute 0x54 and 0x50. Fix: drop them here and write `pri_base+0x54 = 0`, `pri_base+0x50 = 0x4042` per runlist after the device table is parsed.
- **ELPG:** `R/hal/mc/mc_ga10b_fusa.c:270` guards the write with `if (!nvgpu_platform_is_silicon(g))`; the header comment at :43-47 says the register is "protected by priviledge level mask" and BPMP-controlled. The driver writes it conditionally on silicon. Fix: read and fail; never write.
- **Reference order inside reset_enable_hw** (`R/hal/fifo/fifo_ga10b_fusa.c:53-68`): `nvgpu_mc_reset_units(FIFO)` (mask is 0 on ga10b), then CE2 SLCG, then runlist SLCG/BLCG.
- **`fb.init_fs_state`** (`R/hal/fb/fb_ga10b_fusa.c:148-165`): compares LTC counts and writes 0x100ce4 = `U32_MAX` only if not priv-sec. The driver's uncalled `ga10b_fb_ltc_early_init` values match the FB SLCG/BLCG reglist.
- **`ga10b_ltc_init_fs_state`** (`R/hal/ltc/ltc_ga10b_fusa.c:41-77`): also does a read-modify-write of `ltc_ltcs_ltss_tstg_set_mgmt_1` (PLC recompress); the driver has no equivalent.
- **Skipped, housekeeping only:** slcg_acb, ecc, cic, static power-gate, clk support, grmgr, quiesce.

## M2 FB init — MISMATCH (and uncalled)

- **Matching parts** (`R/hal/fb/fb_gm20b_fusa.c:43-74`): 0x100c10 = `addr >> 8`; 0x100cc8 / 0x100ccc = ncoh(0x3) | vol_false(0) | `addr_f(phys>>12)`, i.e. `(v&0xfffffff)<<4`.
- **Separate pages:** `R/common/mm/mm.c:318,325` allocates `mmu_wr_mem` and `mmu_rd_mem` separately. The driver uses one page for both. Fix: allocate two.
- **Missing from `nvgpu_mm_setup_hw`** (`R/common/mm/mm.c:651-672`):
  - `ga10b_fb_intr_vectorid_init` plus `fb.intr.enable` (`fb_ga10b_fusa.c:172-175`).
  - `bar1_bind` and `bar2_bind` (wired at `hal_ga10b.c:1664-1665`). bar2 writes 0x1714 = `0x30000000|0x80000000|(inst>>12)` and polls 0x1710 pending/outstanding, 1000 x 5 us (`R/hal/bus/bus_gp10b_fusa.c:43-61`).
  - `fb_flush` twice: write 0x70000 = 1, poll bits 0 and 1 clear, 100 x 5 us (`R/hal/mm/cache/flush_gk20a_fusa.c:68-83`).
  - `mmu_fault.setup_hw`.
- **Page size:** there is no `fb_mmu_ctrl` page-size write on ga10b (`.set_mmu_page_size = NULL`, `hal_ga10b.c:956`).

## M3 page tables — MATCH

- **PDE** (`R/hal/mm/gmmu/gmmu_ga10b_fusa.c:81-116`): `pde_v[0] |= aperture | gmmu_new_pde_address_sys_f(lo32(phys>>12)) | vol_true; pde_v[1] |= phys_addr >> 24`.
- **Dual PDE0 small half** (:118-184): in words [2] and [3].
- **PTE** (:186-244): `valid | address_sys_f | aperture`, priv 0x20, `pte_w[1] = phys>>36 | kind`, vol for non-cacheable.
- **Fields** (`R/include/nvgpu/hw/ga10b/hw_gmmu_ga10b.h:62-92`): ncoh 0x6, vol 0x8, `(v&0xffffff)<<8`, shift 0xc.
- **Levels** (:353-380): low bits 47/38/29/21/12, entry sizes 8/8/8/16/8 bytes, giving 4/512/512/256/512 entries.
- **Aperture:** ncoh is correct because `.honors_aperture = true` (`R/os/linux/platform_ga10b_tegra.c:821`).
- **Format:** same two-u32 "new" layout; no wider PTE exists in this tree.

## M4 instance-block PDB — MATCH

- `R/hal/fifo/ramin_ga10b_fusa.c:32-52` writes word 128 = `aperture(ncoh 0x3) | vol_true(0x4) | big_page_size_64kb(0x800) | page_dir_base_lo_f(pdb>>12) | use_ver2_pt_format(0x400)` and word 129 = hi32.
- No fault-replay bits are set.
- The 64K bit is set unconditionally and only defines the big-page size, so it is consistent with 4K-only mappings.

## M5 TLB invalidate — MISMATCH

Reference: `R/hal/fb/fb_gm20b_fusa.c:76-154`, wired at `hal_ga10b.c:978`.

- **PDB word:** 0x100cb8 = `addr_f(pdb>>12) | sys_mem(0x2)` matches.
- **Invalidate word:** the reference writes `fb_mmu_invalidate_all_va_true_f() | fb_mmu_invalidate_trigger_true_f()` = 0x80000001 (:128-130). The driver writes 0x80000003. Fix: drop 0x2.
- **Pre-wait** (:103-112): poll 0x100c80 until `pri_fifo_space_v` (bits 23:16) is non-zero, 1000 x 2 us. Missing in the driver.
- **Post-wait** (:132-140): poll 0x100c80 until `pri_fifo_empty` (bit 15) is set, 1000 x 2 us, else `-ETIMEDOUT`. Missing. Note it polls the FIFO-empty bit, not the trigger bit.

## M6 priv ring / bus / interrupts — mostly MATCH

- **Priv ring — MATCH** (`R/hal/priv_ring/priv_ring_gm20b_fusa.c:60-93`):
  - The nine SLCG registers and their zero values match the reglist.
  - Command 0x4 at 0x12004c, 60 x 500 us poll and `start_results` bit 0 match. The gm20b header the function includes has the same offsets as ga10b's (0x12004c, 0x120050).
  - The reference additionally unmasks the priv-ring stall unit before the command (:65); not needed when polled.
- **Bus — MATCH for the enable** (`R/hal/bus/bus_ga10b.c:69-86`): the mask is bits 2-8, 26-28 and 31 = 0x9C0001FC at 0x1140. The W1C write to 0x1100 is not in the reference init; whether it is harmless is CANNOT-DETERMINE.
- **MC — MATCH** (`R/hal/mc/mc_intr_ga10b_fusa.c:314-323`): `func_priv_cpu_intr_top_en_clear__size_1_v()` is 1, so there is one write of `U32_MAX` at 0xB80000+0x1610; "leafs are not touched". One TOP_EN_CLR covers all four subtrees (top bits 0-3 map to leaves 0-7).
- **Order — MISMATCH:** the reference masks before priv ring (`nvgpu_init.c:812` vs :818). Fix: move `ga10b_mc_intr_enable` ahead of `ga10b_priv_ring_init`.

## M7 engine reset — MATCH

- **Register and bit:** `mc_device_enable_r(i)` = 0x600+4i; index `reset_id/32`, bit `reset_id%32` (`R/include/nvgpu/cic_mon.h:43-45`).
- **Sequence:** disable then enable (`R/common/mc/mc.c:55-61`); clear means in reset; no delay between the two.
- **Polling:** the reference polls the whole register equal to the written value, 500 us timeout with 10-200 us backoff (`R/hal/mc/mc_ga10b_fusa.c:72-106`), and ignores a poll failure.
- **Caveat:** `mc_device_enable__size_1_v()` is 1, so only index 0 exists; the driver should reject `reset_id >= 32`.

## M8 chip detect and fuses — MATCH (one minor mismatch)

- **Chip ID:** `R/hal/mc/mc_gm20b_fusa.c:41-46` computes arch = `((r>>24)&0x1f)<<4` and impl = `(r>>20)&0xf`, summed to 0x17B. That equals `(boot_0>>20)&0x1FF`. The driver masks 0xFFF, which fails if bits 31:29 are set. Fix: mask 0x1FF and also reject `boot_0 == 0xFFFFFFFF`.
- **Fuse offsets** (`R/include/nvgpu/hw/ga10b/hw_fuse_ga10b.h:79-88`): 0x820434, 0x820410, 0x8205ec bit 0, 0x820684 bit 0, 0x82067c, 0x821040 all match.
- **Fuse logic** (`R/hal/fuse/fuse_ga10b.c:291-350`): if priv_sec is non-zero, isolation == 0 is an error; it requires `is_wpr_enabled && !is_auto_fetch_disable`, else `-EINVAL`. Matches.

## M9 platform clocks and reset — MISMATCH (rate), partly CANNOT-DETERMINE

- **Clock set and order:** `R/os/linux/platform_ga10b_tegra.c:66-71` lists `{"sysclk", UINT_MAX}, {"gpc0clk", UINT_MAX}, {"gpc1clk", UINT_MAX}, {"fuse", UINT_MAX}`, enabled in that order (`R/os/linux/platform_gp10b_tegra.c:171-199`). Matches the driver's order.
- **Rate:** `clk_set_rate(c, rate)` is called for each at probe (:182-183). The driver never sets a rate. Fix: request the maximum rate on sysclk, gpc0 and gpc1.
- **Floor-swept GPCs:** gpc0/gpc1 are skipped if floor-swept (:105-130).
- **Reset:** the hooks exist (`platform_gp10b_tegra.c:202-226`) but have no caller in the tree. The driver's assert/deassert plus 1 ms has no reference counterpart.
- **CANNOT-DETERMINE:** what BPMP/genpd does with the reset during power-gate-on, and the numeric IDs (PG 35, reset 19, clocks 304/41/236/40), none of which are in nvgpu.
