# ga10bd 审核调研原始记录：Verify ACR/falcon/PMU vs nvgpu

> 来源：本次审核中后台 subagent（general-purpose）的最终输出，**原文照录，未经改写**。
> 启动 2026-11-22 23:28:18，完成 2026-11-22 23:38:45（本地时间）。
> 性质：subagent 对照 nvgpu 源码的阅读结论，属于**待核实线索**，不是已验证事实；
> 各条的采纳/否决情况见同目录 README.md。

## 下发的调研任务（原文）

````text
RESEARCH ONLY — do not edit, create or delete any file. You are auditing a hand-written bare-metal GPU driver against its reference implementation and reporting discrepancies with evidence.

## Context
I am reviewing `ga10bd`, a standalone bring-up driver for the NVIDIA GA10B GPU (Tegra234 / Jetson Orin Nano) written for a hobby OS. It has never run on hardware; it was transcribed from NVIDIA's open-source L4T `nvgpu` driver (rel-36). My job is to find places where the transcription is WRONG before it touches a board. The part you are checking is the secure-boot chain: the ACR high-secure firmware that runs on the GSP RISC-V core, the "LS blob" the driver builds for it, the falcon/RISC-V register helpers, and the PMU RISC-V boot. The retail chip has the priv_sec_en fuse set, so everything must follow the SECURE path.

- Driver under review: `/Users/tony/work/ewokos/machines/nvidia/orin.nano/system/drivers/ga10bd/` — read `ga10b_acr.c`, `ga10b_falcon.c`, `ga10b_pmu.c`, `ga10b_fw_parse.c`, `ga10b_fw.h`, and the falcon/priscv/WPR sections of `ga10b_regs.h`.
- Reference (ground truth): `/tmp/gpu-research/linux-nvgpu/drivers/gpu/nvgpu/` (branch patches-rel-36). Relevant: `common/acr/*` (esp. `acr_sw_ga10b.c`, `acr_bootstrap.c`, `acr_blob_construct.c`, `nvgpu_acr_interface.h`, `acr_wpr.c`), `common/riscv/riscv.c`, `common/falcon/*`, `hal/falcon/*`, `hal/acr/*`, `hal/gsp/*`, `hal/pmu/pmu_ga10b.c`, `common/pmu/pmu_rtos_init.c`, `common/pmu/fw/*`, `common/gr/gr_falcon.c`, `hal/fb/fb_ga10b*`, and headers `include/nvgpu/hw/ga10b/hw_{falcon,priscv,pgsp,pwr,fb}_ga10b.h`. The ga10b HAL reuses older-chip functions — ALWAYS check `hal/init/hal_ga10b.c` (and the acr/falcon sw-init files) to see which function is actually wired for ga10b before quoting one. Where the code branches on safety/FuSa/"NVGPU_HAL_NON_FUSA"/dbg-vs-prod, the configuration that matters is: non-safety, production-signed firmware, priv security enabled.

Only trust the reference source. Do not rely on memory of nvgpu or on comments inside the driver (those comments are what is being audited).

## Questions — for each give MATCH / MISMATCH / CANNOT-DETERMINE, reference `path:line` + short quoted snippet, and for mismatches say exactly what the driver should do instead.

A1. Falcon reset on the GSP RISC-V core. Driver `ga10b_falcon_reset()` writes IRQMCLR=0xffffffff, then CPUCTL (base+0x100) = hreset bit3, delays 10us, then CPUCTL = 0, and does NOT wait for memory scrubbing. Quote what `nvgpu_falcon_reset` actually does for the GSP falcon on ga10b (engine-dependent reset hook? which register/offset/value? scrub wait on which register/bit?).

A2. HS boot sequence. Compare driver `ga10b_acr_boot_gsp()` with the reference riscv HS-ucode load+bootstrap function and its caller: order of reset / mailbox writes / BCR write / IMEM copy / DMEM copies / manifest placement / start. Check: (a) the value written by the wired set_bcr; (b) how DMEM size is obtained (driver: falcon HWCFG at base+0x108, bits 17:9, << 8); (c) what the wired falcon `bootstrap` hook writes for a RISC-V core (driver writes only priscv CPUCTL = 1 at priscv base + 0x388).

A3. IMEM/DMEM PIO copy. Compare driver `ga10b_falcon_pio_load()` with the wired copy_to_imem / copy_to_dmem: the IMEMC value composition, whether the IMEM TAG register is written per 256-byte block (the driver SKIPS the tag write when the core is RISC-V — is that skip present in the reference?), the starting tag value passed by the riscv loader, zero padding, the `sec` flag, and the DMEMC value.

A4. Completion check. After start, the driver polls priscv BR_RETCODE (priscv+0x65c) low 2 bits for 3=pass / 2=fail, then polls priscv CPUCTL bit4 for "halted", then requires falcon MAILBOX0 == 0. Quote the reference completion logic for the riscv ACR path (which register/field for BROM result, whether it waits for halt and how, what error/mailbox checks it does, timeouts).

A5. The descriptor handed to ACR. Compare driver `ga10b_fw_fill_acr_desc()` + struct `ga10b_flcn2_acr_desc_t` with the reference struct and the function that fills it for ga10b: field order/sizes, and the values of wpr_region_id, wpr_offset, regions.no_regions, nonwpr_ucode_blob_start/size, ls_pmu_desc, gpu_mode. Also confirm the descriptor's PHYSICAL address is what goes in MAILBOX0/1.

A6. Which falcons are in the blob and with what flags. From `acr_sw_ga10b.c`: the set of LS falcons, and for each its is_lazy_bootstrap and is_priv_load values and which one is conditional on `support_ls_pmu`. Driver: FECS (lazy 0, priv_load 0) + GPCCS (lazy 0, priv_load 1), no PMU.

A7. Blob layout. Compare driver `ls_layout()` / `ga10b_fw_build_ls_blob()` with the reference WPR-requirements + blob-write functions as used for ga10b (PKC / v2-signature variant): size reserved for the WPR header array, the alignment constants, per-image order (LSB header, ucode, BL data), sizes, the extra reserve for a next-core PMU image, WPR header fields (bootstrap_owner, lazy_bootstrap, bin_version, status), and the terminator.

A8. LSB header fill. Compare driver `fill_lsb()` with the reference static LSB fill for (i) FECS/GPCCS and (ii) the RISC-V next-core PMU: ucode_size, data_size, bl_code_size, bl_imem_off, app_code_off/size, app_data_off/size, flags.

A9. Bootloader DMEM descriptor. Compare driver `fill_bl_desc()` + struct `ga10b_flcn_bl_dmem_desc_t` with the reference: address computation (is the WPR base added?), every field, and specifically what argc/argv are for FECS/GPCCS (driver sets argc=1 for all).

A10. FECS/GPCCS image descriptor. Compare driver `ga10b_fw_ctxsw_image_desc()` (bootloader_size, image_size, app_size, app_start_offset, app_resident_code/data offset+size; FECS vs GPCCS differences) against the reference functions that build the fecs and gpccs ucode details, and the surface layout (boot/code/data offsets) against the gr_falcon ctxsw ucode segment init.

A11. WPR address read. Compare driver `ga10b_wpr_read()` (reads 0x1fa81c/0x1fa820, value = (reg>>4)&0xfffffff, << 12; size = end - start) with the wired fb read_wpr_info.

A12. PMU. On SECURED silicon with next-core (RISC-V) PMU, quote the reference PMU RTOS boot path in `pmu_rtos_init.c` and the hooks it calls for ga10b: does the host program the BROM/BCR DMA addresses itself (and pointing where — sysmem image or WPR), or is the PMU image placed in WPR by ACR via the LS blob? What is the gating flag for skipping PMU entirely, and is there anything else in the GR bring-up path that hard-depends on the PMU RTOS having booted? Driver `ga10b_pmu_boot()` programs BCR FMC code/data/manifest addresses pointing into the sysmem image (>>8), sets DMACFG 0x80000002, BCR_CTRL 0x111, starts the core and waits for BR_RETCODE pass + hwcfg2 bit13 clear — say whether this matches the secure path or only the non-secure one.

## Output
Start with a short ranked summary of the most severe mismatches (ones that would make the secure boot fail on hardware), then A1..A12. Terse, evidence-first, under 1300 words. Say CANNOT-DETERMINE rather than guess.
````

## subagent 返回结果（原文）

# ga10bd secure-boot audit against nvgpu rel-36

Nothing was modified. Reference paths are relative to `/tmp/gpu-research/linux-nvgpu/drivers/gpu/nvgpu/`; driver paths are relative to `/Users/tony/work/ewokos/machines/nvidia/orin.nano/system/drivers/ga10bd/`.

## Ranked severe mismatches

1. **A12: `ga10b_pmu_boot()` is the non-secure path.** The host programs the BROM registers with sysmem addresses, which the reference does only when `NVGPU_SEC_PRIVSECURITY` is off. On the fused chip the PMU image must go through the LS blob, and the host only writes args and starts the CPU.
2. **A1: GSP reset uses the wrong mechanism.** The reference resets GSP through the engine reset register `0x1103c0` and then waits for memory scrubbing. The driver pulses CPUCTL hreset and never waits. Whether hreset works on the RISC-V core is CANNOT-DETERMINE; the reference never uses it for GSP.
3. **A8(ii) and A6: adding the PMU to the blob would be wrong as written.** These matter only once the PMU is added. `bl_data_size`/`bl_data_off` must not be zero for the next-core image, and FECS/GPCCS must become lazy.
4. **A7: image order in the blob is reversed.** The reference emits GPCCS before FECS. Whether ACR cares is CANNOT-DETERMINE.
5. **A4: two post-halt error checks are missing.** This is diagnostic only.

## A1 — MISMATCH

- `common/falcon/falcon.c:65-88`: `if (flcn->flcn_engine_dep_ops.reset_eng != NULL) ... reset_eng(g); else g->ops.falcon.reset(flcn);` followed by `nvgpu_falcon_mem_scrub_wait(flcn)`.
- `common/falcon/falcon_sw_ga10b.c:129-151`: `case FALCON_ID_GSPLITE: flcn_eng_dep_ops->reset_eng = g->ops.gsp.gsp_reset;`
- `hal/init/hal_ga10b.c:1364`: `.gsp_reset = ga10b_gsp_engine_reset`.
- `hal/gsp/gsp_ga10b.c:54-66`: `gk20a_writel(g, pgsp_falcon_engine_r(), pgsp_falcon_engine_reset_true_f()); nvgpu_udelay(10); ...reset_false_f()); nvgpu_cg_slcg_gsp_load_enable(g, true);`
- `hw_pgsp_ga10b.h:62-66`: `pgsp_falcon_engine_r() 0x001103c0`, true is 1, false is 0.
- Scrub wait (`falcon.c:149-185`, `falcon_ga10b_fusa.c:196-208`): poll HWCFG2 `0x1100f4` bit 12 every 10 us for at most 1000 us, else `-ETIMEDOUT`.
- Driver `ga10b_falcon.c:30-34` writes IRQMCLR, then CPUCTL `1<<3`, then 0, with no scrub wait.

Fix: write `0x1103c0 = 1`, delay 10 us, write 0, then poll `0x1100f4` bit 12 until clear and fail on timeout. The IRQMCLR write is not in the reference reset path. The driver has no define for `0x1103c0`.

## A2 — MATCH (apart from A1)

`common/riscv/riscv.c:72-108` runs reset, mailbox0/1, `set_bcr`, `get_mem_size(DMEM)`, IMEM text at 0 with `sec=true`, DMEM data at 0, manifest at `dmem_size - manifest_fw->size`, then `bootstrap(flcn, 0x0)`. The driver order is identical.

- **(a)** `falcon_ga10b_fusa.c:54-57`: `nvgpu_riscv_writel(flcn, priscv_priscv_bcr_ctrl_r(), 0x11)`.
- **(b)** `falcon_gk20a_fusa.c:98-115`: `falcon_falcon_hwcfg_dmem_size_v(hwcfg_val) << FALCON_DMEM_BLKSIZE2` (8). The driver's `((hwcfg>>9)&0x1ff)<<8` is the same.
- **(c)** `falcon_ga10b_fusa.c:59-85`: boot vector is written only `if (boot_vector != 0U)`, then `priscv_priscv_cpuctl_r()` = startcpu.

## A3 — MATCH

- The tag skip is in the reference. `falcon_gk20a_fusa.c:324-341`: `if (!flcn->is_falcon2_enabled) { ... writel(imemt_r(port), tag) ... } else {` IMEMD writes only.
- IMEMC is `offs_f(dst >> 2) | imemc_blk_field(blk) | aincw_f(1) | secure_f(sec ? 1U : 0U)`, and the call passes `sec=true`, tag `0x0`.
- Zero padding is `while ((i % 64U) != 0U)` (343-348).
- DMEMC is `dst | falcon_falcon_dmemc_aincw_f(1)` (173-222).
- Omitted in the driver: the DMEMC read-back check (`-EIO`) and the bounds checks at `falcon.c:239-276`.

## A4 — MATCH on core checks

- `falcon.c:187-236` checks `check_brom_passed`/`check_brom_failed` with a 100 ms timeout and 5 ms poll (`falcon.h:151-152`).
- `common/acr/acr_bootstrap.c:108-150` runs `nvgpu_falcon_wait_for_halt` (100 ms, `acr_priv.h:113`), then `acr_engine_bus_err_status`, then `acr_validate_mem_integrity`, then fails on `MAILBOX_0 != 0` with MAILBOX_1 as the error type.
- Halt is priscv CPUCTL `halted` (`falcon_ga10b_fusa.c:43-52`).

The driver lacks two checks:
- the PMU BAR0 error status (`gv11b_pmu_bar0_error_status`, `hal/pmu/pmu_gv11b_fusa.c:99`);
- the GSP ECC status at `0x110878`, bits 8, 9 and 13 (`gsp_ga10b.c:115-123`).

It should add both and log MAILBOX1 on failure. The driver's longer timeouts are harmless.

## A5 — MATCH

- `nvgpu_acr_interface.h:752-792`: `wpr_region_id; wpr_offset; regions; nonwpr_ucode_blob_size; u64 nonwpr_ucode_blob_start; u64 ls_pmu_desc; u32 gpu_mode`.
- `common/acr/acr_sw_ga10b.c:307-342`: blob start is the blob address, size is `(u32)g->acr->ucode_blob.size`, `regions.no_regions = 1U`, `wpr_offset = WPR_OFFSET`, and the gpu_mode bits are cleared.
- The blob is allocated with `plsfm->wpr_size` (`acr_blob_construct.c:1450`), so the driver's round-up to 256 changes nothing.
- The descriptor is `NVGPU_DMA_PHYSICALLY_ADDRESSED`, and `acr_bootstrap.c:387-388` passes `nvgpu_mem_get_addr(...)` to the mailboxes.

## A6 — MATCH for `support_ls_pmu == false` only

`acr_sw_ga10b.c`:
- 386-393, PMU_NEXT_CORE: `if (!g->support_ls_pmu) return 0;`, lazy false, priv false.
- 417-421, FECS: `is_lazy_bootstrap = g->support_ls_pmu ? true : false;`, priv_load true only with `NVGPU_SUPPORT_MIG`.
- 443-444, GPCCS: same lazy rule, `is_priv_load = true`.

The reference Linux build runs with it true: `os/linux/driver_common.c:373` uses `support_gk20a_pmu()`, and `Makefile.linux.configs:9` has `CONFIG_GK20A_PMU := y`. The driver's no-PMU variant is the one described in the comment at 409-416 ("In absence of LSPMU, ACR will bootstrap LS falcons").

## A7 — MISMATCH (order); the rest MATCH

- Order: `acr_blob_construct.c:729-730` prepends (`pnode->next = plsfm->ucode_img_list`), and discovery runs in ascending id (795-803). Headers and offsets therefore come out in descending id: PMU_NEXT_CORE, GPCCS, FECS. The driver emits FECS then GPCCS and should emit GPCCS first.
- Matching:
  - header array `sizeof(struct lsf_wpr_header) * (flcn_cnt + 1)`;
  - alignment 256 for LSB, 4096 for ucode, 256 for BL data (919-945);
  - `PMU_NVRISCV_WPR_RSVD_BYTES (0x8000)` (`acr_blob_construct.h:49`, used at 1019-1022);
  - `bootstrap_owner`, `LSF_IMAGE_STATUS_COPY`, lazy, `bin_version = ...ls_ucode_version` (712-727);
  - terminator `FALCON_ID_INVALID` (1321-1327).

## A8 — (i) MATCH, (ii) MISMATCH

- **(i)** FECS/GPCCS agree field for field with `acr_blob_construct.c:607-646` and 947-957.
- **(ii)** The static fill zeroes `bl_data_size`/`bl_data_off` (673-692), but `lsf_calc_wpr_size_pkc` runs afterwards for every image and sets `bl_data_size = NVGPU_ALIGN(sizeof(pnode->bl_gen_desc), 256)` and `bl_data_off = wpr_offset` (935-942). The driver's next-core branch re-zeroes both (`ga10b_fw_parse.c`, `fill_lsb`). It should keep the layout values and still write no BL descriptor (`if (!pnode->ucode_img.is_next_core_img)`, 1282).

## A9 — MATCH

- `acr_blob_construct.c:1087`: `addr_base = nvgpu_safe_add_u64(addr_base, wpr_inf.wpr_base)`.
- `addr_code = base + app_start_offset`, `addr_data = addr_code + app_resident_data_offset`.
- `flcn64_set_dma` stores raw lo/hi (`include/nvgpu/flcnif_cmn.h:36-40`).
- `ctx_dma = falcon_dma_idx`, where `GK20A_PMU_DMAIDX_UCODE` is `U32(0)` (`pmu.h:193`).
- `argc = UCODE_PARAMS`, which is 1 because the Linux build defines `CONFIG_NVGPU_LS_PMU` (`Makefile:92`, `acr_blob_construct.h:40-41`).
- argv is untouched for FECS/GPCCS, since they have no `get_cmd_line_args_offset`.

## A10 — MATCH

- `common/gr/gr_falcon.c:248-274`: `boot_size = NVGPU_ALIGN(bootdesc->size, sizeof(u32))`, and each segment offset is aligned with `NVGPU_ALIGN(ucode_offset, 256U)`.
- FECS then GPCCS share one surface (337-366).
- The boot image is `fw->data + sizeof(struct nvgpu_ctxsw_bootloader_desc)` (16 bytes), and code/data come from the netlist inst/data lists.
- The descriptors at `acr_blob_construct.c:240-267` and 389-426 match the driver's formulas. GPCCS data is the surface plus `gpccs->boot.offset`, which equals the driver's `fd.image_size`.
- Firmware names match (`gr_falcon_ga10b_fusa.c:40-42`, `netlist_ga10b.h:40`).

## A11 — MATCH

- `hal/init/hal_ga10b.c:972`: `.read_wpr_info = ga10b_fb_read_wpr_info`.
- `hal/fb/fb_ga10b_fusa.c:184-204`: `*wpr_size = nvgpu_safe_sub_u64(wpr_end, wpr_start)`.
- `hw_fb_ga10b.h:305-310`: `0x001fa81c`/`0x001fa820`, `(((r) >> 4U) & 0xfffffffU)`, alignment `0xc`.

## A12 — MISMATCH (driver matches only the non-secure path)

- **Gate.** `common/pmu/pmu_rtos_init.c:375`: `if (!g->support_ls_pmu || (g->pmu == NULL)) goto exit;`
- **Secure next-core path** (384-455, 476-488):
  - load PMU SLCG/BLCG;
  - `nvgpu_pmu_next_core_rtos_args_setup` (261-305), which puts boot args at `EXTMEM2 + phys` in MAILBOX0/1 and sets `ctx_bind_addr`;
  - `nvgpu_pmu_enable_irq`;
  - `g->ops.falcon.bootstrap(g->pmu->flcn, 0U)`;
  - wait for BROM completion, then for priv-lockdown release.
- **No host BROM programming.** There is no `brom_config` on this path, and `hal_ga10b.c:1411-1412` says "aperture set up is moved to acr".
- **Where the host does program it.** Only the `else` branch (466, `nvgpu_pmu_ns_fw_bootstrap`) reaches `ga10b_pmu_ns_nvriscv_bootstrap` (`hal/pmu/pmu_ga10b.c:192-221`), which is what driver `ga10b_pmu.c:151-182` replicates.
- **How ACR gets the image.** Through the LS blob: the whole image file with `falcon_id = FALCON_ID_PMU_NEXT_CORE` (`acr_blob_construct.c:92-147`), plus `ls_pmu_desc` set to the physical address of a copy of the desc file (`acr_sw_ga10b.c:284-302`).
- **GR dependency.** `gr_falcon.c:588-597` uses the LS PMU only `if (g->support_ls_pmu)`; otherwise it takes "bootstrap by ACR" (662-666) and then `start_gpccs`/`start_fecs`. I found no hard PMU-RTOS dependency in the files read; the rest of GR was not audited.

Fix: do not call `ga10b_pmu_boot()` on fused silicon, and keep the no-PMU blob. A secure PMU would need the PMU in the blob, `ls_pmu_desc`, lazy FECS/GPCCS, CPU start without `brom_config`, and PMU-RTOS LSFM commands to boot FECS/GPCCS.
