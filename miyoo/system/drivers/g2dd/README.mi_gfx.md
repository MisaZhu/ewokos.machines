# SigmaStar MI_GFX Notes

`G2DD_ENABLE_MI_GFX` only becomes linkable when all of the following are true:

- The build toolchain targets Linux user space, not `arm-none-eabi`
- The toolchain ABI matches the vendor libraries
- The SigmaStar MI libraries are available for that ABI
- The runtime provides the Linux kernel drivers and device nodes expected by `libmi_gfx.so` and `libmi_sys.so`

The extracted Miyoo firmware libraries have these properties:

- ELF32 ARM shared objects
- glibc dynamic dependencies such as `libc.so.6`
- hard-float calling convention (`Tag_ABI_VFP_args: VFP registers`)
- Linux syscalls through `open()` and `ioctl()`

Current EwokOS Miyoo user space has these properties:

- built with `arm-none-eabi-*`
- statically linked against EwokOS libc
- `-mfloat-abi=softfp`
- no Linux glibc dynamic loader

Because of that, the stock firmware `libmi_gfx.so` and `libmi_sys.so` cannot be linked directly into the current EwokOS `g2dd` binary.

Practical ways forward:

1. Build a Linux-side companion service with a compatible hard-float Linux ARM toolchain, then bridge EwokOS `g2dd` requests to it.
2. Continue reversing `mi_gfx.ko` and `mi_sys.ko`, then reimplement the needed subset natively for EwokOS.

The current `g2dd.c` keeps the MI_GFX backend source path in place so the logic can be reused once one of the above routes is available.

## Current EwokOS Native Path

`machines/miyoo/system/drivers/g2dd/g2dd.c` now prefers a native `ssd20xd-ge`
backend before any MI_GFX fallback. The current native path does the following:

- maps the GE MMIO block at `0x1f281200`
- allocates an ARGB8888 DMA canvas through EwokOS `dma_alloc()`
- translates ARM physical addresses to the GE-visible MIU bus window
- submits synchronous `FillRect` and same-size `BitBlt` jobs directly to GE
- falls back to the software renderer for unsupported cases such as scaled blits
  or alpha blending

The boot script also mounts `/drivers/miyoo/g2dd` at `/dev/g2d`, so the EwokOS
GUI-side `libg2d` and `g2dtest` path can use the new backend without extra
manual startup steps.

## Recovered ioctl set

The stock Linux user-space libraries are thin wrappers over these device nodes:

- `libmi_gfx.so` -> `/dev/mi_gfx` or `/dev/mi/gfx`
- `libmi_sys.so` -> `/dev/mi_sys` or `/dev/mi/sys`

Recovered command numbers from disassembly:

- `MI_GFX_Open` -> `ioctl(fd, 0x00006900, NULL)`
- `MI_GFX_Close` -> `ioctl(fd, 0x00006901, NULL)`
- `MI_GFX_WaitAllDone` -> `ioctl(fd, 0x40046903, &args)`
- `MI_GFX_QuickFill` -> `ioctl(fd, 0xC0306904, &args)`
- `MI_GFX_BitBlit` -> `ioctl(fd, 0xC0A86908, &args)`
- `MI_SYS_Init` -> `ioctl(fd, 0x80046900, &version_4)`
- `MI_SYS_Exit` -> `ioctl(fd, 0x80046901, &version_4)`
- `MI_SYS_Mmap` -> `ioctl(fd, 0xC018690A, &args)`
- `MI_SYS_Munmap` -> `ioctl(fd, 0x4008690B, &args)`
- `MI_SYS_MMA_Alloc` -> `ioctl(fd, 0xC0306919, &args)`
- `MI_SYS_MMA_Free` -> `ioctl(fd, 0x4008691A, &phy)`
- `MI_SYS_FlushInvCache` -> `ioctl(fd, 0x4008691B, &args)`

Recovered argument structure sizes:

- `MI_GFX_WaitAllDoneArgs_t` -> `0x04`
- `MI_GFX_QuickFillArgs_t` -> `0x30`
- `MI_GFX_BitBlitArgs_t` -> `0xA8`
- `MI_SYS_MmapArgs_t` -> `0x18`
- `MI_SYS_MunmapArgs_t` -> `0x08`
- `MI_SYS_MMA_AllocArgs_t` -> `0x30`
- `MI_SYS_FlushInvCacheArgs_t` -> `0x08`

## Direct Linux path

There is now a reverse-engineered direct Linux reference implementation in:

- `machines/miyoo/system/drivers/g2dd/linux_direct/mi_ioctl_re.h`
- `machines/miyoo/system/drivers/g2dd/linux_direct/mi_direct.c`
- `machines/miyoo/system/drivers/g2dd/linux_direct/g2d_linux_test.c`

This path bypasses `libmi_gfx.so` and `libmi_sys.so`, and talks to the vendor
kernel drivers with plain `open()` and `ioctl()`. It still requires a Linux ARM
hard-float runtime with the vendor kernel modules loaded, but it removes the
user-space SDK library dependency from the experiment path.
