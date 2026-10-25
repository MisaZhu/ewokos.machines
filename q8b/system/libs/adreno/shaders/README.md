# Built-in compute programs

`blit_blob.h` and `raster_blob.h` are generated from the corresponding `.comp`
sources using the current PGL compiler frontend, Adreno backend and serializer.
They accelerate alpha blending, scaling, transparent fills and rotation.

Regenerate from the repository root with native Clang and Make installed:

```sh
make -C machines/q8b/system/libs/adreno -f shaders/Makefile.host -j8
```

Alternatively, run `make builtin-shaders` in `libadreno` with the usual Q8B
build environment. Native compiler objects stay in `/tmp/q8b-builtin-shaders`;
override `OUT` to select another directory. Ordinary target builds use the
checked-in headers and do not require a native shader compiler build.

The generator validates the twelve-word uniform layout, SSBO count, workgroup
size and serialized header. The driver checks the generated protocol version
and header size at compile time. Regenerate both programs when changing the
wire protocol; changing just the version bytes cannot migrate an old header.

The previous version-2 programs were rejected by the version-4 driver, causing
transparent window composition to fall back to the CPU and block mouse input.
