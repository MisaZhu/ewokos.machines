# Huawei MateBook gaokun3 Himax HX83121A touchscreen

`himaxd` serves the gaokun3's SPI touchscreen as `/dev/touch0`.  The
driver lives under `machines/q8b/system/drivers/`, next to `goodixd`: the
q8b machine tree serves both panels and `/etc/init.rd` selects the active
one (TW220 default -> goodixd; gaokun -> uncomment the himaxd entry).  The
TW220 Goodix driver is a different IC and is not changed; both daemons speak
the same six-byte record contract, so `xtouch` and the X server work with
either.

## Hardware

| Signal | gaokun3 |
| --- | --- |
| Touch IC | Himax **HX83121A** cascade controller (raw-frame output) |
| Bus | QUP0 SE6 (`spi@998000`), SPI mode 3, 8 bit, 12 MHz in Linux |
| MISO / MOSI / CLK / CS | TLMM gpio154 / 155 / 156 / 157 (`qup6` function) |
| Interrupt | TLMM gpio175, level-low pulse (measured ~25 us, ~120 Hz) |
| Reset | TLMM gpio99, active-low |
| Host-interface select | TLMM gpio174: low = SPI, high = I2C-HID |
| Panel power | always on in this board design (no enable GPIO) |
| Panel | CSOT ppc357db1-4, 2560 x 1600 (native portrait 1600 x 2560) |
| Raw frame | 60 x 40 capacitance nodes, 5132-byte stream record |

Sources: `sc8280xp-huawei-gaokun3.dts` from the working gaokun3 kernel tree
(`himax,hx83121a-ts`, `spi6_default`, `ts0_default`), the gaokun3 touch patches
in `../q8b/easy-for-gaokun/patches/`, and `../q8b/EGo-Q8B移植关键信息.md`.

Two board facts matter:

1. The mainline device tree describes `hid-over-i2c@4f`; the real hardware is
   SPI. The IC latches its host interface from gpio174 **before** the touch
   firmware reloads. Boot firmware leaves it high, so the IC answers SPI
   register reads but never reports touch data. `himax_gpio_open()` drives
   gpio174 low (2 mA, no pull) before any reset. Verified on hardware: with
   gpio174 high the interrupt never fires and no record is readable; with it
   low frames arrive normally.
2. In SPI mode this panel only exposes the raw capacitance stream. The
   processed 56-byte touch report used by the mainline `himax_hx83112b`
   driver was implemented and tested on the device: after a plain reset, and
   also after a firmware restart, the IC reports no events and never raises
   its interrupt, even while a finger draws on the panel. All touch solving
   therefore happens on the host, which is why this driver carries a full
   algorithm. (The community Linux driver reaches the same conclusion and
   ships the same algorithm.)

## Transport: GPIO bit-bang, not GENI/GPI

The controller sits on the GENI QUP0 SE6 serial engine. On this platform the
engine has FIFO mode disabled (`GENI_IF_DISABLE_RO`), so Linux forces GPI DMA;
that path needs a GPI channel, an SMMU stream mapping and interconnect votes.
On SC8280XP an unprepared GENI MMIO access already hung a machine during TW220
bring-up (`machines/q8b/skills/q8b-tw220-spi-bringup`), and none of that is
validated for QUP0 SE6 here.

`himaxd` maps **only TLMM** and bit-bangs SPI mode 3: CS low per access, data
changes while the clock is high, the IC shifts on the falling edge, the master
samples MISO on the rising edge.

Measured on the device with the diagnostic tool:

| Measurement | Value |
| --- | --- |
| Register access | AHB burst registers read back, IC ID `HX83121a` |
| One 5132-byte frame | ~16-19 ms (261 KiB/s, ~2.1 MHz equivalent) |
| Interrupt | ~25 us low pulse at ~120 Hz |
| Accepted frames (idle, 80 ms budget) | ~8-12 fps, ~29% of one core |

## CPU budget / tuning

One bit-banged frame dominates the cost (~16-19 ms), so CPU load is roughly
frame rate × read time.  The daemon samples on a fixed budget (default
**80 ms**, `HIMAX_FRAME_MS` env or the optional `frame-ms` argument) and
sleeps between samples instead of spinning.

Measured on the device (single daemon instance, idling):

| Transport / sync | fps | Idle CPU | Torn frames |
| --- | --- | --- | --- |
| busy-wait read + 10 ms pulse spin | ~8-12 | ~29% | 1.2% |
| **gap read (no pulse spin) + budget sleep (yield)** | ~10-11 | **~19-20%** | ~0 steady (startup only) |
| + `usleep(500)` every 512 B mid-read | - | ~20% | tears (panel rejects) |

Answering "is the GPIO SPI busy-waiting?":

* The byte loop itself **must** be continuous (it drives the wires; there is
  no way to sleep between clock edges).  The `HIMAX_SPI_YIELD` env selects
  how the loop behaves at 512-byte boundaries inside a CS-window:
  `0` = pure busy-wait (fastest, the default), `1` = `sched_yield()` (shares
  the core during the read, microsecond gaps), `2` = `usleep(500)` (tears the
  panel's raw record stream - do not use).
* The old 10 ms pulse-wait spin was a pure busy-wait and is gone.  On this
  panel the level-low pulse marks the record being *rewritten*: reads that
  start on a caught pulse tear more, so the daemon no longer chases it - the
  budget `usleep` between frames is the settle gap (validated: ~0 steady
  torn frames).  This is what cut CPU to ~20%.

A tap shorter than the budget can fall between two samples and be missed; the
tap latency is one budget period.  5/5 quick taps registered at the 80 ms
default.  Lower `HIMAX_FRAME_MS` for more responsiveness, raise it to save
CPU.

Two operational rules learned the hard way on the device:

* **Exactly one daemon may run.**  Starting a second instance bit-bangs the
  same pads and every read tears; symptoms are exactly the "90% torn frames"
  mode.  If `ps` shows several `/tmp/himaxd-*`, kill each by its exact
  basename (`killall himaxd`, `killall himaxd-a`, ...) or pid.
* A killed daemon can leave an orphaned `/dev/touch0` that blocks
  re-registration until reboot; after a forced kill, remove the node and also
  restart `xtouch` (a stale client fd silently steals the records).

The read is the bottleneck: it takes longer than one panel frame. That has a
consequence the first implementation got wrong:

* **A read must start right after the interrupt pulse.** Reads started at an
  arbitrary moment return a misaligned slice of the stream: measured 29 of 40
  such reads were corrupt (no `0x5aa2` record marker, garbage grid).
* **Every record is checked before use.** `himax_frame_valid()` requires the
  `0xa55a` marker word and at least 90% of the grid inside the 0x7e00..0x8200
  baseline window. A rejected record is retried, never fed to the solver.
  Before this check the solver turned corrupt slices into ~30 false touches/s;
  afterwards, idle measurements show zero contacts over thousands of frames.
* If the panel pauses its pulses, the daemon reads anyway and lets the record
  check decide, because skipping would drop a whole quick tap.

## Initialisation and frame processing

Ported from `chiyuki0325/EGoTouchRev-Linux` (`himax-spi-core.c`), GPL-2.0,
commit `475a6db2b58b`:

```
gpio174 low -> CS/CLK idle high
reset pulse (low 20 ms, high 50 ms)
interface_on     wake the bus, read back AHB CONTI(0x31) / INCR4(0x10)
sense_off        request the firmware to stop, enter safe mode (0x9527)
chip_detect      read IC ID 0x900000d0, expect HX83121a
disable_fw_reload  write 0x5aa5 to 0x10007f00 (IC keeps its own flash image)
power_on_init    sorting mode 0, nframe 1, reload status 0, firmware re-init,
                 pin reset, wait for 0x100072c0 == 0x000072c0 (<= 300 ms),
                 raw-out select 0xf6 at 0x100072ec
```

`hx_algo.c` is a direct port of the community algorithm (baseline subtraction
from 0x7ffe, CMF, GridIIR, 8-connected macro zones, palm rejection,
asymmetric peak detection, weighted centroid, greedy tracking). The maths is
unchanged; only Linux types/helpers were replaced (`hx_compat.h`).

Because one frame is ~40 ms here, the tunables that assume 120 Hz were
adjusted: new contacts are reported on their first frame (`debounce_base = 0`,
`track_start_debounce = 1`), a lift is released after two missed frames, and
the detection thresholds were lowered to 600 (idle noise measured ~±120).

Contacts come out in panel pixels: solver x in 0..2560 (60 grid columns) and
y in 0..1600 (40 grid rows). Orientation is not compiled in; it lives in
`/etc/x/xtouch-himax.json`. Measured by tapping all four screen corners:

| Screen corner | Driver (x, y) |
| --- | --- |
| top-left | 2485, 1502 |
| top-right | 110, 1462 |
| bottom-left | 2441, 93 |
| bottom-right | 43, 55 |

Both axes are inverted relative to the desktop, so the installed config is
`x_rev: true`, `y_rev: false`, `x_max: 2560`, `y_max: 1600`.

## Reporting

`/dev/touch0` records are `{uint16 state, uint16 x, uint16 y}`, three fields,
one contact, exactly like the TW220 driver. The solver tracks up to ten
contacts but only the first stable one is reported; the existing contract and
`xtouch` client have no multi-contact frame. A release keeps the last contact
coordinates so `xtouch`/`xserverd` can still synthesise a click at the press
point.

Startup order in `/etc/init.rd`:

```
@/bin/ipcserv /drivers/q8b/himaxd /dev/touch0
@/bin/bgrun /sbin/x/xtouch -c /etc/x/xtouch-himax.json /dev/touch0
```

Only one process may read `/dev/touch0`: a second reader (for example a
leftover diagnostic) steals the records and the desktop stops responding to
taps. Restart the client after restarting the daemon, because an open fd
belongs to the previous device instance.
