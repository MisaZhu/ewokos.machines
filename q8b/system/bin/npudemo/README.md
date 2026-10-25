# npudemo

Small Q8B test client for the local `libadrenonpu` public C API. It imports an
official QAIRT V68 opaque context using `execution_backend=htp_native` and
checks that CPU fallback is disabled. It requires the firmware and DSP runtime
described in [npu-mainline.md](../../../docs/npu-mainline.md).

Build after the normal Q8B system headers and libraries are installed:

```sh
make -C machines/q8b/system/libs/adrenonpu
make -C machines/q8b/system/bin npudemo
```

The executable is installed in `system/build_aarch64/q8b/rootfs/bin/npudemo`.
No SDK, firmware or model binaries are included in this directory.

## Run

```sh
npudemo context.bin model input.bin expected.bin 9 0
npudemo context.bin model input.bin expected.bin 9 1000
npudemo context.bin model input.bin expected.bin 8 1000 173408256
```

Arguments are context binary, exact graph name, input corpus, expected output
corpus, sample count, timed iterations and optional MACs per graph. Zero timed
iterations selects API regression only. Counts are limited to 1–1000 samples
and 0–10000 timed iterations. Exit status is 0 on success, 1 on a regression,
execution or cleanup failure, and 2 on invalid command arguments.

Only UINT8 graph boundaries are supported (at most four inputs and four
outputs). Files contain contiguous native-layout tensors: samples in order,
then tensors in the order returned by `QnnGraph_getInputTensors` or
`QnnGraph_getOutputTensors`. The file size must exactly match the sum of tensor
byte sizes times the sample count. Each file and context is limited to 4 MiB.
For example, an NHWC input must be converted from source NCHW before packing.
Quantize each value as `clamp(round(real / scale) + zeroPoint, 0, 255)` using
the imported tensor's encoding. Expected outputs should come from an
independent reference implementation, with the same layout and output encoding.
The fixed acceptance tolerance is 8 U8 LSB; this is a numerical smoke check,
not a model accuracy evaluation.

Every invocation loads the context twice in one backend/session. Before any
benchmark it checks binary export/roundtrip, tensor count queries and undersized
arrays, context-owned handle protection, backend live-child protection, invalid
data sizes and execute counts, duplicate inputs when applicable, execution
counters, and every sample's output. The first context is unloaded and reloaded.
The second context remains live for warmup and measurement, avoiding a PAS
restart between regression and benchmark. Final context/backend cleanup must
also succeed for exit status 0.

## Measurement

After regression passes, the tool runs ten warmups followed by the requested
number of executions, cycling through the corpus. **Every** output byte is
poisoned and checked, including warmup and measurement. Any mismatch aborts the
benchmark. Use the final `PASS` and process exit status as the acceptance gate;
an earlier `API PASS` or `BENCH` line alone does not establish successful cleanup.

- `AP_graphExecute`: elapsed time immediately around the synchronous public API
  call, in microseconds. Includes AP tensor copies, FastRPC and DSP work; excludes
  input binding, output poisoning, reference checking and profile-property reads.
- `DSP_graphExecute`: the adapter's `native.last_dsp_us`, timing the official DSP
  `graphExecute` call. It is not an AP round-trip or a hardware cycle counter.
- Both report arithmetic mean, nearest-rank p50/p95, min/max and sequential FPS
  (`iterations / sum(time)`). AP intervals use the raw free-running ARM counter
  `CNTVCT_EL0` and its architectural frequency `CNTFRQ_EL0` (`counter_hz`,
  19.2 MHz on this board),
  converted only when reporting. An unavailable raw counter fails the run; there
  is no tick-clock fallback. DSP intervals have microsecond resolution.
- `checked_loop_fps` includes binding, poisoning, property reads and comparisons
  across the measured loop. Load/unload, warmup, sorting and console output are
  excluded from that loop.
- With a nonzero MAC count, `effective_GOPS = 2 * MACs / mean_us / 1000`.
  The caller must calculate and document the graph's nominal MACs. Dense Conv2D
  uses `N * Hout * Wout * Cout * (Cin / groups) * Kh * Kw`; sum the counted
  layers. State which non-MAC operations are omitted. This is effective graph
  throughput, not a measurement of the NPU's advertised peak TOPS.

The demo uses the runtime's default power policy and does not lock or sample
clock frequency. Results are workload/policy dependent; small graphs can be
dominated by RPC overhead. No watts, joules or energy efficiency are measured.
Transport timeout and PAS restart reliability remain unresolved; see the port status.
