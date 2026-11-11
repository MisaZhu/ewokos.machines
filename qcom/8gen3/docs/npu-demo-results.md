# Q8B npudemo evaluation, 2026-10-25

The minimal public-API client and its metric definitions are documented in
[nputest / npudemo](../../../apps/nputest/README.md). Tests use the production native HTP
adapter and official QAIRT 2.51.0.260929 V68 contexts, with CPU fallback disabled.
No kernel or driver changes were made for these tests.

## API regression

CNN (9 samples), residual CNN (9 samples, two inputs/two outputs) and MNIST
(1,000 samples) pass two context loads each: 2,036 executions. Maximum errors
are respectively 1, 1 and 3 U8 LSB against independent references; tolerance is 8.
Checks cover binary roundtrip, tensor queries/capacity, borrowed-handle and
live-child protection, bad sizes/counts, duplicate inputs, execution counters,
reference outputs, context reload and final cleanup.

Host ASan/UBSan checks exercise the production API with mocked DSP boundaries
and the demo's API-only/timed paths, decimal bounds/overflow, corpus-size
validation, execution failures, bad-reference benchmark gating and cleanup.
Synthetic counter intervals also verify the conversion to microseconds and GOPS.
Host measurements are not NPU benchmark data.

## Comparable 1,000-execution windows

After recovery and a fresh board boot, each graph passes its two-load API
regression (8 samples per load), ten warmups and 1,000 timed executions. Every
output is checked, including warmup and measurement. Both runs complete with
successful context/backend cleanup. Runs execute sequentially, 112 first and
then 56; file transfers finish before timing. Default runtime power policy,
no frequency lock, no optional DSP profiling, batch one, no external power meter.
The final sequence, including the three API-only fixtures, completes 4,088
checked executions; the final PAS state is 0.

Both synthetic networks contain six dense 3x3 Conv+ReLU layers, 32 input/output
channels, stride one, SAME padding and groups one, followed by global average
pooling and flatten to 32 outputs. Activations and weights are quantized to
8 bits. MACs count convolution only:
`6 * H * W * 32 * 32 * 3 * 3`. ReLU, pooling and layout operations contribute
execution time but no counted MACs. Effective GOPS use two operations per MAC.
These are random-weight compute workloads, not trained accuracy benchmarks.

| Spatial size | MACs/graph | AP mean / p95 (us) | DSP mean / p95 (us) | AP effective GOPS | DSP effective GOPS | Checked-loop FPS | Max error (LSB) |
|---|---:|---:|---:|---:|---:|---:|---:|
| 56x56 | 173,408,256 | 1,979.614 / 2,082.865 | 106.520 / 137 | 175.194 | 3,255.882 | 505.063 | 3 |
| 112x112 | 693,633,024 | 2,051.513 / 2,111.979 | 319.396 / 329 | 676.216 | 4,343.405 | 487.361 | 4 |

AP intervals use raw `CNTVCT_EL0` ticks at the architectural `CNTFRQ_EL0`
frequency, measured as 19,200,000 Hz. DSP intervals time the official
`graphExecute` call with `HAP_perf_get_time_us`. AP includes transport and AP
copies; the DSP throughput column does not describe application throughput.
The AP/DSP time difference points to substantial overhead outside the DSP call;
this test does not break that overhead down further.

## Longer runs and limits

A prior 56x56 run using the same raw-counter timing path completed 10,000 timed
executions with output checking: AP mean 3,131.868 us, p95 3,267.813 us,
110.738 effective GOPS; DSP mean 237.915 us, p95 272 us, 1,457.731 effective
GOPS. This predates the addition of failure-counter diagnostics to the demo.
The difference from the shorter windows demonstrates run-to-run variability;
clock/thermal state was not recorded, so its cause is not established.

Two 112x112 attempts at 10,000 timed executions fail with a 30-second FastRPC
response timeout and PAS state `0x40` (recovery-required). The second attempt
starts after a full board reset, with this workload first. Both context-load
regressions pass before measurement; the second attempt reports 9,032 completed
executions in the live context before failure. The tool returns failure and
emits no benchmark aggregate for these incomplete windows. No corresponding
NSP watchdog interrupt is present in the captured failure logs. The transport
failure's root cause remains unresolved; a board reset restores operation.

The valid windows above establish correctness and workload throughput for those
runs. They do not establish long-run transport reliability, fixed-frequency
performance, advertised peak TOPS, physical watts or joules.

## Workload and runtime provenance

Weights use NumPy 2.5.3 `default_rng(20261025)`, normal distribution with standard
deviation `sqrt(2 / (32 * 9))`, generated layer by layer in ONNX OIHW order.
Calibration and validation inputs use separate `default_rng(91)` and
`default_rng(271)` streams, uniform [-1, 1), float32, with 16 and 8 NCHW samples.
Source ONNX uses opset 13/IR 8. Expected outputs come from float32 ONNX Runtime
1.31.0 on the original validation samples, transformed to native layout and
rounded/clipped to the official output encoding. Input files use the converter's
native NHWC layout and encoding. All deployed input/context/reference files
are checked against host MD5 before running.

| Artifact | SHA256 |
|---|---|
| 56x56 ONNX | `e8ddfe3470230e678d345a9c927d194667d4163c71d0ef91defffdbfa3566333` |
| 56x56 context | `126065b6c66b95df5d759abfb0c4e8f0288900120ec09d3954d3d237b01afdf1` |
| 112x112 ONNX | `90138b697bbd69bfc83c96b764d070d142cc8e04e589acaf8ba9f6175acf7d9e` |
| 112x112 context | `2db3f27b34d748a2c94efb79f48e00674accd0019b2e1766f299c236edc62c03` |
| Kernel binary | `0d5d603e66f4e100c8f309edb290a250585f633f6b9dddbd7613d6fc8f94097f` |
| DSP adapter | `28476c2379bff20b68e7b7ed5730abc91febfe819077916825dc5cde09e571f6` |
| Final AP demo | `8d825825e73ba43e547ebae1007fe7dfd7c9d378bfcb6e143263c4a1e4ce9e2d` |

From each workload's directory, reproduce the measured windows with:

```sh
npudemo context.bin model input.bin expected.bin 8 1000 173408256
npudemo context.bin model input.bin expected.bin 8 1000 693633024
```

Vendor runtimes, conversion helpers and full experimental logs remain external
inputs/evidence. The migrated `apps/nputest/res` directory now bundles the CNN,
56x56 and 112x112 contexts, source ONNX, validation inputs and references used
by its graphical three-case suite.
