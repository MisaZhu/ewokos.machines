# Q8B g2dd API conformance and integration

Build the default desktop with `make -C machines/q8b/system x`, then package
it with `make -C machines/q8b/system sd`; `make -C machines/q8b rootfs`
does both. The optional `gui` target builds the graphical console instead.
Both profiles set `ADRENO_ENABLE=1` and start g2dd directly in `init.rd`
through `ipcserv`, before display/Graph clients.
On a normal boot, use `/bin/g2dtest` against that daemon: do not launch a
second g2dd. Full desktop compilation is now authorized by the user's
request to enable default g2dd rendering and compile/commit the system.
`system/build_aarch64/q8b/bin/gpusmoke` contains bounded API and graph
integration suites. The generic `g2dtest` also includes long benchmarks.
Its 31 failures on the previous driver exposed missing fractional solid fill
and 90/45-degree rotation. Those operations are now implemented; retain its
original assertions and require a complete rerun against the updated daemon.
The new BSP/private-kernel routes, full `--apis` and complete `g2dtest`
passed on hardware on 2026-10-22: `g2dtest summary: PASS (0 failure)`.
The same driver also passed the display-library and real 720x720 GOP
presentation/desktop-restoration reruns with zero pixel errors.
See HANDOVER.md for the exact driver/client hashes and serial evidence.

## Board procedure

Follow HANDOVER.md: one GPU initialization per physical cold boot. Additional
requests to that same running daemon do not initialize the GPU again. Never
replace/restart the daemon during the same boot. Use `scp -l 50` for one file
at a time and check `/bin/md5` before transferring the next file. Run tests
through serial, using RAM `/tmp` only; no storage flashing or kernel change.

The manual `/tmp/g2dd` launch below applies only to a diagnostic image
whose init script did not already start g2dd. Confirm `/bin/ps` first.

```text
/bin/bgrun /tmp/g2dd /dev/g2d
/tmp/gpusmoke-api --api-raster
/tmp/gpusmoke-api --extended
/tmp/gpusmoke-api --apis
/tmp/g2dtest
```

Stop on an unexpected result. On a submitted timeout/MMU fault, gpusmoke
retains its process and buffers until reboot; do not kill it or submit more
work. Fault-policy regression runs on the host, without board fault injection.
Separate groups can isolate ordinary pixel/return-code failures on the same
healthy daemon:

```text
/tmp/gpusmoke-api --api-physical
/tmp/gpusmoke-api --api-integration
/tmp/gpusmoke-api --api-alpha
/tmp/gpusmoke-api --api-cpu-alpha
/tmp/gpusmoke-api --api-scale
/tmp/gpusmoke-api --api-raster
```

Every direct operation checks the complete destination and a trailing 16 KiB
guard against independent integer expectations. Physical-destination tests
write only their own contiguous allocation, with a prefix, 320-byte row pitch,
padding and suffix checked. They never name the real framebuffer or MMIO.
CPU rendering is used only in the separately labelled graph integration
references/fallback tests. A correct decline is not an accelerated pixel PASS.

## Acceptance matrix

| API | Required Q8B result |
|---|---|
| get_version | success, 690 |
| get_clock | NOT_SUPPORTED, zero; frequency is not reported |
| pgl_clear | exact A2D readback in `--extended` |
| fill_rect | opaque/clipped A2D; alpha 0 no-op; exact fractional-alpha compute fill |
| fill_triangle | NOT_SUPPORTED, no write; alpha 0 no-op |
| blit | exact A2D 1:1 including narrow crops; rational nearest compute scaling |
| blit_alpha | alpha 0 no-op; exact compute source-over for other global alphas |
| rotate | exact 0/90/180/270 and normalized angles; arbitrary-angle Q14 compute rotation |
| scale_to | exact corner-preserving Q15 nearest, down/up, odd width and one row |
| blit_to_phy | exact cropped copy, explicit pitch/padding; short size/bad pitch rejected |
| rotate_to_phy | exact cropped 180; 90/270 rejected without writes |
| gaussian_blur | NOT_SUPPORTED, destination/scratch unchanged for radii 1..4 and 6 |
| pgl_program/draw | separate `README.shader.md` suite; not a 2D fallback test |

`--api-alpha` covers source alphas 0,1,127,128,254,255, global alphas
1,128,254,255, and opaque/partial/transparent destinations. The final compute
implementation must match the integer reference exactly (zero tolerance),
including `(src_a * global) >> 8`, the opaque-copy fast path, /255 rounding,
and retaining source RGB for a transparent destination. `--api-cpu-alpha`
independently checks the actual NEON fallback against that same scalar oracle
in 96 cases: widths 1/15/16/17/31/32/33/64, four global alphas and three
destination alphas. It checks the entire canvas and guards, including lanes
whose effective alpha becomes zero. The SIMD and scalar paths must agree.

Identity `g2d_rotate` requests (0/360/-360 degrees) use a whole-canvas opaque
BLIT in the client, since existing daemons reject zero in their ROTATE
command. Dimensions must match; this route performs no CPU pixel copying.

`--api-scale` tests 64x64 -> 32x37, 96x83, 17x9, 16x1 and 64x64, plus
cropped up/down BLITs. SCALE_TO uses `ceil(((src-1)<<15)/(dst-1))` (zero step
when dst is one); BLIT uses `floor(pixel * crop_extent / dst_extent)`.
These are different mappings. A2D pixel-centre sampling failed this oracle.

Public graph integration tests run at/above the offload size threshold and
compare whole canvases/guards for opaque fill, fractional fill,
triangle fallback, global alpha, non-contiguous fallback, 90/180/270 rotation
and exact software Gaussian fallback. graph_scale_tof has bilinear/factor
semantics, so its GPU helper only accepts identity scaling through this
nearest-only protocol. Both input and output must be eligible shared graphs.

Triangle and exact Gaussian remain unsupported GPU features. Correct client
fallback provides those operations; their tests must not be described as GPU
acceleration. Raw physical 90/270 rotation also continues to decline; canvas
rotation is a separate compute path.

`--api-raster` tests 18 solid-alpha cases (six color alphas and three
destination alphas), including a 29x23 crop, then 44 rotation cases at
64x48, 65x37, 1x1 and 320x240. Angles include quadrant mappings, 45/135,
1/89/91/179, negative and >360 normalization, and packed odd pitches.
Its arbitrary-angle oracle retains the original 64-bit CPU two-shift Q14
formula, independent of the shader's bounded 32-bit combined formula.
Every output/source pixel and guard must match exactly.

## Reproducible application shader

The private 2D programs come from ordinary GLSL in
`system/libs/adreno/shaders/blit.comp` (scale/blend) and `raster.comp`
(solid source-over/rotation), using the already-tested Mesa GLSL/NIR/
ir3 compiler. It is unrelated to GMU/SQE/zap firmware. It uses two SSBOs,
8x8 workgroups, twelve uniform words and no private memory. The reflected
uniform offsets are 0/4/8. Its own kernel slot preserves all app program handles.
The driver declines overlapping source/destination allocations before submission,
except solid fill, which reads/writes only its invocation's destination pixel.

After building the host compiler as described in README.shader.md:

```sh
sh machines/q8b/test/check_2d_shader.sh /tmp/q8b-shader.gshj4B/out/compile_shader_host
```

This checks reflection and byte-for-byte reproduction of the embedded blob.
It proves compilation/packaging, not GPU execution.

To test the new raster payload using an already initialized older daemon:

```sh
sh machines/q8b/test/check_2d_shader.sh /tmp/q8b-shader.gshj4B/out/compile_shader_host \
  /tmp/q8b-raster-probe.bin
```

Transfer the generated probe and current `gpushader` individually with MD5
checks, then run through serial:

```text
/tmp/gpushader-raster /tmp/raster-probe.bin --raster
```

The compiler changes only the GLSL destination binding to 0, because the old
fullscreen public API supplies only binding 0. The script verifies that the
complete blobs differ in exactly that binding metadata byte: application
instructions and constants are identical. Both descriptors name one owned
allocation, with disjoint source/output rows. The client checks 18 alpha and
44 rotation cases, including all source, padding, prefix/suffix and guards.
This passed on hardware at 19:45:57 on 2026-10-22 against the existing daemon,
without a second GPU initialization. It proves the new shader arithmetic and
bounds, not the new private two-buffer dispatch or full g2dtest API integration.
Each probe upload consumes an application program slot until reboot.

The backend disables `has_isam_ssbo`: this driver's blob protocol binds UAV
descriptors only. Mesa otherwise optimizes readonly SSBO loads into texture
reads, allocating an unbound texture slot. Variant serialization rejects any
remaining sampler/texture slots; the script also checks that rejection using
`cs_texture_unsupported.comp`. The previous readonly-source blob produced zero
pixels on hardware despite a successful fence. The corrected blob uses LDIB
for SSBO reads and passed --extended and the full --apis hardware matrix on
2026-10-22 (see HANDOVER.md for the exact live driver/client hashes). Previously prepared
geometry blobs with readonly buffers must also be recompiled before use.

## Host routing and fault regression (macOS)

These link actual library sources with mocked IPC/sleep and never access a GPU.
Run after the prescribed gui build installs the SDK headers:

```sh
cc -std=gnu99 -O2 -ffunction-sections -I system/build_aarch64/q8b/include \
  machines/q8b/test/g2d_fault_test.c system/gui/libs/g2dclient/src/g2dclient.c \
  -Wl,-dead_strip -o /tmp/q8b-g2d-fault-test
/tmp/q8b-g2d-fault-test
cc -std=gnu99 -O2 -fgnu89-inline -ffunction-sections \
  -I system/build_aarch64/q8b/include -I system/platform/aarch64/include/aarch64 \
  -include g2dclient/g2dclient.h machines/q8b/test/g2d_graph_test.c \
  system/gui/libs/graph/src/graph_g2d.c system/gui/libs/graph/src/blt.c \
  system/gui/libs/graph/src/graph.c system/gui/libs/graph/src/pixel.c \
  -Wl,-dead_strip -o /tmp/q8b-g2d-graph-test
/tmp/q8b-g2d-graph-test
```

Opaque graph fill must reach IPC; transient declines must replay without
disabling that capability; unsupported triangles must latch independently;
either ineligible scale canvas must block IPC. Submitted timeout/MMU results
must park before returning, whereas safe declines return normally.

The Q8B daemon builds with `G2D_PITCH_ALIGN=4`: A2D alignment is about the
surface pitch, not crop width. Consequently the whole crop reaches the BSP;
there is no CPU tail rasterization hidden inside g2dd. Inspecting the built
daemon object's undefined symbols must show BSP hooks and no `arch_g2d_*`
pixel hooks. Other machines keep their existing split policy.

The exact official board-selected zap is now installed by the gui/basic build
under `/etc/firmware/adreno/sc8280xp/LENOVO/21BX/`; ephemeral tests may still use
the same unchanged image in `/tmp`. See firmware/adreno/README.md for hashes.

## Display library integration

The prescribed gui build also produces `system/build_aarch64/q8b/bin/g2dscanout`.
Transfer it individually at 50 Kbit/s, verify MD5, then run `/tmp/g2dscanout`
through serial against the same initialized daemon. It links the actual
displayd library and uses only owned RAM as a simulated physical framebuffer.
It does not initialize a display daemon or write the real GOP framebuffer.

The targets are 64x64 with a 320-byte pitch inside an 80x72 allocation, and
128x128 with a 640-byte pitch inside a 160x136 allocation. Both use visible
origin (16,1). Tests check every pixel, row-padding word, prefix/suffix and
16 KiB guard: cropped/full pushes, 90/180/270 rotations, forced CPU-only
fallback and invalid-rect no-write behavior. Source pixels/guards must survive.
Daemon counter deltas must be exactly two `blit_phy` / three `rotate_phy`
for 64x64, and four `blit_phy` / three `rotate_phy` for 128x128. The small
rotation intermediate is non-contiguous by graph's allocation policy, so its
copy uses the CPU. At the 128x128 threshold, 90/270 decline, CPU-rotate, then
GPU-copy. In both cases 180 rotates directly. This detects
passing graph enums 1/2/3 instead of protocol angles 90/180/270 even when a
fallback would hide that mistake. CPU copies preserve pitch and apply the
visible offset once. This suite validates library integration, not full live
desktop startup or real framebuffer scan-out.

## Native GOP presentation

`g2dpresent` checks the real Q8B display path. Transfer the current built
`machines/q8b/system/build_aarch64/drivers/q8b/fbdisplayd` and
`system/build_aarch64/q8b/bin/g2dpresent` separately with `scp -l 50`, checking
each board MD5. With the original desktop idle and `/dev/g2d` already running,
start the new display daemon on a separate device through serial:

```text
/bin/bgrun /tmp/fbdisplayd-gputest -i 1 /dev/disp1
/tmp/g2dpresent-final /dev/disp1
```

Use `/dev/disp1` only if it is free; keep the original `/dev/disp0` and GPU
daemon running. This initializes a display mapping, never the GPU: neither
test executable contains Adreno or BSP GPU initialization symbols. The native
framebuffer must be BGR32, with matching contiguous test client/display
canvases. The existing GOP allocation is mapped using the ordinary BSP API.

The client draws via public Graph calls, presents two full frames plus a
dirty-rectangle update through actual display IPC, and compares every GOP
pixel to its independent CPU reference. It then restores the original desktop
canvas from `/dev/disp0` through the test display and verifies that readback.
It requires at least five fill and four canvas-blit requests, and exactly four
physical blits including restoration. The Q8B daemon currently has no dirty
flush hook, so the dirty client sync is followed by a full GPU scan-out copy.
`display_flush` returns zero on successful IPC, not a byte count; GOP readback
and counters establish rendering and physical-copy success.

This covers current display-daemon startup and actual GOP memory output.
Panel appearance and a complete rebuilt xserver/window-manager startup are
separate acceptance steps. The prescribed `gui` target rebuilds the display
stack but not the `x` target's statically linked desktop components.
