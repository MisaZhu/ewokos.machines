# ga10bd 审核调研原始记录：Verify GR/FECS/golden ctx vs nvgpu

> 来源：本次审核中后台 subagent（general-purpose）的最终输出，**原文照录，未经改写**。
> 启动 2026-11-22 23:27:49，完成 2026-11-22 23:43:19（本地时间）。
> 性质：subagent 对照 nvgpu 源码的阅读结论，属于**待核实线索**，不是已验证事实；
> 各条的采纳/否决情况见同目录 README.md。

## 下发的调研任务（原文）

````text
RESEARCH ONLY — do not edit, create or delete any file. You are auditing a hand-written bare-metal GPU driver against its reference implementation and reporting discrepancies with evidence.

## Context
I am reviewing `ga10bd`, a standalone bring-up driver for the NVIDIA GA10B GPU (Tegra234 / Jetson Orin Nano) written for a hobby OS. It has never run on hardware; it was transcribed from NVIDIA's open-source L4T `nvgpu` driver (rel-36). My job is to find places where the transcription is WRONG before it touches a board. The part you are checking is the graphics/compute engine (GR) bring-up: starting the FECS/GPCCS context-switch falcons, the FECS method handshake, and building the "golden" context image.

- Driver under review: `/Users/tony/work/ewokos/machines/nvidia/orin.nano/system/drivers/ga10bd/` — read `ga10b_gr.c` fully, plus `ga10b_ctx.h`, `ga10b_netlist.h`, and the GR/FECS section of `ga10b_regs.h`. The CLI flow that calls it is `cmd_gr()` in `ga10bd.c`.
- Reference (ground truth): `/tmp/gpu-research/linux-nvgpu/drivers/gpu/nvgpu/` (branch patches-rel-36). Relevant: `common/gr/gr.c`, `common/gr/gr_falcon.c`, `common/gr/obj_ctx.c`, `common/gr/ctx.c`, `common/gr/subctx.c`, `common/gr/fs_state.c`, `common/init/nvgpu_init.c`, `hal/gr/falcon/*`, `hal/gr/init/*`, `hal/gr/intr/*`, `hal/gr/ctxsw_prog/*`, `hal/fifo/ramin_*`, `common/netlist/*`, and headers `include/nvgpu/hw/ga10b/hw_{gr,ctxsw_prog,ram}_ga10b.h`. The ga10b HAL reuses many older-chip functions — ALWAYS check `hal/init/hal_ga10b.c` to see which function is actually wired for ga10b before quoting one.

Only trust the reference source. Do not rely on memory of nvgpu or on comments inside the driver (those comments are what is being audited).

## Questions — for each give MATCH / MISMATCH / CANNOT-DETERMINE, reference `path:line` + short quoted snippet, and for mismatches say exactly what the driver should do instead.

G1. ORDER relative to the secure boot (ACR) step. In `nvgpu_init.c` finalize_poweron, where do the GR engine reset (mc enable_dev off/on for the graphics device), the GPC reset (`reset_gpcs`), `sw_non_ctx_load` register writes, and GR interrupt/fifo-access enables happen relative to the ACR step that loads FECS/GPCCS firmware? The driver does things in this order: ACR boot (which loads FECS/GPCCS) → start GPCCS/FECS cpu → wait ctxsw handshake → override_context_reset → MC engine reset of GR → GPC reset → engine_config write → FECS DISCOVER_IMAGE_SIZE → golden context. I suspect resetting the GR engine AFTER the falcons were loaded and started is wrong. Give the true order as a numbered list of the GR-related steps from power-on to "golden image saved", naming the function for each.

G2. Starting the falcons. Quote the wired start_gpccs and start_fecs functions and list the exact registers (numeric offsets) and values they write. The driver writes the FECS falcon's generic MAILBOX0 (0x409040) = 0 and MAILBOX1 (0x409044) = 1, then cpuctl_alias startcpu for GPCCS (0x409130+0x11000) and FECS (0x409130). Are those the right registers — or should it be the ctxsw mailbox registers (0x409800+4i) and a ctxsw mailbox CLEAR register? Give the numeric offset of `gr_fecs_ctxsw_mailbox_r(i)`, `gr_fecs_ctxsw_mailbox_clear_r(i)`, the GPCCS dmactl register, and anything else written.

G3. FECS method protocol. The driver's `fecs_method(adr, data)` always does: write ctxsw_mailbox(0)=0, method_data=data, method_push=adr, then waits until ctxsw_mailbox(0) == 1 exactly. Quote the wired submit function and the per-method op parameters for these methods: BIND_PTR (0x3), GOLDEN_IMAGE_SAVE (0x9), DISCOVER_IMAGE_SIZE (0x10), and the watchdog-timeout method. For each: which mailbox id gets pre-written, the mailbox-clear mask, the OK condition (equal / not-equal / AND-mask and its value), the FAIL condition, and WHERE the return value is read from. The driver reads the image size from ctxsw_mailbox(1) after waiting for mailbox(0)==1 — is that right?

G4. After the handshake. Quote wait_ctxsw_ready as wired for ga10b: what it waits for, and what it does afterwards (e.g. current_ctx invalidation, watchdog timeout method). Does the driver omit a step that FECS needs?

G5. GR hardware init the driver may have skipped. List the steps of the functions that run between "ctxsw ready" and golden-context creation (gr init_hw / setup_hw equivalents: gpc_mmu, fifo_access, interrupt enables, tpc mask, sm id numbering, sw_non_ctx_load, non_ctx local/global compute lists, engine config, pd/ds tpc-per-gpc, cwd fs, etc.) and mark for each whether `ga10b_gr.c` does it anywhere. I need to know which omitted ones are functionally required for a compute-only context versus interrupt/ECC/graphics-only plumbing.

G6. Golden context sequence. Compare `gr_golden_init()` step by step against the reference golden-image creation path as wired for rel-36 ga10b (obj_ctx.c: init_hw_state, commit_hw_state, alloc_sw_bundle, save golden). Report every reordering, omission or extra step. Include: whether the golden image creation uses a real channel/TSG or a dedicated instance block, the two golden saves + comparison if any, and which register filter (`is_allowed_reg` or similar) is applied to sw_ctx_load — check the driver's graphics-range table in `gr_reg_is_compute()` against the reference table entry by entry.

G7. Bundle loaders. Compare `gr_load_bundle`, `gr_load_veid_bundle`, `gr_load_bundle64`, `gr_load_method_init` with the wired functions: register offsets, the stats-counter special case, go-idle handling, the veid field position, and idle-wait conditions (which gr_status bits).

G8. `override_context_reset` and `reset_gpcs`: quote the wired functions and give the numeric values written; driver writes FECS ctxsw_reset_ctl 0x10 then 0x110, GPCCS 0x20 then 0xa20, and GPCS engine reset 0 then 1.

G9. Context header + RAMIN. Check `ga10b_ctx.h` word indices against `hw_ctxsw_prog_ga10b.h` (byte offset / 4) and check `ga10b_ramin_gr()` against the RAMIN macros + the wired subctx/ramin functions: engine_wfi_target/ptr encoding (word 132/133), veid word 134, sc_pdb_valid words 166/167, sc_page_dir_base words 168..171. Flag wrong indices or encodings.

G10. Per-channel context load. Compare `ga10b_gr_ctx_prepare()` with the reference per-channel path (ctx alloc + load golden image + patch + subctx header + commit to instance block): anything missing that FECS needs to restore the context on first use?

## Output
Start with a short ranked summary of the most severe mismatches (the ones that would make hardware bring-up fail), then G1..G10. Terse, evidence-first, under 1300 words. Say CANNOT-DETERMINE rather than guess.
````

## subagent 返回结果（原文）

**Audit of `ga10bd` GR bring-up against nvgpu (patches-rel-36)**

The driver will not get past GR init as written: it stalls at the first FECS method. Nothing was run or edited; every verdict comes from reading the source.

- REF = `/tmp/gpu-research/linux-nvgpu/drivers/gpu/nvgpu`
- DRV = `/Users/tony/work/ewokos/machines/nvidia/orin.nano/system/drivers/ga10bd/ga10b_gr.c`

## Ranked blockers

1. **FECS method wait conditions (G3).** `fecs_method()` (DRV:69-80) waits for mailbox(0)==1 on every method. DISCOVER_IMAGE_SIZE succeeds on mailbox(0)!=0 and returns the size in mailbox(0), so init times out there (unless the size is literally 1). BIND_PTR succeeds on bit 0x10, so it also times out.
2. **`start_fecs` writes the wrong mailboxes (G2).** The driver writes the generic falcon mailboxes 0x409040/0x409044 instead of ctxsw mailboxes 0x409800/0x409804/0x409818.
3. **GR engine and GPC reset happen after the falcons are booted (G1).** The reference does both in `nvgpu_gr_enable_hw`, before ACR. The driver also never loads `sw_non_ctx_load` or the non-ctx compute lists (regions 6/48/50).
4. **Post-handshake steps missing (G4).** No current-ctx invalidate and no SET_WATCHDOG_TIMEOUT.
5. **Golden init mixes MIG and non-MIG behaviour (G6/G7).** It also applies a stats-counter mask without the paired restore.
6. **SM-id ordering is linear (G5).** The reference uses a perf-ordered table. This is internally consistent, so probably not fatal.

## G1 — Order: MISMATCH

True order, from `common/init/nvgpu_init.c:975-1077`:

1. `nvgpu_gr_enable_hw` (:975; body in `common/gr/gr.c:624-673`):
   - engine disable, `reset_gpcs`, engine enable
   - MC GR interrupt, `enable_interrupts`, `fifo_access(true)`
   - `sw_non_ctx_load`, non-ctx local/global compute lists
   - `wait_mem_scrubbing`, `wait_idle`
2. `acr_construct_execute` (:977).
3. PMU rtos init (:1015).
4. `gr_init_support` (:1020):
   - `sm_id_config_early`
   - `start_gpccs`, `start_fecs`, `wait_ctxsw_ready`
   - `init_ctx_state` (DISCOVER_IMAGE_SIZE)
   - setup_hw: `eng_config`, `gpc_mmu`, `fecs_host_int_enable`, exception enables, `nvgpu_gr_fs_state_init`, `wait_idle`
5. `init_golden_image` (:1077).

Driver `ga10b_gr_init` (DRV:548-631) does ACR, falcon start, handshake, `override_context_reset`, MC engine reset, `gr_reset_gpcs`, ENGINE_CONFIG, DISCOVER, golden.

Fix:
- Move the engine reset, with `reset_gpcs` inside the disable window, before ACR.
- Add the non-ctx lists (regions 6/48/50), `fifo_access` and `wait_mem_scrubbing` there.
- Drop the top-level `override_context_reset`; the reference calls it only inside golden init (`common/gr/obj_ctx.c:610-689`).

Whether a late engine reset kills the booted FECS firmware: CANNOT-DETERMINE.

## G2 — Starting falcons: MISMATCH

Wired at `hal/init/hal_ga10b.c:789-790`: `gm20b_gr_falcon_start_gpccs` and `gm20b_gr_falcon_start_fecs`.

- **FECS** (`hal/gr/falcon/gr_falcon_gm20b_fusa.c:103-110`): `fecs_ctxsw_clear_mailbox(g, 0U, ~U32(0U)); nvgpu_writel(g, gr_fecs_ctxsw_mailbox_r(1U), 1U); fecs_ctxsw_clear_mailbox(g, 6U, 0xffffffffU);` then `gr_fecs_cpuctl_alias_r()` = startcpu.
- **GPCCS** (same file :83-101): in secure mode, only `reg_offset + gr_fecs_cpuctl_alias_r()`.
- **Offsets** (`include/nvgpu/hw/ga10b/hw_gr_ga10b.h`):
  - `gr_fecs_ctxsw_mailbox_r(i)` = 0x409800+4i (:571-572)
  - `gr_fecs_cpuctl_alias_r` = 0x409130 (:472)
  - `gr_gpccs_dmactl_r` = 0x41a10c (:760), written only in non-secure boot
- **No `gr_fecs_ctxsw_mailbox_clear_r` exists.** The clear is a read-modify-write of the mailbox itself (`hal/gr/falcon/gr_falcon_ga10b_fusa.c:78-87`, wired at `hal_ga10b.c:787`).

The driver's cpuctl_alias writes (0x41a130=2, 0x409130=2) match. Fix: write 0x409800=0, 0x409804=1, 0x409818=0 instead of 0x409040/0x409044.

## G3 — FECS methods: MISMATCH

Wired chain: `ga100_gr_falcon_ctrl_ctxsw` (`hal_ga10b.c:795`), then tu104, gp10b, gm20b.

Submit sequence (`gr_falcon_gm20b_fusa.c:700-721`): if mailbox id != 0, write that mailbox; clear mailbox(0) bits `clr`; write data to 0x409500; write addr to 0x409504; poll.

| Method | Clear mask | OK | FAIL | Return | Ref lines |
|---|---|---|---|---|---|
| BIND_PTR 0x3 | 0x30 | mb0 & 0x10 | mb0 & 0x20 | none | :817-826 |
| GOLDEN_SAVE 0x9 | 0x3 | mb0 & 0x1 | mb0 & 0x2 | none | :828-837 |
| DISCOVER_IMAGE_SIZE 0x10 | ~0 | mb0 != 0 | none | mb0 | :736-742, :784-788 |
| WATCHDOG 0x21 | ~0 | no wait (`GR_IS_UCODE_OP_SKIP`) | none | none | `hal/gr/falcon/gr_falcon_ga100.c:52-61` |

Driver differences:
- Every method waits for mb0==1 with no fail check.
- The size is read from mailbox(1) instead of mailbox(0).
- The watchdog method is never sent.
- It writes mailbox(0)=0 instead of clearing only the method's bits (a superset, probably harmless).

The BIND_PTR data word matches (`gr_falcon_gm20b_fusa.c:908-920`: `ptr | 0x30000000 | valid`).

## G4 — `wait_ctxsw_ready`: MISMATCH

Wired `gm20b_gr_falcon_wait_ctxsw_ready` (`hal_ga10b.c:794`). The poll for mailbox(0)==1 matches (`gr_falcon_gm20b_fusa.c:495-497`, `common/gr/gr_falcon_priv.h:86`).

Omitted:
- **Current-ctx invalidate** (:503-507): `if (... nvgpu_is_enabled(g, NVGPU_SEC_SECUREGPCCS)) nvgpu_writel(g, gr_fecs_current_ctx_r(), gr_fecs_current_ctx_valid_false_f())`, i.e. 0x409b00=0. The flag is true when the priv-sec fuse is set (`hal/fuse/fuse_ga10b.c:318-319`).
- **SET_WATCHDOG_TIMEOUT** (:529-530), default data 0x3FFFFFFF (:50). On silicon it can be sysclk-derived (:510-524); runtime value CANNOT-DETERMINE.

## G5 — HW init before golden: MISMATCH (omissions)

Omitted entirely:
- `enable_interrupts`
- `fifo_access` (`hal/gr/init/gr_init_gm20b_fusa.c:91-108`, 0x400500 bits 0 and 16)
- `sw_non_ctx_load` and the non-ctx compute lists
- `wait_mem_scrubbing`
- `gpc_mmu` (`hal/gr/init/gr_init_ga10b_fusa.c:126-152`)
- `fecs_host_int_enable`
- hww/GPC exception enables
- `sm_id_config_early` before falcon start
- `nvgpu_cg_blcg_gr_load_enable` (`obj_ctx.c:680`)

Done and numerically matching:
- `eng_config` 0xF, the non-MIG value (`gr_init_ga10b_fusa.c:369-382`)
- `fs_state` (:259-302)
- `sm_cfg` and `pd_sm_id` register offsets
- CWD tables (`hal/gr/init/gr_init_gv11b_fusa.c:446-529`)
- `cwd_gpcs_tpcs_num`

**SM ordering: MISMATCH.** The reference builds the table with `gv100_gr_config_init_sm_id_table` (`hal_ga10b.c:539`; `hal/gr/config/gr_config_gv100_fusa.c:405-459`). It is perf-ordered and picks with `if (perf >= maxperf)`, so it is not gpc-major linear. The exact order for Orin Nano is CANNOT-DETERMINE without evaluating it.

My judgement on what matters for compute-only: the non-ctx lists, `fifo_access`, `gpc_mmu` and the current-ctx invalidate. Interrupt enables are not needed for a polled bring-up.

## G6 — Golden sequence: order MATCH, content MISMATCH

- **Step order: MATCH.** DRV:412-495 follows `obj_ctx.c:610-689`, `:691-770` and `:562-608`.
- **Dedicated inst block: MATCH.** No channel, TSG or RAMFC (`obj_ctx.c:991-1038`: "golden image does not use host states").
- **Single save: MATCH.** The second save and compare exist only under `CONFIG_NVGPU_GR_GOLDEN_CTX_VERIFICATION` (`obj_ctx.c:772-839`; defined only in `Makefile.shared.configs:124`, safety profiles).
- **`set_current_ctx_invalid` after save: MATCH** (`obj_ctx.c:908`).

Filter:
- Wired `ga10b_gr_init_is_allowed_reg` (`hal_ga10b.c:705`; `gr_init_ga10b_fusa.c:532-626`).
- Applied only `if nvgpu_is_enabled(g, NVGPU_SUPPORT_MIG)` (`obj_ctx.c:645-657`); the flag is set only at `os/linux/sysfs.c:1429`.
- The driver's `gr_reg_is_compute()` table (DRV:197-215) is MATCH, 22 of 22 entries.
- The driver applies it unconditionally: MISMATCH against default nvgpu.

The driver is a hybrid. Pick one mode:

| Step | Non-MIG (default) | MIG | Driver |
|---|---|---|---|
| `sw_ctx_load` filter | none | filtered | filtered |
| `eng_config` | 0xF | 0x2 | 0xF |
| `commit_global_timeslice` (`obj_ctx.c:712-715`) | yes | no | yes; its three registers are among those the filter skips |
| `pd_tpc_per_gpc` | yes | no | yes |
| bundle / method-class filters | none | filtered | none |
| global ctx buffers (`obj_ctx.c:454-560`) | committed | skipped | skipped |

Also omitted: the graphics auto-go-idle bit17 at 0x400088 (`gr_init_ga10b_fusa.c:79-124`).

## G7 — Bundle loaders: MATCH except stats-counter

- **`load_sw_bundle_init`: MISMATCH.** `gm20b_gr_init_load_sw_bundle_init` is wired (`hal_ga10b.c:667-671`; `gr_init_gm20b_fusa.c:487-527`) and has no stats-counter mask. The driver's `if (addr == 0x00a9) val &= ~0x18000` (DRV:313-335) comes from `gv11b_gr_init_load_sw_bundle_init` (`gr_init_gv11b_fusa.c:1027-1081`), a verification-build function paired with `restore_stats_counter_bundle_data` (:1100-1117), which the driver lacks. Fix: remove the mask.
- **Registers, go-idle test, fe-idle wait: MATCH.** Data to 0x400204, addr to 0x400200, go-idle 0xe100 then `wait_idle`, always `wait_fe_idle` (bit2).
- **veid bundle: MATCH.** `addr | (j<<20)`, 6-bit field (`gr_init_gv11b_fusa.c:803-868`). The runtime `max_veid_count` (driver uses 64) was not traced.
- **bundle64: MATCH** (`hal/gr/init/gr_init_tu104.c:107-154`; hi word at 0x40020c).
- **`method_init`: MATCH** (`gr_init_gm20b_fusa.c:357-397`).
- **Idle bits: MATCH.** `wait_idle` uses status bit0 (`gr_init_ga10b_fusa.c:331-367`).

## G8 — Reset values: MATCH

- **`override_context_reset`** (`gr_init_ga10b_fusa.c:45-77`): FECS 0x409614 = 0x10 then 0x110; GPCCS 0x41a614 = 0x20 then 0xa20; 10 us delays with readbacks.
- **`reset_gpcs`** (:384-444): 0x41a610 = 0, then 1, then a per-GPC check that 0x502610+stride == 1.
- Only difference: the delay is `NVGPU_GR_GPCS_RESET_DELAY_US 20U` (:42); the driver uses 10 us.
- The driver's comments describe the bit polarity backwards.

## G9 — Layout: MATCH

- **Header words** (`include/nvgpu/hw/ga10b/hw_ctxsw_prog_ga10b.h`): all driver indices equal offset/4 — 0x0c, 0x10, 0x14, 0x18, 0x28, 0x2c, 0x3c, 0x78, 0x7c, 0x84, 0x94, 0xa0, 0xa4, 0xa8, 0xd0, 0xd4, 0xf4, 0xf8. Per-veid type is 0x21, header size 0x300.
- **RAMIN** (`include/nvgpu/hw/ga10b/hw_ram_ga10b.h:62-116`): words 128/129, 131 = 0xcafeca11, 132 (mode bit2, ptr_lo<<12), 133, 166, 168/169 match.
- **Encodings** match `hal/fifo/ramin_gv11b_fusa.c:32-90`.
- **Veid-header fields** match `common/gr/subctx.c:131-159`.

## G10 — Per-channel load: MATCH with minor gaps

Compared against `obj_ctx.c:1262-1360` and `common/gr/ctx.c:353-424`.

Gaps:
- Words 0xd8/0xdc (gfxp/cilp save counts) are not zeroed (`hal/gr/ctxsw_prog/ctxsw_prog_gp10b.c:100-118`).
- The access map is all-zero with `use_map`; the reference fills a whitelist.
- Unused subctx PDB entries are left zero instead of the "invalid" target (`hal/mm/mm_gv11b_fusa.c:30-88`).
- No `enable_mme_config_ptimer` (`obj_ctx.c:1335-1345`).
- No global-ctx-buffer patches (non-MIG only).

Whether any of these breaks compute: CANNOT-DETERMINE from source.
