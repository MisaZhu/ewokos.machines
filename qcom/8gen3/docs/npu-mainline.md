# Q8B NPU port status

As of 2026-10-25, the SC8280XP NSP0 path supports signed firmware loading
through SCM PAS, SMP2P/IPCC readiness, GLINK and QRTR shutdown, FastRPC
unsigned USER_PD creation, reverse file/memory RPC, and apps_smmu DMA mappings.
Kernel DMA pins retain physical allocations until DSP access is revoked,
including owner exit and failed shutdown. Faults quarantine the transport;
failed secure stop or SMMU teardown retains the corresponding DMA storage.

The local `libadrenonpu` C API can import official opaque QAIRT contexts with
`execution_backend=htp_native`. This mode uses the native DSP adapter and
never falls back to the CPU. It owns one context per backend and does not
provide the Qualcomm AP binary ABI or online graph construction. The older
private graph protocol requires an explicitly selected matching skel.

CNN, residual CNN, MNIST and ten standard operators have passed hardware
correctness checks. Earlier performance runs completed 36,840 executions
including warmups, with output checking. Those results predate the logging
cleanup and are not performance guarantees for this revision. There is no
external power meter, so physical watts and joules have not been measured.

Repeated PAS restart can still trigger an NSP watchdog. Its root cause is
unresolved. A latched failure requires a board reset; successful shutdown
alone does not prove that restarting a failed transport is safe.
Extended convolution stress has also exposed a FastRPC response timeout that
latched recovery-required. Performance results do not establish transport
reliability; the demo evaluation records the failed attempt separately.

The cleaned production candidate builds from the selected source files
without local tools or vendor binaries in the source tree. Its hardware
regression passed 1,018 CNN/residual/MNIST samples, plus 27 CNN executions
through the public API across three context reloads. All comparisons passed
with maximum errors of 1/1/3 U8 LSB; shutdown returned PAS state 0.
Host regressions cover DMA pin ownership, FastRPC framing/lifetimes, opaque
context handling and malformed model metadata under ASan/UBSan. These checks
do not establish sustained PAS restart reliability.

The [nputest application](../../../apps/nputest/README.md) now owns the minimal
`npudemo` CLI and shared public native API regression/benchmark runner. Its
graphical window runs three bundled cases using resources under `res/`. See the
[demo evaluation](npu-demo-results.md) for measured workloads and timing scope.

## Build and runtime inputs

Build the kernel with `make -C machines/q8b/kernel ewokos="$PWD"` and build
the AP library with `make -C machines/q8b/system/libs/adrenonpu` after the
normal Q8B system headers and libraries have been installed.

The optional native DSP runtime uses external QAIRT headers and V68 libraries:

```sh
make -C machines/q8b/system qairt_native_build \
  QAIRT_SDK_ROOT=/path/to/versioned/qairt \
  HEXAGON_CLANG=/path/to/hexagon-capable/clang \
  HEXAGON_LD=/path/to/ld.lld
```

`qairt_native_install` installs only the locally built adapter into the
generated rootfs. Supply the official V68 `libQnnHtpV68.so`, `libQnnSystem.so`,
a board-compatible `fastrpc_shell_unsigned_3` (or signed shell), and their
matching runtime dependencies separately under `/usr/lib/dsp` or
`/etc/firmware`. The AP library loads these via its restricted read-only
listener. The SDK and board runtime must be compatible; QAIRT 2.51 with a
V68 runtime has been exercised on this board.

Place the signed `qccdsp8280.mbn` in `machines/q8b/firmware/npu` before the
normal rootfs firmware-install phase. This file comes from
`qcom/sc8280xp/qccdsp8280.mbn` in Radxa's firmware package; the exercised image
has MD5 `aa794aee23732765ca1ab8a97c069adc`.

Vendor firmware, SDK binaries and extensive experimental evidence
are not distributed with this port. The standalone `apps/nputest/res` directory
bundles the three demo models, validation inputs and reference outputs. Kernel
protocol tracing is disabled by default; define `Q8B_NPU_DEBUG=1` for a
diagnostic build. User-space DEBUG messages are delivered to an explicitly
registered log callback and omitted from the default system log.
