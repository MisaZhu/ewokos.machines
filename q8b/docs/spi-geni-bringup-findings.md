# gaokun3 GENI (QUP0 SE6) SPI bring-up: findings

Status: **the GENI FIFO path works.** `himaxd` streams records through QUP0
SE6 on the device at ~22-24 fps using ~30% of one core, with no rejected
records.  This file records what was fixed, what was measured, and the traps
that cost the most time.

## Transport fixes that are correct and covered by host tests

`machines/q8b/system/libs/bsp/src/spi.c` (`q8b_spi_transfer`):

1. **RX-only transfers had no data at all.** The sequencer clocks `TX_LEN`
   bytes; an `RX_LEN`-only command completed immediately with an empty
   FIFO. The TX FIFO is now fed `0xff` for the whole length when the caller
   supplies no TX buffer.
2. **Partial last word.** `RX_STATUS[30:28]` holds the valid byte count of
   the last word (the ones before it are full). That word still costs one
   FIFO read; the old code dropped it, which shortened every 4-byte register
   read by one byte and shifted the whole value.
3. **Drain points.** The RX FIFO is emptied before a transfer starts, again
   when `CMD_DONE` is seen, and after a failed command. Without the
   start-of-transfer drain a stale word from a previous error makes the next
   value look byte-shifted.
4. **`pinmux()` no longer clears the pad pull/drive fields.** Only the
   function field is rewritten; the boot firmware's pulls are preserved.

`q8b_spi_open()` also no longer corrupts unrelated TLMM configuration.

Host regression: `system/libs/bsp/tests/spi_test.c` models a GENI engine
(TX-driven clocking, per-word RX FIFO, `RX_STATUS` valid-byte field). The
new RX-only and RX-drain cases fail on the previous `spi.c` and pass now:
`sh system/libs/bsp/tests/run-spi.sh`.

## Ruled out on the device (10.91.9.188)

Each of these was measured, not assumed:

- **SPI clock**: 100 kHz, 300 kHz, 960 kHz behave identically.
- **Pad pull**: pull none / down / up all give the same failure.
- **Packing**: recomputed from the kernel `geni_se_config_packing()` formula
  (`cfg0=0x0007f8fe`, `cfg1=0x000ffefe` for 8-bit, 4 words/FIFO word) —
  identical to the driver's values.
- **Register init**: `GENI_OUTPUT_CTRL`, `GENI_FORCE_DEFAULT`,
  `SE_GENI_CGC_CTRL`, `SE_DMA_GENERAL_CFG`, `SE_GSI_EVENT_EN`,
  `SE_IRQ_EN`, `SE_GENI_DMA_MODE_EN`, `SE_SPI_TRANS_CFG` (CS_TOGGLE
  cleared), `CPHA`/`CPOL`, `WORD_LEN`, `BYTE_GRAN`, `M_CLK`/`CLK_SEL` —
  all match `drivers/spi/spi-geni-qcom.c` and
  `drivers/soc/qcom/qcom-geni-se.c`.
- **Reset sequencing**: resetting the IC with the pads parked as plain GPIO
  (CS/CLK/MOSI high, MISO input, pulls matching `himax_gpio_open()`), with
  `gpio174` held low so the IC latches SPI while it boots, does not change
  the outcome. The GPIO master can always recover the IC afterwards, so the
  IC is never damaged.
- **`FRAGMENTATION`** (M_PARAMS bit 2, which holds CS across the command):
  this made consecutive reads consistent (12/12 identical) but the value was
  then always `00`. Linux only sets it for `cs_change` transfers, so
  unconditional use is wrong; the change was reverted.

## What actually made GENI read the IC

Two things had to change together; either one alone gives all-zero reads:

1. **Every engine command gets its own CS window on a GPIO-owned CS pad.**
   `Q8B_SPI_SW_CS`: `pinmux()` skips the CS pad, the engine demux is pointed
   at an unused channel, and the caller drives gpio157.  Freeing the CS pad is
   not enough by itself - each `spi_transfer` must be bracketed by CS low/high,
   which `himax_native_transport` now does in `himax_native_cs_xfer()`.
2. **The full Himax AHB read sequence.**  One register read is four separate
   CS windows: CONTI=0x31, INCR4=0x10, the 4-byte address, direction=0x00, and
   only then reading RDATA.  Reading RDATA alone returns stale data (observed
   as the `ed fe ad de` filler), which is what made earlier GENI attempts look
   like a transport fault.

With both in place, over GENI: `himax_reg_read(0x900000d0)` returns
`00 1a 12 83`, `himax_interface_on()` passes and `himax_chip_detect()`
reports `ic_id=0083121a`.

### SPI mode sweep (CPOL x CPHA)

Docs and the working bit-bang master both specify **mode 3**.  Measured over
GENI with GPIO CS, two reads per mode:

| CPOL | CPHA | Result |
| --- | --- | --- |
| 0 | 0 | `00 1a 12 83` (works) |
| 0 | 1 | `00 00 00 00` |
| 1 | 0 | `00 00 00 00` |
| 1 | 1 | `00 1a 12 83` (works - mode 3, as specified) |

Mode 3 is confirmed correct; CPHA is the discriminating bit for this IC.

## Why the firmware looked stuck in safe mode

The GENI path appeared to hang in SAFE_MODE (`FW_STATUS` 0x0c) with all-zero
frames.  It was not a firmware problem: **the bring-up sequence itself put a
perfectly healthy IC into safe mode.**

The boot firmware leaves the touch IC running and streaming; a plain record
read returns a complete, valid frame (`mark=a55a`, ~4800 non-zero grid bytes,
6/6 reads).  Running `chip_detect` + `disable_fw_reload` +
`power_on_init_skip_reload` against that live IC drops it into safe mode
(0x0c), after which every frame is empty.  The GPIO master never showed this
because its probe path issues a full reset first, which restarts the firmware.

`himaxd`'s native probe now **reads a frame first** and only runs the bring-up
when that fails (`himaxd.c`, `probe_ic()`), so a streaming IC is left alone.

Measured with that change, `himaxd --native` on /dev/touch: frames
160 -> 192 -> 224 over 15 s, `invalid 0`, ~8 fps, 35-40% of one core.

## Clock and frame rate

The record read was the dominant cost, and it is pure clock time (a bare
5132-byte transfer measures the same as a full record read):

| SCK | record read | result |
| --- | --- | --- |
| 960 kHz | 42.9 ms | 8 fps @ 44% of one core |
| 2.74 MHz | 15.0 ms | |
| 4.8 MHz | 8.6 ms | 8/8 records accepted |
| 9.6 MHz | 5.4 ms | 8/8 records accepted, 22-24 fps @ ~30% |

9.6 MHz is the fastest the QUP0 SE6 divider reaches from its 19.2 MHz source
and is well under the IC's 12.5 MHz rating.  At that speed the panel's own
refresh (~20 fps) is the limit, so the sampling budget is 20 ms; do not
slow it down to dodge torn reads.

## Sync on the record marker, not on the clock

The event-stack port is a stream: a read that a panel update overtakes comes
back shifted, so the 0xa55a marker is not at offset 0.  `himax_frame_valid()`
rejects that, and `himax_read_frame_synced()` reports it as
`frame-no-marker`.  Every record also carries a 16-bit sequence word at
offset 2 that changes on each panel update; the daemon skips records whose
sequence it has already processed (`duplicates` in `/tmp/himaxd-status`), so
a read never re-feeds the solver a frame it has seen.

The Linux driver instead reads inside the record interrupt.  That is not
usable here: gpio175 pulses for only about 0.3% of the time (310 low samples
out of 100 000 polled at 10 us), so a userspace poller never catches it.
Marker and sequence checking is the polled equivalent and costs nothing.

## The sub-second stalls: nothing that talks to another server in the sample path

The last symptom was an occasional delay of a second or more between touches,
with whole frames missing, on an otherwise healthy sensor (invalid 0,
duplicates 0, ~18 fps in the counters). It looked like a slow read, a panelled
update, or a kernel clock problem, and it was none of them.

The way to find it was to stop guessing which span was slow and make the spans
*partition* the gap: stamp the fine clock (`kernel_tic_nsec`, 19.2 MHz on this
SoC) at the sampling point, at the start and end of the budget sleep, at the
start and end of the read/solve work, and at the iteration exit. `gap` is
measured on the millisecond clock and the fine clock at once, so an artefact
and a real stall can be told apart - they disagreed by less than 1 ms.

Two independent causes, both measured on gaokun3:

1. **`write_status()` was called from the sampling loop** every 32 frames. Its
   `fopen`/`fputs`/`fclose` is three synchronous IPCs to the filesystem server
   (`vfs_sync`, `FS_CMD_CLOSE`, `VFS_CLOSE`), and EwokOS publishes on close, so
   a single slow reply lands *between two frame reads*. Measured 0.5-1.3 s per
   write against a 30 ms budget: sampling stopped for exactly that long and
   every frame in the window was lost. The counters now accumulate in memory
   and are published from `touch_poll()`, which runs on the IPC thread.

2. **The probe retry backoff was capped at 30 s.** A panel that missed one
   probe parked the sampler for up to half a minute. The first retry is now
   50 ms, the cap 250 ms - well under the delay a user can feel.

The rule this leaves behind: *nothing that talks to another server may sit
between two frame reads.* Any VFS, `slog` or other IPC call in that window
becomes a frame gap of whatever the other server's latency happens to be.
After the fix: 24-25 fps, 12-18% of one core, no gap over twice the budget
across ~6 minutes of sampling.

Two measurement traps worth remembering, because both produced convincing but
wrong numbers first:
- `kernel_tic_ms` is tick-quantized (one tick is ~990 us here). A gap measured
  on it alone can look like a stall; always confirm with `kernel_tic_nsec`.
- The status line is written through a fixed-size `snprintf` buffer. When it
  grew past that buffer the last fields silently vanished, and a `?:` that was
  missing its parentheses swallowed the two arguments after it - the file
  looked complete while two counters were never printed at all.
