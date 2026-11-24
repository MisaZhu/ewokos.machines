# ga10bd 审核调研原始记录：Verify FIFO/runlist/CE vs nvgpu

> 来源：本次审核中后台 subagent（general-purpose）的最终输出，**原文照录，未经改写**。
> 启动 2026-11-22 23:27:23，完成 2026-11-22 23:38:37（本地时间）。
> 性质：subagent 对照 nvgpu 源码的阅读结论，属于**待核实线索**，不是已验证事实；
> 各条的采纳/否决情况见同目录 README.md。

## 下发的调研任务（原文）

````text
RESEARCH ONLY — do not edit, create or delete any file. You are auditing a hand-written bare-metal GPU driver against its reference implementation and reporting discrepancies with evidence.

## Context
I am reviewing `ga10bd`, a standalone bring-up driver for the NVIDIA GA10B GPU (Tegra234 / Jetson Orin Nano) written for a hobby OS. It has never run on hardware; it was transcribed from NVIDIA's open-source L4T `nvgpu` driver (rel-36). My job is to find places where the transcription is WRONG before it touches a board, because a wrong register sequence on this board hangs it or raises a fatal bus error.

- Driver under review (read these): `/Users/tony/work/ewokos/machines/nvidia/orin.nano/system/drivers/ga10bd/` — for this task read `ga10b_fifo.c`, `ga10b_fifo.h`, `ga10b_ce.c`, `ga10b_regs.h` (runlist/CHRAM/doorbell/CE sections), and `ga10b_device_table()` + `ga10b_engine_reset()` in `ga10b_power.c` (lines ~246-283).
- Reference source (ground truth): `/tmp/gpu-research/linux-nvgpu/drivers/gpu/nvgpu/` (branch patches-rel-36). Relevant places: `hal/fifo/*ga10b*`, `hal/fifo/*gv11b*`, `hal/fifo/*gm20b*`, `hal/fifo/*tu104*` (ga10b HAL often reuses older chips' functions — check `hal/init/hal_ga10b.c` to see WHICH function is actually wired for ga10b before quoting it), `hal/top/*ga10b*`, `hal/mc/*ga10b*`, `hal/ce/*`, `common/ce/ce_app.c`, `common/fifo/*`, and headers `include/nvgpu/hw/ga10b/hw_{runlist,ram,pbdma,ctrl,func,top,ce,mc}_ga10b.h`.

Only trust the reference source. Do not rely on your memory of nvgpu or on comments in the driver under review (those comments are exactly what is being audited). When hal_ga10b.c wires a function, quote the wired function, not a similarly named one.

## Questions — for each, give a verdict MATCH / MISMATCH / CANNOT-DETERMINE, with reference `path:line` and a short quoted snippet as evidence, and for mismatches state exactly what the driver should do instead.

F1. TOP device table parsing. Driver assumes: `top_device_info_cfg` version (bits 3:0) must be 2; device count = cfg bits 31:20; each device is exactly 3 consecutive rows at `0x22800 + 4*n`; row1 → reset_id bits 7:0, device pri_base = bits 25:8 << 8, is_engine = bit 30; row2 → rleng_id bits 1:0, runlist_pri_base = bits 25:10 << 10. It never reads the device TYPE or instance. Report the real cfg field meanings (is bits 31:20 a ROW count or a DEVICE count?), the real per-row field layout including the device-type/instance fields and the chain bit, and the actual parse loop. Also list the numeric device-type enum values for graphics and for LCE (copy engine).

F2. Which runlist and which `rleng_id` does a channel use? The driver picks the FIRST device-table entry with a non-zero runlist base and uses that runlist + that entry's rleng_id for every channel (both the Copy-Engine test channel and the GR/compute channel), ignoring the engine. Find how nvgpu picks the runlist for a CE channel vs a GR channel on ga10b, and what value goes into the RAMFC `target` word (which device's rleng_id). Is it correct for a CE channel to sit on the GR device's runlist with the GR rleng_id?

F3. RAMFC contents. List every instance-block word that the ga10b ramfc setup function writes (word index macro, numeric index, value/encoding), and compare to the driver's `chan_init_instance()` + the constants in `ga10b_fifo.h` (signature 0xface at word 4, acquire 0 at word 12, gp_base w18, gp_base_hi w19, pb_header 0x20400000 w33, subdevice 0x30000001 w37, target w43, hce_ctrl 0x20 w57, config 0x1100 w61, intr_notify w62, set_channel_info w63). Flag anything missing, any wrong index and any wrong value — in particular whether `acquire = 0` is what nvgpu writes.

F4. Subchannels. nvgpu's `common/ce/ce_app.c` emits raw method headers such as 0x20018000; decode which subchannel they use. The driver emits all CE methods on subchannel 0. Is there any evidence in the reference tree (comments, defines, header docs) about whether subchannel number matters for routing methods to the copy engine vs graphics?

F5. Runlist buffer entry encodings. Compare driver `ga10b_rl_tsg()` / `ga10b_rl_ch()` (ga10b_fifo.h) with the wired get_tsg_entry / get_ch_entry functions and the `ram_rl_entry_*` macros: every field, shift and mask. In particular what timeslice scale/timeout values nvgpu writes for a TSG entry (the driver writes 0 for both) and the exact shift used for the USERD pointer and the instance pointer.

F6. Runlist submit. Compare driver `runlist_submit()` with the wired hw_submit + wait_pending: register offsets, the pointer shift/mask and target-aperture encoding of submit_base_lo/hi, the submit word encoding, and the pending bit. Also: what does nvgpu do with the scheduler-disable register around submit, and is writing `runlist base + 0x300 = 0x7ff|0x80000000` (driver's "VIRT_CH_CFG") something nvgpu does — if so from which function and against which base address (runlist base or usermode/func base)?

F7. Channel bind/enable. What does the wired ga10b channel bind / enable / disable / unbind write to CHRAM (offsets, values)? Driver writes value 0x2 to `chram_base + 4*chid` to enable and 0x3 to disable, where chram_base = ((channel_config >> 4) & 0xfffffff) << 4.

F8. Doorbell. Absolute BAR0 offset of the usermode doorbell register and the token encoding wired for ga10b. Driver uses offset 0xB80000 + 0x30090 and token = (chid & 0xfff) | ((runlist_id & 0x7f) << 16) with runlist_id = bits 31:16 of the runlist's doorbell_config register.

F9. FIFO / PBDMA hardware setup. List the register writes done by the wired fifo reset_enable_hw, fifo init/setup_hw, pbdma setup_hw, usermode setup_hw and fifo intr enable for ga10b. The driver does none of these except writing sched_disable=1. Which of them look functionally required for a polled (no-interrupt) channel to execute work, versus interrupt plumbing only?

F10. Copy engine. Compare the wired ce init/ce intr-enable functions and the method stream built in `common/ce/ce_app.c` against driver `ga10b_ce_init()` and `ga10b_ce_blit()`: method offsets, LAUNCH_DMA bit values, and how the semaphore/completion is expressed.

F11. USERD and GPFIFO. Confirm the USERD word index of GP_PUT/GP_GET, the gp_base / gp_base_hi encodings, and the GPFIFO entry format against the driver.

## Output
A numbered list F1..F11 in the format above, most severe mismatches first in a short summary at the top. Be concrete and terse; no general background. Under 1100 words. If a question would need hardware documentation that is not in the tree, say CANNOT-DETERMINE rather than guessing.
````

## subagent 返回结果（原文）

Prefixes: `REF` = `/tmp/gpu-research/linux-nvgpu/drivers/gpu/nvgpu`, `DRV` = `/Users/tony/work/ewokos/machines/nvidia/orin.nano/system/drivers/ga10bd`. Nothing was edited.

## Most severe first
1. **F1**: the device-table parser treats a row count as a device count and assumes a fixed 3-row stride, so the runlist base can come from the wrong rows.
2. **F9 (outside the requested lines)**: `DRV/ga10b_power.c:229-230` writes the runlist clock-gating values to absolute BAR0 0x50/0x54; nvgpu writes them at `runlist_pri_base + 0x50/0x54`.
3. **F3**: RAMFC signature 0xface is wrong for ga10b (should be 0xC76F), and acquire is never 0.
4. **F5**: TSG timeslice is written as 0; nvgpu writes a non-zero scale/timeout.
5. **F2**: runlist and rleng_id are chosen by "first row", not by device type.
6. **F4/F10**: CE methods use subchannel 0 (nvgpu uses 4), and LAUNCH_DMA bits and completion differ.
7. **F7**: teardown skips the CHRAM clear and runlist removal.

## F1. MISMATCH
- **cfg**: bits 3:0 must be 2 (driver matches). Bits 31:20 are a row count: `top_device_info_cfg_num_rows_v(r) (((r) >> 20U) & 0xfffU)` (`REF/include/nvgpu/hw/ga10b/hw_top_ga10b.h:83`, used at `REF/hal/top/top_ga10b_fusa.c:121`).
- **Row 0**: chain b31, `type_enum` 30:24, `instance_id` 23:16, `fault_id` 10:0.
- **Row 1**: chain b31, `is_engine` b30, pri_base 25:8 << 8, reset_id 7:0.
- **Row 2**: runlist_pri_base 25:10 << 10, rleng_id 1:0 (`hw_top_ga10b.h:86-99`). The driver's row-1/row-2 fields match.
- **Loop** (`top_ga10b_fusa.c:152-178`): skip rows equal to 0; the first non-zero row is row 0; read up to two more and stop when a row's chain bit is 0. Row 0 must have chain = 1 (`:66`). "Not all devices (e.g GSP) have 3 rows populated" (`:62-63`).
- **Types** (`REF/include/nvgpu/device.h:49,108`): GRAPHICS = 0, LCE = 19.
- **Fix**: iterate rows `< num_rows` with the skip/chain logic and store type and instance.

## F2. MISMATCH (selection); CANNOT-DETERMINE (real topology)
- **GR channel**: uses the runlist of GRAPHICS instance 0 (`REF/common/fifo/engines.c:616-627`).
- **CE channel**: uses the runlist of the last LCE (`engines.c:586-593`). Its only caller is dGPU/vidmem-only (`REF/common/mm/mm.c:339-346`), so this path never runs on ga10b.
- **RAMFC target**: the rleng_id of the lowest-engine_id engine on the channel's runlist, not the channel's own engine (`REF/hal/fifo/ramfc_ga10b_fusa.c:61-63,85-87`).
- **CE on the GR runlist**: supported by a comment only: "only runlist 0 has more than 1 engine(gr0, grcopy0 and grcopy1)" (`ramfc_ga10b_fusa.c:53-58`). The actual table contents need hardware.
- **Fix**: take the GR runlist from the type-0 device and the CE runlist from the last type-19 device. Compute target from the engine with the lowest `(rd(rl_base + 0x228 + 64*rleng) >> 24) & 0x3f` (`REF/hal/fifo/runlist_ga10b_fusa.c:44-52`). If the two runlist bases differ, a second runlist (CHRAM base, doorbell id, buffer) is needed.

## F3. MISMATCH
`ga10b_ramfc_setup` (`ramfc_ga10b_fusa.c:37-131`) writes:

| Word | Index | nvgpu value | Driver |
|---|---|---|---|
| gp_base | 18 | `((va>>3)&0x1fffffff)<<3` | match |
| gp_base_hi | 19 | `hi32&0xff \| ilog2(entries)<<16` | match |
| signature | 4 | 0xC76F | **0xface** |
| pb_header | 33 | 0x20400000 | match |
| subdevice | 37 | 0x30000001 | match |
| target | 43 | `rleng&3 \| 0x30000` | encoding matches; rleng per F2 |
| acquire | 12 | 0x102 minimum | **0** |
| set_channel_info | 63 | `veid<<8 \| chid<<16` | match |
| wfi_veid (RAMIN) | 134 | `veid&0x3f` | match |
| intr_notify | 62 | `intr_id \| 0x80000000` | **vector 0** |
| config | 61 | 0x1000, plus 0x100 if privileged | 0x1100 always |
| hce_ctrl | 57 | 0x20 if privileged | 0x20 always |

- **Signature**: wired `gp10b_pbdma_get_signature` returns `GPU_LIT_GPFIFO_CLASS | pbdma_signature_sw_zero_f()` (`REF/hal/fifo/pbdma_gp10b_fusa.c:30-34`), which is `AMPERE_CHANNEL_GPFIFO_B` = 0xC76F (`REF/hal/init/hal_ga10b_litter.c:148-150`, `REF/include/nvgpu/class.h:103`). 0xface is the unwired gm20b variant; the driver comment at `DRV/ga10b_fifo.h:120-122` is backwards.
- **Acquire**: `gm20b_pbdma_acquire_val` returns `retry_man_2 | retry_exp_2` = 0x102 for timeout 0 (`REF/hal/fifo/pbdma_gm20b_fusa.c:246-251`).
- **intr_notify**: the vector is the engine `intr_id`, read from `rl_base + 0x204 + 64*rleng`, bits 20:16. Interrupt-only.
- **Subctx**: at bind nvgpu also programs the subctx PDB table and valid mask (words 168+, 166) for every channel. The driver does this only for the GR channel (`DRV/ga10b_ctx.h:120-125`), not the CE channel.

## F4. CANNOT-DETERMINE (routing); driver differs from nvgpu
- 0x20018000 / 0x20028100 / 0x20068102 / 0x200180c0 (`REF/common/ce/ce_app.c:350,262,292,330`) have bit 15 set, so subchannel 4 under the 15:13 layout. The tree has no macro for that field.
- Host semaphore methods use subchannel 0 (`0x20010017..1b`, `REF/hal/sync/sema_cmdbuf_gv11b.c:45-61,89`).
- Subchannels 5/6/7 are SW-method subchannels (`REF/hal/fifo/pbdma_ga10b_fusa.c:165-167`).
- Nothing in the tree documents subchannel-to-engine routing. Use subchannel 4 for CE, as nvgpu does.

## F5. MISMATCH (TSG), MATCH (channel)
- **TSG w0**: `type_tsg | scale<<16 | timeout<<24`, with `WARN_ON(timeslice == 0U)` (`REF/hal/fifo/runlist_ram_gv11b_fusa.c:53,66-68`). My derivation from the default 1024 µs timeslice and 31.25 MHz ptimer gives timeout 128, scale 3, so w0 = 0x80030001. The driver writes 0x1.
- **TSG w1/w2/w3**: match.
- **Channel entry**: USERD `((lo32>>8)&0xffffff)<<8`, inst `((lo32>>12)&0xfffff)<<12 | chid`, targets at bits 4 and 6, runqueue selector 0 (`:85-109`). All match.

## F6. MATCH (registers); MISMATCH (sched-disable and VIRT_CH_CFG usage)
- **Registers**: 0x80 = `((lo32>>10)&0x3fffff)<<10 | target`, 0x84 = `hi32&0xff`, 0x88 = `0<<16 | count`, pending = 0x8c & 0x8000 (`REF/hal/fifo/runlist_fifo_ga10b_fusa.c:53-93`). All match.
- **Target**: 3 (ncoh) is right unless the device tree says `dma-coherent`, in which case it is 2.
- **Sched-disable**: nvgpu never writes 0x94 at init or around a submit; only the recovery, preempt and engine-activity paths do (`REF/common/fifo/runlist.c:919`). Drop both driver writes. Whether pending clears while disabled is CANNOT-DETERMINE.
- **VIRT_CH_CFG**: nvgpu writes it in `ga10b_usermode_setup_hw` (`REF/hal/fifo/usermode_ga10b_fusa.c:89-98`), called from `ga10b_init_fifo_setup_hw`. It targets the runlist base + 0x300, as read-OR-write, once at init for every runlist. The driver has the right base and bits but does a blind overwrite after each submit.

## F7. MATCH (values); MISMATCH (order and teardown)
- **Enable/disable**: `chram_base + 4*chid` gets 0x2 / 0x3 (`REF/hal/fifo/channel_ga10b_fusa.c:59-75`). The chram_base derivation matches.
- **Bind**: subctx programming, then enable (`:77-96`). It runs after ramfc setup and the runlist submit (`REF/common/fifo/channel.c:1429-1435,1612`); the driver enables before submitting.
- **Unbind**: writes 0xffffffff (`:136-152`) after TSG disable, preempt and resubmitting the runlist without the channel. The driver writes only 0x3 and frees memory the runlist still references.

## F8. MATCH
- **Offset**: `g->regs + 0xb80000 + 0x30000`, plus `func_doorbell_r() - func_cfg0_r()` = 0x90, giving 0xBB0090 (`REF/os/linux/module_usermode.c:43`, `REF/os/linux/io_usermode.c:13`).
- **Token**: `(chid&0xfff) | (runlist_id&0x7f)<<16` (`REF/hal/fifo/usermode_tu104.c:64-65`), with runlist_id = doorbell_config bits 31:16 (`runlist_ga10b_fusa.c:36-42`).

## F9. Driver omits most of it
- **reset_enable_hw** (`REF/hal/fifo/fifo_ga10b_fusa.c:46-83`):
  - FIFO unit reset is a no-op mask on ga10b (`REF/hal/mc/mc_ga10b_fusa.c:118-131`).
  - Clock gating: 0x104204 = 0, `rl_base+0x54` = 0, `rl_base+0x50` = 0x4042 (`REF/hal/power_features/cg/ga10b_gating_reglist.c:62-64,238-240,459-462`).
  - `pbdma.setup_hw` is NULL (`REF/hal/init/hal_ga10b.c:1093`).
- **init_fifo_setup_hw** (`:110-141`):
  - VIRT_CH_CFG per runlist.
  - `rl_base+0x28` = 0x64 (USERD writeback timer).
  - Interrupt enables: `rl+0x100` W1C, `rl+0x120` = 0x31007, `rl+0x148` = ~0, `rl+0x160/0x164` cpu/gsp bits, `rl+0x220+64*rleng` ctxsw timeout, PBDMA intr_0/1 en_set.
- **Looks required for a polled channel**: VIRT_CH_CFG. The writeback timer matters only if GP_GET is read. Whether either is strictly required is CANNOT-DETERMINE.
- **Interrupt/CG plumbing only**: everything else. Polled mode hides PBDMA faults ("SIGNATURE bad", "ACQUIRE timeout", `pbdma_ga10b_fusa.c:144-146`), so poll `rl+0x100`.

## F10. MISMATCH
- **Init** (`REF/common/ce/ce.c:36-84`):
  - nvgpu resets all type-19 devices; the driver resets one device matched by a hardcoded pri_base 0x104000.
  - nvgpu also writes 0x104200 = 0xc244 and 0x104044 = 1; the driver does not.
  - The force-barriers loop count is `top_num_ces` (0x22444 & 0x1f), not `hweight(pce_map)`.
  - The driver's pce_map read is justified (`REF/hal/ce/ce_ga10b_fusa.c:253-262`).
- **Interrupts**: enable mask is 0x8000007F (no bit 30); the driver writes 0xC000007F. In this build notify_ctrl has CPU disabled (`ce_ga10b_fusa.c:123-128`); the driver enables it.
- **Stream** (`ce_app.c:250-333`):
  - Methods: nvgpu sends 0x400(2), 0x260, 0x408(6), 0x264, 0x300. The driver adds 0x240-0x248 and omits 0x260/0x264. The offsets it shares match.
  - LAUNCH_DMA: nvgpu sends 0x3185 (bits 1:0 = 1, flush, bits 7/8, bits 12/13 = physical, no semaphore, no bit 9). The driver sends 0x18E/0x38E with virtual addresses.
- **Completion**: nvgpu uses host semaphore methods on subchannel 0 (`sema_cmdbuf_gv11b.c:45-61,87-95`), not the CE semaphore. There is no `clc7b5.h` in the tree, so whether the driver's variant is valid is CANNOT-DETERMINE.

## F11. MATCH
- **USERD**: GP_GET at word 34, GP_PUT at word 35 (`REF/include/nvgpu/hw/ga10b/hw_ram_ga10b.h:149-150`).
- **GPFIFO entry**: entry0 = lo32(va), entry1 = `hi32 | (words&0x1fffff)<<10` (`pbdma_gm20b_fusa.c:288-290`).
- **gp_base / gp_base_hi**: as in F3.
