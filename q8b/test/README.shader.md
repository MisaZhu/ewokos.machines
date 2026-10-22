# Q8B compute shader smoke test

This tests the userspace GLSL -> NIR -> Mesa ir3 -> program-upload -> compute
dispatch path. It does not modify or interpret any firmware. The kernel and
official GMU/SQE/zap images are unchanged from the successful A2D baseline.

The projects worktree currently points at `f8ad7121`, which predates the shared
compiler frontend. The root's staged projects gitlink, `e08c8b50`, contains the
matching `pglcompiler` contract. Export only that dependency into a temporary
directory, leaving the worktree and gitlinks untouched:

```sh
q8b_shader_dir=$(mktemp -d /tmp/q8b-shader.XXXXXX)
git -C projects archive e08c8b50effa10fbf6c347691191319af91804f4 \
  soft3d/libs/gles/pglcompiler | tar -x -C "$q8b_shader_dir"
q8b_shader_fe="$q8b_shader_dir/soft3d/libs/gles/pglcompiler"
q8b_shader_root="$PWD"
gmake -j8 -C "$q8b_shader_fe" \
  -f "$q8b_shader_root/machines/q8b/test/Makefile.compiler-host" \
  PART=frontend FE_DIR="$q8b_shader_fe" OUT="$q8b_shader_dir/out" HW=q8b
gmake -j8 -C machines/q8b/system/libs/adrenocompiler \
  -f "$q8b_shader_root/machines/q8b/test/Makefile.compiler-host" \
  PART=backend FE_DIR="$q8b_shader_fe" OUT="$q8b_shader_dir/out"
gmake -C machines/q8b/system/libs/adrenocompiler \
  -f "$q8b_shader_root/machines/q8b/test/Makefile.compiler-host" \
  PART=backend FE_DIR="$q8b_shader_fe" OUT="$q8b_shader_dir/out" shader-host
"$q8b_shader_dir/out/compile_shader_host" \
  machines/q8b/test/cs_smoke.comp "$q8b_shader_dir/cs_smoke.bin"
make -C machines/q8b/system gui
```

The host harness uses the device compiler source lists and feature config,
with host libc, assertions enabled and separate output directories. It does
not install compiler libraries into the EwokOS SDK. The result for this source:

```text
blob=880 binary=384 instrlen=3 constlen=4 local=8,8,1 ssbo=1 ubo=0 uniform=0 private=0
MD5 cs_smoke.bin: 01b592f819bb6318acbfd8ac5af2ca9f
```

This is a regular application shader, not firmware. Each invocation writes one
ARGB8888 pixel of an integer coordinate pattern to binding 0. The client checks
all 4096 pixels against independent CPU expectations and the following 16 KiB
guard region. CPU code never renders the image. Transport requires a three-vertex
canvas; the fullscreen compute path does not consume its contents.

Copy `gpushader` and the blob individually using `scp -l 50`, verifying each
with board `/bin/md5`. Start the following commands only through serial, against
the existing initialized g2dd:

```text
/tmp/gpushader /tmp/cs_smoke.bin --upload-only
/tmp/gpushader /tmp/cs_smoke.bin
```

`--upload-only` validates the IPC contract and backend parser, allocating and
copying shader memory but submitting no GPU commands. It passed on the old
A2D-verified driver at 16:54:07 on 2026-10-22, returning handle 0. That is not
evidence of a completed shader dispatch or pixel write.

The new driver adds Mesa's A690 static register table, SP/TPL1 GL mode, HLSQ
constant state, immediate state mode, disabled retained draw groups, and UCHE
invalidation before each compute dispatch. Those are UMD rendering state,
separate from already-verified Linux-style firmware/core initialization.
After replacing the driver, physical off/on is required by HANDOVER.md's
one-initialization-per-cold-boot test rule. First repeat `gpusmoke --extended`,
then run one compute draw. Stop on any error. On a submitted-job timeout the
new client retains its buffer mappings and sleeps until physical off/on, so
potentially active GPU addresses cannot be reused by client teardown.

On 2026-10-22 at 17:10:58 the basic shader passed all 4096 pixels and the
16 KiB guard. At 17:16:59 the same driver passed `cs_uniforms.comp` on
64x64, 65x37, 320x240 and 720x720, four changing uniform sets per size,
with an A2D fill before each CS draw. Every pixel, row-padding word and
trailing guard matched. The driver MD5 was `33b01e3bce8dc6017bdeca4fff8ebb08`;
no reinitialization occurred between tests.

Build and run the additional uniform and two-pass geometry tests:

```sh
"$q8b_shader_dir/out/compile_shader_host" \
  machines/q8b/test/cs_uniforms.comp "$q8b_shader_dir/cs_uniforms.bin"
"$q8b_shader_dir/out/compile_shader_host" \
  --geometry "$q8b_shader_dir/geom_smoke.test"
make -C machines/q8b/system gui
```

```text
/tmp/gpushader-uniforms /tmp/cs_uniforms.bin --uniforms
/tmp/gpushader-geom /tmp/geom_smoke.test --geometry
```

Transfer each artifact separately with the same MD5/serial procedure above.
The geometry host mode compiles the shared `geom_xform_color_cs_glsl` and
`geom_raster_cs_glsl` unchanged. Its test envelope holds reflected uniform
offsets followed by a regular VERSION 2 program (3596 bytes, raster 16x16,
xform 8x8, three SSBOs each, no private memory). The client uploads only
that enclosed program. It checks full color/depth surfaces, GPU frame clears,
far rejection, near replacement, depth-write disable, cull-all, MVP translation,
input preservation and guards on 64x64 and 320x320 targets. Depth allows two
UNORM LSBs for float conversion; color and coverage checks are exact.

Geometry hardware results are recorded in HANDOVER.md. Full 3D scenes and
display integration remain unverified until corresponding tests succeed.
