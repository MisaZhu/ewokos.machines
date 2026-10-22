# Q8B GPU driver handover

## Latest state (2026-10-22, default g2dd desktop built and packaged)

User requested default system rendering through g2dd, compilation and commit.
This authorizes the full `x` desktop build, superseding the earlier gui-only
build restriction for this work. Both graphical-console and desktop init
scripts now start UART first, then synchronously run shared `/etc/g2d.rd`.
It sets `ADRENO_ENABLE=1` and starts one g2dd through `ipcserv`, whose
readiness wait completes before display/Graph/desktop clients are launched.
The top-level Q8B `rootfs` target now builds `system x` rather than `gui`.
The official GMU/SQE and board-specific signed zap are packaged unchanged.

Validation: `make -C machines/q8b/kernel`, `make -C machines/q8b/system x`
and `make -C machines/q8b/system sd` succeeded. The 112 MiB ext3 image contains the xwin
init, shared GPU startup script, desktop binaries, daemon and firmware.
Built g2dd/g2dtest/fbdisplayd hashes match the hardware-tested artifacts in
the following section. Build logs are `/tmp/q8b-default-g2dd-x-20261022.log`
and `/tmp/q8b-default-g2dd-sd-20261022.log` on the host; the kernel log is
`/tmp/q8b-default-g2dd-kernel-20261022.log`. The new default
desktop boot has NOT been run on the board. No board daemon was replaced,
no second GPU initialization was performed, and no PXE/storage was changed.
On a normal new-image boot, run `/bin/g2dtest` against the existing
`/dev/g2d`; never launch `/tmp/g2dd` in addition to the init-started daemon.

## Latest state (2026-10-22 20:12, g2dtest/API/display integration all PASS)

User physically power-cycled the board. PXE/EwokOS startup was observed at
19:51:00/19:51:01; serial login/ps at 19:52:46 confirmed no GPU daemon.
Transferred each file separately at 50 Kbit/s and checked board MD5 before
the next file. Exactly one GPU initialization ran at 19:57:51 through serial:
`/bin/bgrun /tmp/g2dd /dev/g2d`. Live driver PID 327, MD5
`100da03f5f41af3b49f1b2dc63c1dac6`. Official GMU/SQE, unchanged signed zap
PAS authentication, secure-mode exit, idle/fault checks and CP selftest passed.
Do not replace or restart this daemon during this boot.

Hardware results against this same instance:

- 19:58:20 `/tmp/gpusmoke-api --api-raster`: PASS. All 18 exact solid-alpha
  cases and 44 exact rotation cases passed through the new BSP/private
  two-buffer kernel path, with zero output/source/guard errors. This now
  validates the production API routes, beyond the earlier shader-only probe.
- 19:58:37 `--extended`: PASS, including changing fills, narrow cropped copy,
  180 rotation and exact uniform compute scale through 720x720, with all
  source pixels and guards preserved.
- 19:58:54..56 `--apis`: full PASS. Includes 96 independent NEON alpha cases,
  the new solid-alpha/rotation matrix, exact GPU global alpha/scaling,
  physical pitch/bounds/no-write declines, and public Graph integration.
- 19:59:22..20:00:19 `/tmp/g2dtest`: **PASS (0 failure)**. All original
  assertions and benchmarks retained. Former fractional fill, 90/45 rotation,
  dependent readbacks and performance failures are resolved. Includes exact
  1920x1080 rotated data and 1415x1415 45-degree content checks. All four
  performance groups through 1920x1080 and break-even tables completed.
  Existing Gaussian capability skips remain explicit: radii 1..4 and 6
  decline; exact software Gaussian is separately checked by Graph integration.
- 20:04:04 `/tmp/g2dscanout`: PASS on owned RAM at 64x64/pitch320 and
  128x128/pitch640, origin (16,1). Full/cropped writes, 90/180/270 rotations,
  forced CPU paths, invalid rectangles, source/padding/prefix/suffix/guards
  all matched exactly. Physical blit/rotate counter deltas were 2/3 and 4/3.
- 20:11:08 `/tmp/g2dpresent /dev/disp1`: PASS through public Graph, display
  IPC, new fbdisplayd, same g2dd and the real native 720x720 BGR32 GOP at
  PA `0xc6200000`, pitch2880. Two full frames, a dirty client update and
  original-desktop restoration each matched every GOP pixel exactly.
  Counter deltas were fill=5, blit=4, blit_phy=4. The daemon's GOP copy is
  full-frame even for a dirty client update. This proves GOP memory output,
  not panel appearance, tear-free presentation or a full rebuilt x desktop.

Validated artifacts, all in board RAM:

| artifact | MD5 |
|---|---|
| `/tmp/g2dd` | `100da03f5f41af3b49f1b2dc63c1dac6` |
| `/tmp/g2dtest` | `df85ed8ce3554b33db4177fecbd13082` |
| `/tmp/gpusmoke-api` | `ddcced87191a8be026a173e4f67ce205` |
| `/tmp/qcdxkmsuc8280.mbn` | `7315a508384872b908323a775a19221f` |
| `/tmp/adreno.cfg` | `515137e989a9602a950c58983cbc2a8d` |
| `/tmp/g2dscanout` | `46c7b336e7e0f4210066ad4a49d969c2` |
| `/tmp/fbdisplayd-gputest` | `c3217b229d6b2afff71214c6c13f2815`, launched at 20:07:49 as PID 157 on `/dev/disp1` |
| `/tmp/g2dpresent` | `cce93fdc2d86236b175067426a3ecabe` |

Full g2dtest transcript `/tmp/q8b-raster-g2dtest-full-20261022.log`; full API
transcript `/tmp/q8b-raster-allapis-full-20261022.log`; initialization log
`/tmp/q8b-raster-coldboot-init-20261022.log`. These are host-side files.
Serial ps at 20:01:57 confirmed PID 327 sleeping and the original desktop
intact, CPUs 97-99% idle. Display-library and real-GOP reruns both passed.
Additional logs `/tmp/q8b-raster-scanout-20261022.log` and
`/tmp/q8b-raster-present-20261022.log`. No second GPU init, firmware,
kernel/PXE changes or board storage flashing occurred. Full rebuilt x
desktop remains outside the approved gui target; geometry remains deferred.

At 20:06:30/37 serial MD5 also confirmed the actual loaded GMU/SQE files
match the official repository copies: `cd3a235218418f51360888637c1cb2ac`
and `aef1fcec7a927d0ceb2bc5d8903eb298`. The new test display only maps GOP
PA `0xc6200000`, native 720x720 BGR32, pitch2880; it does not initialize GPU.
The first g2dpresent transfer timed out with mismatched MD5; it was never
executed. At 20:08:55..20:09:40 the known serial net/netd/sshd recovery was
used, preserving GPU PID 327 and both display daemons. The retry completed
with the expected MD5 before the successful GOP run.

Final serial ps at 20:11:40: clients exited normally, same g2dd PID 327 and
test display PID 157 sleeping, original desktop intact, CPUs 97-99% idle.
EUD has no transition since 19:52:28; no new EDL/reset event. Whitespace
checks passed. Kernel MD5 remains `f86d304c1045c5d95b13ce7a19b8c514`.
All new board artifacts are RAM-only; no PXE image/storage was changed.
This completes the reported g2dtest failures and same-instance 2D/display
regressions. Keep the current GPU daemon running for further client work.
Full rebuilt desktop and geometry are separate pending work, as noted above.

## Latest state (2026-10-22 19:48, g2dtest fixes built; raster payload hardware PASS)

User reran `/bin/g2dtest` at 19:26:25; it ended at 19:27:10 with 31 failures.
These are missing fractional solid fill, 90-degree and 45-degree rotation,
including dependent readbacks/benchmarks. Existing supported operations passed.
The resolution is to implement those GPU operations, not weaken the test or
relabel its failures as acceptance. Original g2dtest assertions are unchanged.

Implemented, built, but new daemon NOT deployed:

- New ordinary `raster.comp` implements exact solid source-over and integer
  0/90/180/270 mappings plus arbitrary-angle nearest Q14 rotation. Twelve
  uniform words, 8x8x1 groups, two SSBOs, no private memory. The inverse map
  combines the CPU's two 64-bit shifts into bounded 32-bit arithmetic. This is
  application GLSL compiled by the existing compiler; no firmware changes.
- `adreno_3d.c` has a separate private raster kernel/lock, using the existing
  validated dispatch path. Checks dimensions/rects/physical overflow and
  rejects overlapping rotation allocations before submission. Fractional
  solid fill touches only each invocation's own destination pixel. Existing
  validated blit.comp/blob and GPU initialization are unchanged.
- BSP routes fractional fill and non-A2D canvas rotation to compute. CPU math
  only calculates Q14 coefficients/geometry. Raw physical 90/270, triangles
  and exact Gaussian still decline without writes for public client fallback.
- `gpusmoke --api-raster` adds 18 exact alpha cases and 44 rotation cases,
  with source preservation and full guards. `--apis` includes them. The
  rotation oracle retains the independent original CPU 64-bit formula.
  g2dtest now parks on submitted faults/timeouts before buffers can be freed;
  it otherwise retains all original functional assertions and benchmarks.

Actual hardware evidence on the SAME old daemon PID 245:

- At 19:45:57 serial `/tmp/gpushader-raster /tmp/raster-probe.bin --raster`
  uploaded handle 0 and passed all 18 solid-alpha and 44 rotation cases.
  Sizes 64x48, 65x37, 1x1, 320x240; angles 0/90/180/270/45/135/1/89/91/179/315.
  Every pixel, source row, padding, prefix/suffix and 16 KiB guard matched.
- The probe is compiled from the same raster source with destination binding
  0, to use the old public fullscreen path. The reproduction script proves
  exactly one binding-metadata byte differs; ALL application instructions
  and constants are identical to the embedded production raster program.
  It binds both descriptors to disjoint rows in one owned allocation.
  This proves shader arithmetic/bounds, NOT the new private two-buffer/BSP
  route, full g2dtest, large-surface benchmarks or full desktop integration.
- The first 2680-byte shader transfer stalled despite `scp -l 50`; its board
  MD5 did not match, and it was never executed. Host transfer was stopped;
  serial cleared scp 461 and restarted only net/netd/sshd at 19:44-19:45.
  Retry and board MD5 passed before executing. No GPU restart occurred.

Artifacts from successful prescribed gui build:

| artifact | MD5 |
|---|---|
| new `machines/q8b/system/build_aarch64/drivers/q8b/g2dd` | `100da03f5f41af3b49f1b2dc63c1dac6`, NOT deployed |
| `machines/q8b/system/build_aarch64/bin/g2dtest` | `df85ed8ce3554b33db4177fecbd13082`, NOT deployed |
| `system/build_aarch64/q8b/bin/gpusmoke` | `ddcced87191a8be026a173e4f67ce205`, new API matrix NOT deployed |
| `system/build_aarch64/q8b/bin/gpushader` / board `/tmp/gpushader-raster` | `4f1e162c0f14d22e62fc2c2328dc5278`, raster probe hardware tested |
| `/tmp/q8b-raster-probe-checked.bin` / board `/tmp/raster-probe.bin` | `4db7c4ea6e043b8cfc099d1b311e7473`, hardware tested |

Build log `/tmp/q8b-gpu-20261022-raster-probe-gui.log`; no new warnings in
changed GPU/BSP/test code. `check_2d_shader.sh` passed both shader reflections,
byte-for-byte header reproduction, probe payload equivalence and negative
texture rejection. Production raster blob SHA256
`41d292dc8ce506e5b0846215d1e68e6841ff7809035fe599362f245dff9114e4`.
Kernel MD5 unchanged `f86d304c1045c5d95b13ce7a19b8c514`. No firmware,
kernel/PXE changes, second GPU initialization or board storage flashing.

Serial ps at 19:48:29 confirmed the probe exited normally; PID 245 and test
display PID 510 are still sleeping, original desktop intact, CPUs 98-99%
idle. EUD has no transition since 18:54:11, so no new EDL/reset event.
Scoped whitespace checks passed; the original g2dtest diff contains only the
fault-retention wrappers, no weakened assertions or capability skips.

Next: physical off/on is required before changing the initialized daemon.
Transfer new driver, g2dtest, gpusmoke, exact official
zap/config individually at 50 Kbit/s with MD5 checks; launch once through
serial. Run --api-raster, --extended, full --apis, original complete g2dtest
and display integration against the SAME instance. Stop on unexpected error.
Do not claim the original g2dtest passes yet. Full rebuilt x desktop remains
pending and its `x` build target has not been approved. Geometry is deferred.

## Latest state (2026-10-22 19:25, 2D API/Graph/real GOP integration PASS)

User priority remains remaining g2dd APIs and integration, before geometry.
Physical boot at 18:54:17; exactly one initialization at 18:57:45 through
serial: `/bin/bgrun /tmp/g2dd /dev/g2d`. Live driver PID 245, MD5
`721eb36107c5c3e4bd1f55967efce8c4`. Official GMU/SQE, unchanged signed zap
authentication, idle/fault checks, secure-mode exit and CP selftest passed.
Do not replace/restart this live daemon in the current boot.

Hardware evidence against that same instance:

- 18:57:57 `/tmp/gpusmoke-api --extended`: PASS, including all pixels/guards
  through 720x720, compute uniform scales and narrow cropped copies with no
  CPU tail. The readonly-SSBO LDIB compiler correction is now hardware proven.
- 18:59:42 alpha and 18:59:56 scale groups independently passed with zero
  tolerance. An earlier full API run stopped at identity rotation rejection;
  Graph integration then exposed a one-channel CPU NEON rounding discrepancy.
- 19:04:23 `/tmp/gpusmoke-client --apis`: full PASS after client/NEON fixes.
  Includes 96 CPU alpha cases, all 12 exact GPU global/destination-alpha
  groups, identity/normalized rotations, Q15 endpoint scales and rational
  cropped BLITs, physical pitches/bounds, no-write declines, public Graph
  acceleration and exact CPU fallback. No pixel/guard errors.
- 19:11:00 first display-library test: all pixels/guards passed, but its
  counter expectation was wrong for a 64x64 non-contiguous intermediate.
  No GPU fault. Production graph allocation policy was not changed.
- 19:17:34 `/tmp/g2dscanout-threshold`: PASS at both 64x64/pitch320 and
  128x128/pitch640, origin (16,1). Full/cropped pushes, 90/180/270 rotations,
  forced CPU copies, invalid-rect no-write, source/row-padding/prefix/suffix/
  16 KiB guards all passed. blit_phy/rotate_phy deltas were exactly 2/3 for
  64x64 and 4/3 for 128x128. This test writes owned RAM, not the real GOP.
- 19:24:12 current fbdisplayd started successfully from RAM at `/dev/disp1`
  with display index 1, independently of the existing desktop. PID 510, MD5
  `c3217b229d6b2afff71214c6c13f2815`. It only maps the existing GOP; it does
  not initialize the GPU. The original `/dev/disp0` remains PID 67.
- 19:24:24 `/tmp/g2dpresent-final /dev/disp1`: PASS. Actual public Graph ->
  display IPC -> new fbdisplayd -> same live g2dd -> native 720x720 BGR32 GOP
  at PA `0xc6200000`, pitch 2880. Two full presentations, dirty-rect update
  and original-desktop restoration each had zero errors across every GOP
  pixel. Counter deltas exactly fill=5, blit=4, blit_phy=4, proving both
  client rendering and physical scan-out copies reached the GPU. Dirty
  client sync uses a partial canvas blit; Q8B's current display daemon has
  no dirty hook, so its output copy is full-frame. Panel appearance still
  needs human observation; GOP memory readback does not prove a tear-free
  presentation or a complete rebuilt desktop startup.

Fixes validated above: g2dclient routes identity rotations through opaque
BLIT; actual AArch64 NEON alpha matches the scalar `(src_a * global) >> 8`
contract, opaque source-copy and zero-effective-alpha no-op. Displayd converts
graph rotation enums to protocol degrees; CPU flush/rotation copies preserve
physical pitch and apply the visible origin once; invalid rectangles decline
before writes; the 32bpp rect path now attempts physical GPU copy.

The preinstalled generic `/bin/g2dtest` also ran at 19:08:05: summary FAIL
(31). Those failures are its unconditional expectations/benchmarks for
fractional solid fill and 90/45-degree GPU rotation, which Q8B deliberately
declines; dependent readbacks then fail. Supported opaque fill, blit, alpha,
180 rotation and scaling passed. Its assumptions are not the Q8B acceptance
contract; see README.g2d.md. Do not claim those unsupported features accelerated.
Triangles, exact Gaussian, fractional solid fill and 90/270 rotation continue
through software fallback at the public Graph layer.

Artifacts:

| artifact | MD5 / status |
|---|---|
| live `/tmp/g2dd` | `721eb36107c5c3e4bd1f55967efce8c4`, hardware tested |
| host built `g2dd` | `d7181625ce71a98b4b769d7c2895543f`, relinked with updated shared libraries, NOT deployed |
| `gpusmoke` / board `/tmp/gpusmoke-client` | `aa968196e86fea758354df1340a3124c`, hardware tested |
| `g2dscanout` / board `/tmp/g2dscanout-threshold` | `46c7b336e7e0f4210066ad4a49d969c2`, hardware tested |
| `fbdisplayd` / board `/tmp/fbdisplayd-gputest` | `c3217b229d6b2afff71214c6c13f2815`, startup and real-GOP integration tested |
| `g2dpresent` / board `/tmp/g2dpresent-final` | `cce93fdc2d86236b175067426a3ecabe`, hardware tested |

Prescribed gui builds passed, latest log
`/tmp/q8b-gpu-20261022-real-presentation-final-gui.log`. Host graph routing, fault
parking and shader reproduction/negative sampler rejection checks passed.
Embedded blit application SHA256 remains
`59238841a22537736589ec9bb9b034aef95e6d527661ceb99523aa1c7192695e`.
Kernel MD5 unchanged `f86d304c1045c5d95b13ce7a19b8c514`; no kernel/PXE/firmware
edits, second GPU initialization or board storage writes. Serial ps at
19:25:00 showed healthy sleeping PID 245 and test display PID 510, CPUs about
99% idle. EUD still has no transition since 18:54:11; no new EDL event.

Next: full updated desktop startup is still pending. Existing `/dev/disp0`,
xserverd and desktop processes remain running with old statically linked
libraries. The approved gui target does not rebuild xserverd/xwm (`x` does),
so expand the build scope explicitly before rebuilding the complete desktop
and preparing its RAM-only PXE rootfs. Do not flash storage or relaunch the
current GPU. Test display PID 510 is idle and may be reused with the same
client while the original desktop is idle. Its display-manager slot is
temporary until reboot. Geometry remains deferred; regenerate readonly
geometry blobs before later use.

## Latest state (2026-10-22 18:52, readonly SSBO compiler fix ready)

User confirmed physical off/on; EwokOS booted at 18:39:03. SSH and serial ps
confirmed no g2dd. Uploaded driver/client/zap/config individually at 50 Kbit/s,
checking each board MD5 against the 18:36 table below. Serial root login
succeeded. Exactly one initialization ran at 18:46:46:
`/bin/bgrun /tmp/g2dd /dev/g2d`, live driver MD5
`651ca82fcd0c83536b3959b08cb314a0`, PID 336. Official GMU/SQE, signed zap
authentication, idle/fault checks, secure-mode exit and CP selftest passed.

At 18:46:57 `gpusmoke-api --extended` passed clear, subrect fill, copy,
eight repeated fills, cropped copy without a CPU tail, rotate180 and source
preservation at 64x64 (zero pixel/guard errors). Its first compute uniform
scale 64x64 -> 32x32 returned success but FAILED all 1024 pixels: output
00000000 vs ffa17c39, zero guard errors. The suite stopped there, so the
320x240/720x720 cases and full --apis have NOT passed with this driver.

Root cause is the compiler/UMD descriptor contract, not firmware: Mesa's
`has_isam_ssbo` defaults true on A690 and compiles reorderable readonly SSBO
loads as texture reads. This UMD only binds UAV descriptors and sets NTEX=0;
the blob carries no texture mapping. Adding a snapshot guard and recompiling
the failing blit.comp reproduced the incompatibility on the host exactly:
`texture descriptors are not supported (samplers 1, texture slots 1)`.

Fix: adreno_backend.c sets `has_isam_ssbo=false` after compiler creation,
using Mesa's ordinary LDIB SSBO path, and rejects any variant still requiring
sampler/texture descriptors. Regenerated embedded blit blob remains 2928 bytes,
binary 2432, 8x8x1, two SSBOs, twelve uniform words, no private memory; SHA256
`59238841a22537736589ec9bb9b034aef95e6d527661ceb99523aa1c7192695e`.
The final host shader check passed reflection/byte-for-byte reproduction plus
negative sampler rejection (`cs_texture_unsupported.comp`). Previously hardware
tested cs_smoke/cs_uniforms binaries are byte-identical after this compiler fix.
Old geometry blobs with readonly buffers MUST be regenerated before use.

After diagnosing the ordinary pixel failure (no timeout/MMU fault), the
independent A2D-only `--api-physical` suite ran at 18:49:37 against the SAME
daemon: cropped copy, cropped rotate180, padded 320-byte pitch, prefix/suffix,
source preservation, short-size/bad-pitch rejection, and 90/270 no-write decline
all PASS with zero pixel/guard errors. No further compute submissions ran.
Serial ps at 18:51:23 showed PID 336 sleeping, healthy OS, CPUs 97-99% idle;
EUD had no new transition since 18:38:48 and no new EDL event.

Corrected driver is built, NOT deployed or hardware-validated:

| artifact | MD5 |
|---|---|
| `machines/q8b/system/build_aarch64/drivers/q8b/g2dd` | `721eb36107c5c3e4bd1f55967efce8c4` |
| `system/build_aarch64/q8b/bin/gpusmoke` (unchanged client) | `5a18ce145d38e6a381218d1c92640256` |

Prescribed `make -C machines/q8b/system gui` passed; log
`/tmp/q8b-gpu-20261022-api-uav-gui.log`. Kernel MD5 remains
`f86d304c1045c5d95b13ce7a19b8c514`; no kernel/PXE/firmware changes or board
storage writes. Existing register-macro redefinition warnings remain.

Next: physical off/on before replacing the initialized daemon. Then transfer
corrected driver, unchanged client, exact signed zap and config one at a time,
checking MD5 before the next transfer; initialize once through serial; run
--extended then --apis. New LDIB source reads, exact alpha/scale and full GUI
startup/scan-out integration remain pending. Do not relaunch the current GPU.

## Latest state (2026-10-22 18:36, g2dd API fixes built; cold boot needed)

User priority is remaining g2dd API tests/integration, before geometry. A new
physical boot was observed at 17:57:17. SSH recovered. Serial login and ps
confirmed no g2dd before testing. Files were uploaded individually at 50 Kbit/s
and board MD5-verified. Exactly one initialization ran at 18:12:19:
`/bin/bgrun /tmp/g2dd /dev/g2d`, live driver MD5
`bb62a3722140fd2ea663b9167143dde0`. Official GMU/SQE, signed zap PAS auth,
secure-mode exit, idle/fault checks and CP selftest passed. Serial ps at
18:32:10 showed this same driver PID 496 sleeping, OS responsive, CPU idle.
No GPU restart/replacement, firmware edit, kernel/PXE update or storage write.

Hardware API results on that live instance:

- 18:12:36 `gpusmoke-api --extended`: prior fill/copy/rotate180/uniform-scale
  readback and guards passed through 720x720. Note: the old shared daemon can
  CPU-copy sub-16-pixel tails of cropped BLITs; this was discovered and removed
  in the final Q8B build. The earlier cropped-copy results do not prove that
  every pixel was GPU-written. Physical-destination APIs do not use that split.
- 18:22:09 `gpusmoke-api3 --apis` (client MD5
  `961a243d3b2b0cda8f233a4c1dbb560e`): clipped/transparent fill, fractional
  fill decline, undersized canvas rejection, triangle decline/transparent
  no-op, transparent blit no-op, global-alpha decline, opaque-destination
  source-over (3-channel tolerance), rotate90/270 decline, Gaussian radii
  1..4/6 decline, unchanged scratch, and opaque fill after declines all passed.
  Source-over passed only the opaque destination case here.
- 18:16:02 and 18:22:09 physical copy and cropped rotate180 passed with
  nonaligned crop coordinates, 320-byte pitch, prefix/padding/suffix and all
  guards unchanged. Short dst size and rotate90/270 correctly declined with
  no writes. Targets were owned contiguous RAM, never real framebuffer/MMIO.
- Graph integration at 18:16:15 and 18:22:10 passed full-canvas/guard checks:
  opaque fill, fractional fill fallback, triangle fallback, global-alpha
  fallback, 90/180/270 rotation, exact Gaussian fallback.
- Nonuniform SCALE_TO FAILED: 64x64 -> 32x37 nearest A2D returned success but
  mismatched 1161 pixels, first (0,0)=ff030501 vs ff000000, zero guard errors.
  Changing the filter alone does not fix A2D pixel-centre coordinate mapping.
- 18:27:26 `gpusmoke-api4 --api-alpha` (client MD5
  `2b08247e6fcfa934d84a4caa19ec6db8`): source-over opaque and partial dst passed
  at tolerance 3, but transparent dst FAILED (1367 pixels, zero guard errors).
  A2D does not preserve straight source RGB for empty destinations.

Fixes now built, NOT deployed/initialized on the current boot:

1. Exact nearest scaling and straight-alpha blending use the ordinary GLSL
   `system/libs/adreno/shaders/blit.comp`, compiled through the existing shared
   GLSL/NIR/ir3 compiler. Two SSBOs, 8x8 groups, 12 uniform words (offsets 0/4/8),
   no private memory. Embedded application blob 2928 bytes, SHA256
   `eee19eaa9769d8c004423c94618565cbb0fc2cc78ca9aaa6e9c17ac9235bcc2f`.
   This is application rendering code, not firmware disassembly/patching.
   A private kernel slot preserves all eight app program handles. Overlapping
   allocations decline before submission. Kernel/descriptor buffers are
   retained after terminal submitted failures.
2. SCALE_TO computes the exact Q15 corner-preserving reference; scaled BLIT
   uses rational origin-based nearest. Blend supports fractional global alpha
   and matches arch_g2d's transparent-dst, opaque-copy and /255 rounding rules.
   These new two-buffer dispatches still need hardware validation.
3. Shared graph now offloads opaque fill, requires BOTH scale canvases to be
   eligible, and declines nonidentity graph_scale_tof offload because graph's
   bilinear/factor semantics differ from nearest SCALE_TO. Submitted timeout/
   MMU-fault results park graph/display callers until reboot, preserving
   buffers instead of racing a CPU replay. No hardware faults were injected.
4. Transparent BLIT returns no-op success in client and daemon. Q8B builds
   g2dd with G2D_PITCH_ALIGN=4 so whole crops reach the BSP without a CPU tail.
   Built g2dd.o has no arch_g2d_* pixel imports. Static math link order fixed.
5. Gaussian mip approximation no longer masquerades as exact Q16 Gaussian:
   return NOT_SUPPORTED with no writes; graph uses its exact software fallback.
   Fractional solid fill, triangles and 90/270 GPU rotation remain unsupported.
6. Exact official signed zap is now packaged in the built rootfs at
   `/etc/firmware/adreno/sc8280xp/LENOVO/21BX/qcdxkmsuc8280.mbn`, verified MD5
   `7315a508384872b908323a775a19221f`. Normal startup need not depend on /tmp.

Ready artifacts (prescribed final gui build passed):

| artifact | MD5 |
|---|---|
| `machines/q8b/system/build_aarch64/drivers/q8b/g2dd` | `651ca82fcd0c83536b3959b08cb314a0` |
| `system/build_aarch64/q8b/bin/gpusmoke` | `5a18ce145d38e6a381218d1c92640256` |

Final log `/tmp/q8b-gpu-20261022-api-final-gui.log`. Host regressions passed:
actual graph routing (opaque IPC, replayable decline, independent sticky cap,
both scale buffers), actual g2dclient fault parking, and byte-for-byte shader
reproduction/reflection. See README.g2d.md for exact commands and full matrix.
Final --apis expects the new compute blend, exact zero-tolerance alpha results,
Q15 up/down/odd-width/one-row scaling, normalized rotations, cropped rational
BLITs and physical bad-pitch rejection. Do not run this new acceptance suite
against the old live driver and call the resulting expected failures a regression.

Next: user has been asked for physical off/on before driver replacement.
After confirmation, verify boot/no g2dd, transfer final driver, client, exact
signed zap and adreno.cfg individually at 50 Kbit/s, verifying each board MD5.
Launch once from serial, run --extended then --apis, stopping on first error.
Do not claim the new scale/blend kernel, updated no-CPU-tail crops, new rotation
normalization cases or final zero-tolerance alpha matrix hardware-passed yet.
Live GUI/display processes still use their earlier statically linked libraries;
graph test clients verify updated integration in isolation. Actual full GUI
startup/scan-out acceptance remains pending. Geometry remains untested and
deprioritized. Root machines conflict and projects gitlinks are untouched.

## Latest state (2026-10-22 17:47, geometry built; deployment blocked)

The uniform/stride/CS-A2D results below remain hardware-verified. The GPU
has NOT run any geometry dispatch in this boot. At 17:46:59 serial ps
responded, with the original `/tmp/g2dd` PID 300 sleeping. EUD reported no
transition/reset since 17:03:59. Kernel, PXE and all official firmware
remain unchanged; no storage flashing or second GPU initialization.

The geometry client and unchanged shared xform/raster kernels are ready
with the 17:26 artifact identities below. Prescribed gui builds passed:
`/tmp/q8b-gpu-20261022-geometry-gui.log` and
`/tmp/q8b-gpu-20261022-geometry-merge-gui.log`. The latter briefly included
a temporary transfer helper; that unused helper was removed from source
and the Makefile, so it is not part of the final implementation.
Final prescribed build after removing that helper also passed:
`/tmp/q8b-gpu-20261022-geometry-final-gui.log`; whitespace checks passed.

Deployment hit two full-file scp timeouts despite `-l 50`. Network-only
recovery restored ping but left an orphan sshd PID 119 consuming CPU;
attempting to kill it returned an error. Current network processes are
net PID 332, netd PID 340 and sshd PID 398. New SSH sessions now repeatedly
close before authentication despite ping working. Do not change the GPU
driver in this boot to address the transfer problem.

All nine 16 KiB-or-smaller client parts `/tmp/gp.00` through `.08` passed
individual board MD5 checks. They were split from the 131408-byte client
MD5 `9f93a1237278ab9e32ed2e20ec9015f4`. Board `/tmp/gpushader-geom` is
currently EMPTY (0 KiB), not runnable and NOT verified. The geometry
envelope `/tmp/geom_smoke.test` has NOT been transferred. Never run the
partial client. Shell cat/redirection did not produce valid binary data;
avoid using it to reconstruct this client. A long serial command was
also truncated; keep commands short. Source-confirmed ESC-[B clears a
fresh shell input line without executing it; Ctrl-C does not clear it.
One empty-CR clear attempt was rejected by automatic approval review and
was not executed; ESC-[B then restored clean input and ps succeeded.

Next: ask for physical off/on to clear the failed transfer/network state.
Deploy complete files separately at 50 Kbit/s, checking each MD5. Use the
same hardware-verified driver `33b01e3bce8dc6017bdeca4fff8ebb08`, signed zap
and config, launch it once from serial, rerun A2D/basic CS, then execute
`/tmp/gpushader-geom /tmp/geom_smoke.test --geometry`. No kernel/PXE update
is needed. Stop on first error; retain client buffers on GPU timeout.
Geometry, multi-triangle scenes, blending/blur and display integration
remain unverified. Root machines conflict and projects gitlinks untouched.

## Latest state (2026-10-22 17:26, uniform/stride/CS-A2D tests passed)

The same 17:04 physical boot and `/tmp/g2dd` PID 300 remain active, with
driver MD5 `33b01e3bce8dc6017bdeca4fff8ebb08`. No GPU restart/replacement.
At 17:16:59, serial `/tmp/gpushader-uniforms /tmp/cs_uniforms.bin --uniforms`
passed 64x64, 65x37 (80-word pitch), 320x240 and 720x720, four changing
uniform sets per size. Each frame first performed an A2D fill and full
readback, then CS pattern generation and full readback. All 32 checks had
zero pixel, row-padding and trailing 16 KiB guard errors. Program blob
1136 bytes, GLSL local size 8x8x1, one SSBO, four pushed uniform dwords.
Client MD5 `0a41ebaee352643ef76d5414fd4c6df6`; shader MD5
`80844916cd7ff58bfedbe62b279e0ce1`. Both board MD5s verified before test.
Serial ps at 17:17:29 remained healthy with sleeping PID 300.

Next geometry test is built, not yet run: host `--geometry` compiles the
shared color xform and Gouraud raster kernels, packs reflected uniforms in
a test envelope, and embeds an ordinary 3596-byte VERSION 2 blob (19+8
uniform words, three SSBOs per pass, no private memory). New client tests
GPU color/depth clears, triangle coverage, far rejection, near replacement,
depth-write disable, cull-all and matrix translation at 64x64/320x320.
Build `/tmp/q8b-gpu-20261022-geometry-gui.log` passed. Ready artifacts:

| artifact | MD5 |
|---|---|
| `system/build_aarch64/q8b/bin/gpushader` (geometry mode) | `9f93a1237278ab9e32ed2e20ec9015f4` |
| `/tmp/q8b-shader.gshj4B/geom_smoke.test` | `fd0e748e73359fcaa6610be7095abe31` |

First geometry client scp timed out; no geometry draw had been submitted.
Serial ps at 17:22:34 and 17:25:10 showed live PID 300 and responsive OS.
EUD showed no transition/reset since 17:03:59. Recovered only the network
using the README serial kill/relaunch procedure: old netd PID 43, old net
PID 41, orphan scp PID 123 cleaned up; relaunch net and netd at 17:25:44/59.
Ping then passed. Driver, kernel, PXE and firmware stayed unchanged.
Client retransfer pending MD5 verification; geometry envelope not uploaded.

## Latest state (2026-10-22 17:11, first compute pixel readback passed)

Physical off/on confirmed by the user. PXE kernel started at 17:04:14 and
EwokOS at 17:04:16; no g2dd process was present at 17:06. All six test
files were copied individually at 50 Kbit/s with serial board MD5 checks,
matching the 16:58 artifact table. One GPU initialization was launched from
serial at 17:10:18 with the CS-context driver MD5
`33b01e3bce8dc6017bdeca4fff8ebb08`.

Official GMU/SQE/HFI, signed zap INIT/MEM/stage/AUTH, secure-mode exit,
idle/fault checks and CP datapath selftest passed. At 17:10:36 the full
`gpusmoke --extended` A2D regression passed again for 64x64, 320x240 and
720x720, including all pixels/guards.

At 17:10:58, `/tmp/gpushader /tmp/cs_smoke.bin` ran against that same daemon:

```text
gpushader: A690 program upload handle=0 (880 bytes)
gpushader: compute draw ret=0
gpushader: PASS (64x64, 0 pixel errors, 0 guard errors)
```

This hardware-verifies GLSL -> shared NIR -> A690 ir3 binary -> IPC upload ->
CP_EXEC_CS -> SSBO pixel write -> CPU readback. The client never renders on
the CPU. It is the first actual compute pixel result, not just a fence or
program-upload result. Kernel, PXE and official firmware are unchanged.
No board storage was flashed; no driver restart/replacement occurred.

Next, exercise per-draw uniform uploads, non-multiple workgroup dimensions,
row padding and compute/A2D transitions through additional IPC requests to
the live instance. Geometry, scene rendering, alpha/blur and display
integration still require validation. Do not reinitialize this GPU.

## Earlier state (2026-10-22 16:58, compute build ready; upload-only passed)

The current physical boot is still 16:18:35, with the same initialized
`/tmp/g2dd` PID 303 (MD5 `387df10ba0b03a6399fc96d0eab4b3d7`). The serial
process check at 16:39:05 responded normally. No replacement or second GPU
initialization occurred. All earlier A2D pixel successes remain valid.

The compiler dependency mismatch is now understood: the clean projects
worktree is at `f8ad7121`, before the shared compiler split; the root's staged
projects gitlink `e08c8b50effa10fbf6c347691191319af91804f4` has the matching
frontend. It was exported into `/tmp/q8b-shader.gshj4B`, leaving projects,
the root gitlinks and the unresolved machines conflict untouched. The new
host harness built shared GLSL/NIR and A690 ir3, then compiled cs_smoke.comp
into an 880-byte VERSION 2 blob (384 instruction bytes, 8x8x1 local size,
one SSBO, no uniforms/private memory). It uses no firmware disassembly.

The client and blob were transferred individually at 50 Kbit/s; board MD5s
were checked at 16:53:42/46. At 16:54:07, serial command
`/tmp/gpushader /tmp/cs_smoke.bin --upload-only` succeeded with handle 0.
This hardware-verifies compiler blob parsing/allocation and IPC upload only:
it submitted no compute commands and proves no shader pixel writes.
EUD afterward showed no transition/reset since the 16:18 physical boot.

Source review found missing Mesa UMD context state before CS execution.
The new driver adds the exact vendored FD690 magic_raw table and SP/TPL1/
HLSQ compute state, disables retained draw-state groups, selects immediate
state mode, and invalidates UCHE before every shader/descriptor upload.
It does not change official firmware, privileged bring-up, kernel or PXE.
This context correction is built but NOT hardware-tested. Do not run the
old driver's CS dispatch to test a known incomplete initialization path.

Ready host artifacts (prescribed gui build passed):

| artifact | MD5 |
|---|---|
| `machines/q8b/system/build_aarch64/drivers/q8b/g2dd` (new CS context) | `33b01e3bce8dc6017bdeca4fff8ebb08` |
| `system/build_aarch64/q8b/bin/gpushader` (includes timeout buffer retention) | `b27c05269429461a0af3573d395da344` |
| `/tmp/q8b-shader.gshj4B/cs_smoke.bin` | `01b592f819bb6318acbfd8ac5af2ca9f` |
| `/tmp/q8b-g2dd-a2d-verified-387df10b` (downloaded baseline backup) | `387df10ba0b03a6399fc96d0eab4b3d7` |

Board `/tmp/gpushader` is the upload-tested client before timeout-retention
was added (MD5 `2c8edf2212054d8a613573da8e61bec5`); deploy the final client
from the table above for an actual draw. The new driver has not been copied
over or started. Latest build: `/tmp/q8b-gpu-20261022-cs-gui.log`.
Host preparation/test instructions: `test/README.shader.md`.

Next: physical off/on before loading the new driver, following the test rule
below. Deploy one file at a time at 50 Kbit/s and verify MD5. Restore the
official signed MBN and adreno.cfg under /tmp; launch g2dd once from serial,
repeat A2D extended readback, then run the 64x64 compute smoke test against
that same process. All compute pixel/geometry/display success remains
unclaimed. No board storage was flashed.

## Earlier state (2026-10-22 16:31, A2D pixel readback passed on hardware)

The user confirmed physical off/on. Latest kernel boot: 16:18:35, EwokOS
console: 16:18:37. No GPU process was present. The CCU-corrected driver,
official signed MBN, config and test client were copied individually at
50 Kbit/s, with board MD5 verification. One GPU initialization was launched
from serial at 16:25:12; the same g2dd PID 303 handled all subsequent IPC
rendering requests. No driver replacement/reinitialization was attempted.

Hardware results:

- Official GMU/SQE initialization, signed zap PAS INIT/MEM/AUTH,
  CP secure-mode exit and CP_MEM_WRITE/IB/fence selftest all passed again.
- `gpusmoke` at 16:25:53: 64x64 clear, nonaligned subrectangle fill, exact
  1:1 copy and copy-source preservation all passed, zero pixel/guard errors.
- `gpusmoke-extended --extended` at 16:30:15: repeated the basic checks,
  then tested 64x64, 320x240 and 720x720 surfaces. Each size passed eight
  consecutive full-surface fills with changing colors, cropped patterned
  copy at nonaligned coordinates, patterned 180-degree rotation, source
  preservation and uniform downscale. Downscale targets: 32x32, 160x120,
  352x360. Every pixel was checked; trailing 16 KiB guards were unchanged.
  Uniform downscale verifies coverage/bounds and cache invalidation after
  a CPU source update; it does not validate nonuniform interpolation rules.
- Serial `/bin/ps` still responded at 16:31:18; PID 303 remained asleep.
  EUD showed no transition or reset since the 16:18 physical boot.

The combined CCU configuration/synchronization correction resolves the
previous unchanged-pixel result on this run. The 2D opt-in fix also works:
`q8b g2d: Adreno A2D blitter available` is now logged. No firmware was
disassembled, modified or substituted. No speculative ECO quirk was used.

Current artifacts (supersede earlier tables):

| artifact | MD5 |
|---|---|
| `machines/q8b/kernel/kernel.bin` | `f86d304c1045c5d95b13ce7a19b8c514` |
| `machines/q8b/system/build_aarch64/drivers/q8b/g2dd` (hardware tested) | `387df10ba0b03a6399fc96d0eab4b3d7` |
| `system/build_aarch64/q8b/bin/gpusmoke` (extended client, hardware tested) | `5b1b4694f524efcf2a883979146dee57` |
| `/tmp/q8b-qcdxkmsuc8280.mbn` | `7315a508384872b908323a775a19221f` |
| `machines/q8b/test/adreno.cfg` | `515137e989a9602a950c58983cbc2a8d` |

Board `/tmp/gpusmoke` is the earlier basic client MD5
`43bcd44da2b7eac6d59e14bff21516de`; board `/tmp/gpusmoke-extended` matches
the current host `gpusmoke` MD5 above. Driver and kernel did not change
while extending the client. The prescribed gui build and whitespace checks
passed. Latest build log: `/tmp/q8b-gpu-20261022-extended-gui.log`.

Remaining work must not be reported as complete: compiled 3D/compute
shader dispatch, alpha blending, nonuniform scaling, blur and display
integration have not been validated by these tests. The signed zap image
still comes from `/tmp` during tests and is not bundled in the rootfs.
The existing port intentionally declines unsupported 90/270-degree A2D
rotation and fractional global alpha. Keep the current hardware-verified
driver as the baseline. Normal rendering remains in userspace; the kernel
only supplies the restricted SCM/PAS bridge. No board storage was flashed.

The board is currently healthy with this single initialized GPU instance;
there is no need to power-cycle merely to preserve these successful results.
Any future driver initialization/replacement still requires physical off/on.

## Earlier state (2026-10-22 16:15, A2D cache correction built)

Physical cold boot at 15:57 loaded the corrected PXE kernel. The new
`gpupas` returned ABI `0x51385001` at 16:02:12. All three official firmware
images remain unchanged. Files were copied separately at 50 Kbit/s and
board MD5s checked before the single serial GPU launch at 16:04:38:

```text
g2dd MD5 cf05ad8d3296ffe411f759df0874ccd7 (tested driver)
q8b SCM fn=201 arg=d a0=0 a1=0 a2=0   INIT_IMAGE
q8b SCM fn=202 arg=d a0=0 a1=0 a2=0   MEM_SETUP
PAS payload stage ret=0
q8b SCM fn=205 arg=d a0=0 a1=0 a2=0   AUTH_AND_RESET
official signed zap authenticated
CP idle complete int0=00000000
CP secure-mode exit complete
CP datapath selftest OK
Adreno 690 bring-up complete
q8b g3d: adreno bring-up OK (rc=0)
```

This hardware-verifies SCM authentication, secure-mode exit and the
CP_MEM_WRITE/IB/fence path. PAS13's advertised support value of zero did
not prevent actual authentication. Rendering still requires validation.

At 16:09 the new `gpusmoke --clear` submitted exactly one 64x64 A2D clear
through the existing PID 330 g2dd (no second initialization). Return was 0,
but all 4096 pixels retained sentinel `0x13579bdf`, instead of `0xff204060`.
The following 16 KiB guard region was unchanged. No GPU fault/timeout was
logged. Serial `/bin/ps` at 16:14:50 still responded; PID 330 remained asleep,
and EUD showed no reset or connection transition since the 15:57 boot.
Do not claim drawing success from the CP selftest or a successful fence.

Two source mismatches are corrected in the newly built, untested driver:

- `bsp_g2d.c` used only getenv and permanently latched 2D off before 3D
  initialization. It now uses `adreno_flag`, matching 3D and `/tmp/adreno.cfg`.
- `adreno_2d.c` incorrectly treated sysmem A2D as bypassing CCU. Mesa's
  `fd6_blitter.cc`, `fd6_emit.{cc,h}`, `fd6_barrier.cc`, `fd6_gmem_cache.h`
  and A690 device properties establish 8 CCUs with 64 KiB depth/color
  per CCU: depth offset 0, color offset `0x80000` (RB_CCU_CNTL `0x40000000`).
  The corrected stream cleans color/depth before invalidation, invalidates
  UCHE before texture reads, and cleans CCU color after CP_BLIT before the
  final UCHE fence. CCU clean events include Mesa's timestamp payload into
  a separate scratch word. IB capacity is increased to 128 dwords.

These are demonstrated source omissions, not a hardware-proven explanation
of the unchanged pixels yet. Other missing Mesa static state remains a
possible follow-up if readback still fails. No speculative ECO quirk or
firmware change was added.

Current ready artifacts:

| artifact | MD5 |
|---|---|
| `machines/q8b/kernel/kernel.bin` (unchanged, tested) | `f86d304c1045c5d95b13ce7a19b8c514` |
| `machines/q8b/system/build_aarch64/drivers/q8b/g2dd` (new, not board-tested) | `387df10ba0b03a6399fc96d0eab4b3d7` |
| `system/build_aarch64/q8b/bin/gpusmoke` (clear mode tested) | `43bcd44da2b7eac6d59e14bff21516de` |
| `system/build_aarch64/q8b/bin/gpupas` | `7e0ac78e2eef4144cbe11107460071e2` |
| `/tmp/q8b-qcdxkmsuc8280.mbn` | `7315a508384872b908323a775a19221f` |
| `machines/q8b/test/adreno.cfg` | `515137e989a9602a950c58983cbc2a8d` |

`make -C machines/q8b/system gui` and diff whitespace checks passed.
Build log: `/tmp/q8b-gpu-20261022-a2d-ccu-gui.log`. No kernel/PXE deployment
change is needed for this correction. No board storage was flashed.

Next: physical off/on before loading the new driver; copy one file at a
time at 50 Kbit/s and verify MD5. Launch g2dd once from serial, then run
`/tmp/gpusmoke` via serial against that same instance. It checks clear,
unaligned subrect fill, exact 1:1 copy, source preservation and trailing
guards, with full pixel readback and no client CPU rendering fallback.
Stop on the first error. Do not restart/replace the active GPU process in
the current boot. Normal rendering still lives in userspace; the kernel
only supplies the restricted SCM/PAS bridge.

## Earlier state (first SCM hardware query; corrected kernel staged)

The user power-cycled again. Latest boot: PON by CBLPWR at 15:45:06,
PXE `q8b/KERNEL.BIN` loaded at 15:45:20, EwokOS at 15:45:21. The new SCM
service is hardware-verified through read-only calls. At 15:48:30:

```text
SCM64 convention query: a0=0 a1=1
PAS_IS_SUPPORTED call availability: a0=0 a1=1
PAS_IS_SUPPORTED(13): a0=0 a1=0
gpupas: -1002 (the initial implementation's overly strict support gate)
```

The SCM transport works. PAS 13 advertised support is zero. This alone
proves neither that image authentication will fail nor that zap-free MMIO
is permitted: Linux's Adreno loader never gates qcom_mdt_load() on
qcom_scm_pas_supported(). The initial added precondition deviated from
that path and was corrected. No GPU MMIO, firmware authentication or GPU
run has occurred in this boot, and the board remains responsive.

The corrected query (`kernel/bsp/scm-probe.h`) requires the SCM64 convention
and logs the remaining PAS advertisements as diagnostics. Actual INIT/AUTH
results determine firmware-loading success. A host regression test models
the exact observed SCM64=yes/PAS13=no response, verifies advisory zeros do
not reject the transport, and checks missing SCM64/BUSY plus read-only call
restrictions. It passed under AddressSanitizer/UndefinedBehaviorSanitizer.
Both kernel and prescribed gui builds passed, as did diff whitespace checks.

Current artifacts (supersede the table in the earlier implementation section):

| artifact | MD5 |
|---|---|
| `machines/q8b/kernel/kernel.bin` | `f86d304c1045c5d95b13ce7a19b8c514` |
| `machines/q8b/system/build_aarch64/drivers/q8b/g2dd` | `cf05ad8d3296ffe411f759df0874ccd7` |
| `system/build_aarch64/q8b/bin/gpupas` | `7e0ac78e2eef4144cbe11107460071e2` |
| `/tmp/q8b-qcdxkmsuc8280.mbn` | `7315a508384872b908323a775a19221f` |

The corrected PXE kernel is staged and byte-verified at
`/Users/tony/work/q8b/eud/pxe/tftp/q8b/KERNEL.BIN`, SHA256
`5f2128eca9d49009cf49df8bac08afa6a1c5e0a853f7463b6523c4bad6c0b583`.
The tested but over-gated kernel is backed up alongside it as
`KERNEL.BIN.scm-gate-20261022-1545`; the original pre-SCM backup remains.
No board storage, loader or rootfs was changed. The current boot has only
the old `gpupas` in `/tmp` (MD5 `33cd855c9ea8076e8b7a4e0ff2472066`);
deploy the new tool and driver/MBN after loading the corrected PXE kernel.

Next: physical off/on to load the corrected kernel, query, then one normal
GPU initialization/authentication/rendering test through serial if the
transport query succeeds. No speculative TRUST_CNTL access. Test source:
`machines/q8b/test/scm_probe_test.c`. Current build logs:
`/tmp/q8b-gpu-20261022-pas-advisory-{kernel,gui}.log`.

## Earlier SCM/PAS build staged for PXE (superseded)

Hardware remains at the successful 15:03 CP initialization described below.
The user explicitly authorized compiling a Q8B kernel and testing through
PXE without flashing board storage. The restricted SCM/PAS service and
board-selected signed MBN loader now compile; a new PXE kernel is staged,
but has not booted or run on hardware yet. Physical off/on was requested.

No Q8B-specific evidence was found for Linux's zap-free initialization
branch. Radxa's DT selects a signed zap image; its `unfused` comment allows
reuse of the X13s image, not a conclusion that zap is optional. Conversely,
the prior crash did not independently prove that direct TRUST_CNTL access
was its cause. Keep those conclusions separate. Do not try that access as
a capability probe. The rendering/command driver remains in userspace.

## Last hardware-verified state (2026-10-22 15:04)

Official GMU/SQE firmware, GMU/HFI startup, GX OOB power-on, all 13 core-init
stages and the full CP_ME_INIT handshake now pass on the board. Ring pointers
are 9/9, the hardware RPTR shadow updates, GPU idle passes and INT0 is zero.
The board remains responsive after initialization returns.

GPU rendering is still unavailable: Q8B declares a signed zap shader, but
EwokOS has no SCM PAS transport. The current build returns `-ENOSYS` (-27 in
this libc) there, clears GPU_SET and leaves GPU/CP readiness false. The old
unverified direct SECVID_TRUST_CNTL fallback was removed. Next work is the
standard signed-image/SCM PAS authentication path, not further GMU/SQE
firmware changes. Current g2dd MD5: `3d22d74ca1faa6be8eb8a0b974d729b2`.

## Active route

Keep the bring-up path narrow and match upstream Linux:

1. Use the exact official A690 catalog firmware files.
2. Complete and verify the standard hardware initialization before releasing
   the GMU CM3 from reset.
3. Do not disassemble, patch, replace, or inject GMU firmware.
4. Do not use CM3 spin/NOP probes, host cache substitutes, broad SMMU stream
   matches, forced SMMU faults, or forced GX power experiments.

Every GPU test must start from a physical cold boot. Approved build commands:

```sh
make -C machines/q8b/system gui
make -C machines/q8b/kernel
```

Copy one file at a time with `scp -l 50`, verify its MD5 on the board, and
start the GPU only from the serial console. Kernel testing is PXE-only;
do not flash the loader, rootfs or board storage.

## Firmware status

The repository, the board, and upstream `linux-firmware` commit
`2aded3b604ae42d3f6b1c5a5873db021ec4e4207` contain identical files:

| file | version | size | SHA256 |
|---|---:|---:|---|
| `a660_gmu.bin` | `v3.01.0B` | 55252 | `8acab7b417d9ebde89a1de9ae1e2c261d352fcab122e31ecd580cec9fe2ae5e7` |
| `a660_sqe.fw` | `v1.12` | 43292 | `d222f3fe290ef0516ee0ec43082596bad2df0fcbc2e0bbb26987623cef90cf76` |

Mainline's A690 catalog explicitly selects these global A660 images. The
loader accepts only `a660_gmu.bin` and validates its exact size and four-block
layout. There is no `a690_gmu.bin` fallback.

## Initialization status

The GMU release sequence is the upstream sequence:

```text
CM3_SYSRESET = 1
CM3_FW_INIT_RESULT = 0
CM3_SYSRESET = 0
poll CM3_FW_INIT_RESULT
```

Power, RPMh, clocks, interconnect, SMMU TLB synchronization, and firmware
layout validation fail closed. The old diagnostic paths that modified CM3
ITCM/icache or bypassed normal power/SMMU behavior have been removed.

The GCC runtime check covers the clocks actually named by the SC8280XP GMU
and GPU SMMU DT nodes. `gcc_gpu_tcu_throttle_ahb_clk` is not one of those
clocks and must not be invented as a GMU boot prerequisite.

The current correction is in the Adreno SMMU geometry. Context-bank pages
must not be hardcoded. Linux derives them from `GR0_ID1`:

```text
pgshift = ID1.PAGESIZE ? 16 : 12
numpage = 1 << (ID1.NUMPAGENDXB + 1)
CB0 = numpage << pgshift
CB1 = (numpage + 1) << pgshift
```

`NUMCB` is an actual count, not a count-minus-one field. Before any context
bank access, the driver checks that CB0/CB1 and the hardware-reported total
span fit the DT resource `Q8B_ADRENO_SMMU_SIZE=0x20000` and logs `ID1`,
`pgshift`, `numpage`, `numcb`, `cb0_byte`, and `cb1_byte`.

The page-table walker uses WBWA and Inner Shareable for the DT
`dma-coherent` SMMU. The AArch64 configuration was further corrected below;
the current expected TCR is `0x00803519`, not the earlier `0x80803519`.

The GMU cold boot has now completed with the official firmware. Every HFI
message, including the initial DCVS vote, is acknowledged with `error=0`.
The initial vote is not expected to power GX immediately. Linux first issues
the non-legacy `GPU_SET` OOB request from `hw_init()`, then accesses GPU core
registers while the request is held. The driver now follows that order:

```text
initial DCVS ACK: gxstat=0xc0
GPU_SET OOB ACK: info=0x80000000, gxstat=0
GX_GMU clock enable: success
```

This confirms the firmware selection/loading and the GMU-side initialization
through GX power-on. The current failure is after `GPU_SET`, at the first GPU
core initialization accesses in `a6xx_hw_init()`.

The current diagnostic build adds numbered stage messages around those
accesses, with separate markers after the first `GBIF_HALT` and
`RBBM_GBIF_HALT` writes. It also uses the A690 catalog values
`PC_DBG_ECO_CNTL=0x00800200`, `RBBM_INTERFACE_HANG_INT_CNTL=0x404fffff`, and
`CP_CHICKEN_DBG=0x00028801`.

The first cold-boot test of those markers reached stage 13, proving the full
GPU core MMIO initialization completes. `CP_ME_INIT` then drained the ring
(`rptr=wptr=9`) but latched `RBBM_INT_0_STATUS=0x200` (CP_HW_ERROR). The next
diagnostic build adds the standard A690 `UCHE_CLIENT_PF=0x81`,
`UCHE_CMDQ_CONFIG=0x90`, and A660-family LPAC FIFO size `0x20`, plus CP error,
SQE/ring-base, and SMMU fault readbacks matching Linux's IRQ diagnostics.

That diagnostic isolated the fault:

```text
CP pre-init int0=0 intr=0 hw=0 protect=0
CP fault intr=0x4 hw=0x07400000
CB0 FSR=0x80000408 FAR=0x9592a700
```

`FSR=0x408` is a long-descriptor permission fault and the FAR is inside the
SQE BO. CB0's identity blocks were incorrectly privileged-only. Expanded
APRIV made the ring read privileged, explaining why it drained, while SQE
instruction fetches remained unprivileged and faulted. CB0 now uses the
unprivileged Normal-NC mapping Linux gives ordinary GPU BOs. This is a host
page-table permission fix; the official firmware is unchanged.

The follow-up cold-boot test with that fix and Linux-style fault diagnostics
completed the same bring-up path with the new result:

```text
CP config sqe=00000000:95920000 rb=00000000:95910000 cntl=0000020c sqectl=00000001
CP_ME_INIT words 70c80008 0000002f 00000003 20000000 00000000 00000000 00000000 00000000 00000000
GPU fault status=00000200 rbbm_status=00000001 rptr=00000009 wptr=00000009
CP fault intr=00000004 hw=07400000 protect=00000000 status1=00000000
SQE stat1=70c80008 ib1=00000000:00000000 rem=00000000 ib2=00000000:00000000 rem=00000000
CB0 FSR=400 ... GFSR=0
```

The prior `CB0 FSR=0x408` permission fault is gone. The official firmware,
GMU HFI, initial DCVS vote, `GPU_SET` OOB, GX clock enable, SMMU CB0/CB1
setup, and all thirteen GPU core-init stages still complete. The remaining
blocker is therefore a CP hardware fault during the standard `CP_ME_INIT`
handshake, not firmware selection or GMU startup. The `FSR=0x400` readback
has no `PF`/`TF` bit; its FAR/FSYNR fields are retained only as diagnostics
until the CP fault source is identified from the upstream A690 initialization
state.

## 2026-10-22 standard A690 RPTR-shadow completion

The remaining host-init difference was identified by direct comparison with
Linux `a6xx_ucode_load()` and `hw_init()`: an A690 with `hw_apriv` always has
a hardware-written RPTR shadow, and Linux maps that shadow BO with
`MSM_BO_MAP_PRIV` before programming `CP_RB_RPTR_ADDR`. The A690 APRIV value
already used here includes `RBRPWB` (bit 3), so omitting that privileged
shadow was not a faithful initialization.

The port now follows that standard path without modifying either firmware:

- ordinary SQE/IB BOs remain unprivileged Normal-NC mappings;
- the ring and independent RPTR-shadow page are mapped privileged, matching
  Linux `check_apriv()` for an A690 with `hw_apriv`;
- `CP_RB_RPTR_ADDR` is programmed before `CP_SQE_CNTL=1`;
- ring-idle polling reads the hardware shadow, as Linux does for `hw_apriv`;
- the CP configuration log includes the RPTR address register and shadow word.

The build command completed successfully:

```text
make -C machines/q8b/system gui
g2dd MD5: 9e01b119ca4d1ca94ed538d2639e7836
```

This hash includes the final `ring_freewords()` change to read the hardware
RPTR shadow. The earlier `8de3d9c583d04c4caabd8431ed236a5f` artifact is
obsolete and must not be deployed.

Firmware was rechecked in both source and build trees:

```text
a660_gmu.bin  55252 bytes  sha256 8acab7b417d9ebde89a1de9ae1e2c261d352fcab122e31ecd580cec9fe2ae5e7
a660_sqe.fw   43292 bytes  sha256 d222f3fe290ef0516ee0ec43082596bad2df0fcbc2e0bbb26987623cef90cf76
```

Board validation is pending another physical cold boot. Two EUD SRST attempts
at about 12:53 local time produced no new UEFI/PXE or serial output. A later
physical cycle was confirmed by a new PXE DHCP exchange at 12:55 and SSH was
briefly reachable, but the board then stopped answering ICMP/SSH and the
newly enumerated `/dev/cu.usbserial-0035E5950` console produced no output.
No new `g2dd` was deployed or started, so this is not a GPU test result. The
EUD implementation documents that a missed SRST SET can leave the board held
in reset until power is removed. Power the board fully off and on again while
capturing the serial console, then deploy only the MD5 above at 50 Kbit/s and
start `g2dd` from that console.

## 2026-10-22 cold-boot result: privileged mappings built before CB0 enable

The physical cold boot at 14:08 was used for exactly one GPU run. The deployed
artifact was verified on the board before launch:

```text
g2dd MD5: e7977582669f9a87400e39ba61d456ea
```

The official firmware path remained healthy: the GMU reached cold-boot
complete, every HFI response reported `error=0`, `GPU_SET` completed, and all
13 GPU-core initialization stages completed. CB0's software walk verified the
privileged RPTR page and both ends of the 32 KB privileged ring before the
context bank was enabled. CB0 and CB1 both read back `ACTLR=0x303`.

The run again stopped immediately after printing the nine standard
`CP_ME_INIT` words. The RPTR shadow remained zero and the SoC stopped before a
timeout/fault dump could be printed; EUD 9501 disappeared about seven seconds
later. The EDL guard detected the failure at 14:30:10 and automatically reset
the board; a complete UEFI/EwokOS reboot appeared at 14:32. That automatic
recovery is not a physical cold boot and must not be used for the next GPU
run. No second GPU run was attempted in that boot cycle.

Both context banks were written with `SCTLR=0x1127`, matching Linux with CFIE
omitted because EwokOS has no context-fault IRQ handler, but this SMMU reads the
value back as `0x127`: bit 12 (`S1_ASIDPNE`) did not read back set in that
configuration. That observation alone does not establish RAZ/WI behavior or
the cause of the CP failure. Requested and actual values are logged.

The next build follows current Linux more literally at the CP boundary:

- read back each GBIF halt register immediately after clearing it;
- program `CP_RB_RPTR_ADDR`, then zero/publish the shadow;
- do not issue the extra pre-start `CP_RB_WPTR=0` write;
- log `CP_APRIV_CNTL` and whether the `CP_RB_WPTR` doorbell store returns.

This remains standard host initialization and does not alter either firmware.
The prescribed build completed successfully:

```text
make -C machines/q8b/system gui
g2dd MD5: d1ce1667321ea8516eb35b2991f644d9
```

The earlier `e7977582669f9a87400e39ba61d456ea` artifact is retained only as
the identity of the completed 14:27 test and must not be used for the next run.

## 2026-10-22 AArch64 context-bank configuration review

The user physically power-cycled the board again; a new EwokOS boot was
captured at 14:44. No GPU process was present before deployment.

Direct comparison with Linux v6.6 `arm_smmu_init_context_bank()` and
`arm_smmu_write_context_bank()` identified concrete host configuration
differences in both CB0 and CB1. The current build:

- sets CBAR bypass shareability to NSH (`3 << 8`), giving `CBAR=0x1f300`,
  so TCR/PTE attributes take precedence, as in Linux;
- sets `TCR2.AS` for AArch64 (`TCR2=0x38012`) and omits AArch32-only TCR.EAE
  (`TCR=0x00803519`);
- writes CBA2R before CBAR, TCR2 before TCR/TTBRs, and MAIRs after TTBRs;
- explicitly writes the disabled TTBR1 with the context ASID and zero base,
  and leaves AArch32-only CONTEXTIDR out of the setup;
- prints a CP doorbell-return marker using only the software WPTR, before
  any GX MMIO or RPTR-shadow read. The prior marker included a GPU register
  read and could not distinguish a blocked read from a blocked store.

These changes match standard host initialization; neither official firmware
file was changed. They have not yet proved the cause of the CP hang.

The required build and `git diff --check` succeeded (existing build warnings
remain):

```text
make -C machines/q8b/system gui
g2dd MD5: 5ba235e76dc8b500decfc63cbea275b1
g2dd size: 214392 bytes
```

This artifact supersedes `d1ce1667321ea8516eb35b2991f644d9`, which was not
tested. The first 50 Kbit/s upload timed out with an incomplete 4 KB file.
Network services were restarted through serial; retry completed and the
driver MD5 was verified on the serial console at 14:50:07. The minimal config
and both firmware files were also verified before launch.

At 14:52:17 the driver was started once through serial. The standard GMU/HFI,
GX OOB and all 13 hardware-init stages completed again. New evidence:

```text
CB0 AArch64 cbar=1f300 cba2r=1 tcr=803519 tcr2=38072 ttbr1=1000000000000
CB1 AArch64 tcr2=38072 ttbr1=1000000000000
CP config ... apriv=0000006e
CP_ME_INIT doorbell store returned wptr=00000009
CP_ME_INIT doorbell posted wptr=00000009 shadow=00000009
```

TCR2 was requested as `0x38012` but read back as `0x38072`; record this
without inferring that the additional bits are the fault source. SCTLR
still read `0x127` after requesting `0x1127`.

The CP has now demonstrably consumed all nine command words and written the
privileged RPTR shadow. No CP-complete/secure-mode boundary marker existed
in this build, so the subsequent crash cannot be assigned to a particular
access. EUD disappeared at 14:52:25 and 900E/ramdump boot output appeared at
14:53. Do not label this a complete CP handshake or successful GPU bring-up.

## 2026-10-22 CP idle and required zap authentication

Further direct comparison found that Linux `a6xx_idle()` waits for GMU
`GPUBUSYIGNAHB` to clear, then all RBBM busy bits except CX-master AHB busy,
then checks hang status. Consuming the ring alone does not establish idle.
The next build adds that sequence after CP_ME_INIT, with stage markers
separating ring drain, idle reads, fault check and CP completion.

The Q8B device-tree patch explicitly enables `gpu_zap_shader` with
`qcom/sc8280xp/LENOVO/21BX/qcdxkmsuc8280.mbn`. Missing EwokOS SCM transport
is not Linux's `-ENODEV` case (which means no zap node). The old code
incorrectly wrote `RBBM_SECVID_TRUST_CNTL=0` and also emitted
`CP_SET_SECURE_MODE` without authenticating a zap image. Linux warns that
the direct register write may cause a permissions violation on a device
requiring zap. That remains a possible cause of the last crash, not proven
by its logs.

The new build removes that incorrect fallback. After a successful CP idle
and fault check, it reports the missing SCM PAS dependency and returns
`-ENOSYS`, leaving `gpu_ready`, `cp_ready` and `zap_done` false. Completing
the standard Q8B secure-mode exit follows the signed-image/SCM PAS path.
There is no evidence yet that this Q8B permits Linux's zap-free branch.

```text
make -C machines/q8b/system gui: success
git diff --check: success
g2dd MD5: 3d22d74ca1faa6be8eb8a0b974d729b2
g2dd size: 214512 bytes
```

The user confirmed another physical off/on, matching the new boot at 14:58.
No GPU process was present. Both files were copied separately at 50 Kbit/s
and their board MD5s verified through serial at 15:02. The earlier
`5ba235e76dc8b500decfc63cbea275b1` is only the identity of the 14:52 test.

The one GPU test of this boot started from serial at 15:03:09 and completed
with the expected controlled error:

```text
CP_ME_INIT doorbell posted wptr=00000009 shadow=00000009
CP_ME_INIT ring drained
CP idle GMU busy=71000000
CP idle RBBM status=00000001
CP idle complete int0=00000000
CP_ME_INIT fault check begin
CP_ME_INIT complete
secure-mode exit requires Q8B signed zap + SCM PAS; SCM transport unavailable
secure-mode exit failed (-27)
GPU_SET OOB cleared
q8b g3d: adreno bring-up DECLINED (rc=-27)
```

At 15:04:35 serial `/bin/ps` still responded, with `/tmp/g2dd` PID 215 asleep
and normal CPU idle percentages. This establishes that CP startup can finish
without the earlier hang. It does not independently prove which access
caused the 14:52 crash: idle ordering and the secure-mode error path both
changed between these two runs. The CP_MEM_WRITE datapath selftest has not
run, and no rendering success is claimed. Do not start another GPU run in
this boot.

The kernel currently exposes PSCI SMC internally, but no userspace SCM/PAS
service. A normal zap implementation needs the board-selected signed image
`qcom/sc8280xp/LENOVO/21BX/qcdxkmsuc8280.mbn`, a loader matching Linux
`qcom_mdt_load()` (including metadata/hash handling, ELF class, segment
validation and relocation), and the PAS init/memory setup/auth-and-reset
calls from privileged context. The existing unused generic zap parser is
not ready: it prefers a generic image over the board-specific image, assumes
split segments, and does not implement Linux's hash-segment exclusion.
Do not wire that parser directly into an authentication path.

The cached `/tmp/adrfw/a690_zap.mdt` is an HTML response, not a firmware
image (`file` verified). It was not deployed or loaded and must not be used.
The final EUD check remained online with no USB state changes or EDL guard
reset after the physical 14:58 boot and the successful 15:03 CP test.

The user subsequently authorized the kernel/PXE scope. See the new section
below for the built implementation and exact staged artifacts.

## 2026-10-22 signed zap / restricted SCM PAS implementation (not board-tested)

The new kernel service uses `SYS_PLATFORM_CTL=0x100` outside the syscall
counter enum, preserving `SYS_CALL_NUM` and the existing `sys_state_t` ABI.
It is gated by CAP_ROOT and only permits GPU PAS 13 operations in order.
No arbitrary SMC function, PAS ID or destination address is caller-selected.
The SMCCC wrapper preserves Qualcomm's x6 interrupted-call token; BUSY
retries sleep in userspace. WAITQ is an explicit unsupported error.

The query first probes SCM64, PAS_IS_SUPPORTED availability, GPU PAS 13
support and INIT_IMAGE/MEM_SETUP/AUTH_AND_RESET availability. Only the exact
ABI token `0x51385001` permits further initialization. An unsupported query
is not proof that a zap-free GPU configuration is permitted.

Metadata is copied from a caller-owned DMA allocation into an independent,
kernel-owned coherent allocation and retained through authentication (also
retained on failure until cold boot). This avoids caller exit releasing
memory while PAS may protect it. The executable payload is staged through
a kernel-only Normal-NC alias of the fixed DT region at `0x8bf00000`.
The entire 16KB CPU page is excluded from heap allocation, but staging
writes only the validated footprint within the 8KB zap resource.

Userspace accepts the board-selected monolithic `qcdxkmsuc8280.mbn` only,
not generic a690 split files. `adreno_zap.c` validates ELF32 tables, ranges,
overflows, overlaps and relocation; excludes HASH segments; and packs ELF
header plus hash bytes without file padding. The official image yields:

```text
file size 14392, metadata 0x1a34, one payload segment 0x838 bytes
relocation base 0x5000, reserved-memory footprint 0x1000
SHA256 d7b8cc648c2f020c0fb7770b2d844b40688e99fb541e45514533049611b34d5c
```

Unchanged source download: `/tmp/q8b-qcdxkmsuc8280.mbn`, from linux-firmware
`qcom/sc8280xp/LENOVO/21BX/qcdxkmsuc8280.mbn`. It is not bundled in the repo;
copy to `/tmp/qcdxkmsuc8280.mbn` on each test boot. TrustZone, not this parser,
verifies the signature. No firmware instructions were disassembled or patched.

The loader requests SCM's DT crypto-to-EBI bandwidth path (CE0, SN4, SN0,
SH0, MC0/ACV), then follows INIT_IMAGE -> MEM_SETUP -> payload stage ->
AUTH_AND_RESET. On success it emits CP_SET_SECURE_MODE directly on the
ring, waits for drain/idle/fault check, and only then runs the existing
CP_MEM_WRITE/IB/fence selftest. SCM bandwidth votes remain held because
the port has no validated suspend path yet.

Validation completed:

- Kernel and prescribed gui builds passed; existing unrelated warnings remain.
- Host parser test passed with AddressSanitizer and UndefinedBehaviorSanitizer,
  checking the official metadata/payload, every truncated prefix, wrong
  class/endian, missing hash/relocation, malformed ranges/footprint, unaligned
  input, and HASH segments marked PT_LOAD.
- Diff whitespace checks passed. The final kernel links the strong Q8B
  service and the separate SMCCC wrapper.

Final artifacts:

| artifact | MD5 |
|---|---|
| `machines/q8b/kernel/kernel.bin` | `37340e2070f89037107091398a66dbbb` |
| `machines/q8b/system/build_aarch64/drivers/q8b/g2dd` | `cf05ad8d3296ffe411f759df0874ccd7` |
| `system/build_aarch64/q8b/bin/gpupas` | `33cd855c9ea8076e8b7a4e0ff2472066` |
| `/tmp/q8b-qcdxkmsuc8280.mbn` | `7315a508384872b908323a775a19221f` |

The exact TFTP kernel `/Users/tony/work/q8b/eud/pxe/tftp/q8b/KERNEL.BIN`
was replaced atomically after backup and byte comparison. New SHA256:
`1dd264f7c2a25c8d48e6c1904f51055d4bbca61c918e6be16eabc9747770636a`.
Backup: same directory, `KERNEL.BIN.pre-scm-20261022-1537`, SHA256
`410c096dfbfb378a7e585212b8e21a574f7b883812f031c46983fd82e5229d88`.
No loader or rootfs was replaced; no board storage was flashed.

Next: wait for physical off/on to load that PXE kernel, deploy one file at
a time at 50 Kbit/s, verify board MD5s, run `/tmp/gpupas` first (read-only,
no GPU MMIO or resets), then one serial-only GPU run if the ABI query passes.
Keep the official GMU/SQE unchanged. Hardware authentication and rendering
are still unverified. Last checked board state remained the physical 14:58
boot with old `/tmp/g2dd` PID 215 asleep; do not run another GPU test there.

Build logs: `/tmp/q8b-gpu-20261022-pas-kernel-final.log` and
`/tmp/q8b-gpu-20261022-pas-gui-final.log`. Host test source:
`machines/q8b/test/zap_parser_test.c` (compile command in its header).

## Corrected cache-handshake evidence

The earlier conclusion that an AOP/PMIC grant was missing is invalid. The
correct host-visible handshake registers are:

```text
request 0: 0x4c05    acknowledge 0: 0x4c0a
request 1: 0x4c04    acknowledge 1: 0x4c09
```

The AP can obtain both acknowledgements reliably. Do not use the old
`0x1f405/0x1f40a/0x1f404/0x1f409` interpretation to guide further work.

## Board-test rule

Every run must start from a cold boot. Build only with the command in the
active route, copy one file at a time at 50 Kbit/s, verify its MD5 on the
board, and start the GPU process only from the serial console.

## Code entry points

- `system/libs/adreno/src/adreno_fw.c`: exact firmware selection and validation.
- `system/libs/adreno/src/adreno_smmu.c`: SMMU discovery and CB0/CB1 setup.
- `system/libs/adreno/src/adreno_gmu.c`: standard GMU initialization and release.
- `system/libs/adreno/src/adreno_gpucc.c`: GPUCC/GCC clocks and power domains.
- `system/libs/adreno/src/adreno_rpmh.c`: RPMh and interconnect votes.
- `test/README.md`: historical test record; superseded experiments are not an
  active test plan.
