# powerd - TW220 battery status daemon

Battery charging/capacity status for the Robo & Kala TW220 (Qualcomm
SC8280XP), modeled on the uConsole powerd.  It serves `/dev/power0` with the
same 3-byte protocol statusbar's PowerInfo widget already consumes:

```
byte0 = battery present, byte1 = charging, byte2 = capacity percent
```

The reference design (uConsole) reads an AXP223 PMU over I2C.  The TW220 has
no usable I2C/GENI path - goodixd established that only TLMM GPIO and the
SPMI arbiter are validated on this board - so this driver reaches the power
hardware over SPMI with the same core/cfg/obs window pattern goodixd uses.

## Hardware paths (verified on device)

| Fact | Status |
| --- | --- |
| SPMI arbiter v5 windows (core `0x0c440000`, cfg `0x0c40a000`, obs `0x0e600000`) | Verified by goodixd and by this driver |
| APID map: 499 channels in 512 slots, SIDs 0/1/2/3/4/6/8/10/12, ALL EE0-owned | Verified every boot (map contents shift between boots - enumerate at runtime) |
| Observer reads on all channels | Safe, proven by `--dump`/`--probe`/`--read` sweeps; SID6 channels transaction-fail cleanly (st=3), never hang |
| Charger status CHGR `0x15`/`0x46` reg 0x06 bits[2:0], BATIF `0x48` reg 0x10 | Read on device: charger INHIBIT (0x06=0x00, EN_CMD off), battery present |
| Fuel gauge FG `0x40` SOC 0x09 / VOLT 0x0A | Read on device: both FGs dormant (all zero) - never enabled by boot firmware |
| RW-window writes to battery PMIC channels | **PROVEN UNSAFE** (see below) |
| RW-window writes to GPIO `0x88` on SID1 | Work (goodixd's enable_vcc3b runs every boot) |

## Write path is proven unsafe - do not write

Two controlled experiments froze the board (network and UI dead, reboot
required), while the write command format was verified byte-for-byte
against mainline `spmi-pmic-arb.c` v5 and all channels are EE0-owned:

1. ADC5@SID8 (apid 269): the first RW-window write ever issued froze the
   system silently mid-write.
2. FG@SID1 (apid 82, reg 0x09): a write-back-same-value probe (write the
   value just read) via the streamed `--write` diagnostic froze the system
   again - output stopped at the `writing` line, post-read never printed.

goodixd writes the same RW window on the same board (GPIO@SID1) and works,
so the stall is specific to the FG/ADC blocks of the battery PMICs in this
firmware state - likely the peripheral stalls the SPMI link on a write
frame, which wedges the arbiter and then the SoC fabric.  The kernel's
EE0-ownership gate (now mirrored in `spmi_write`) would not have prevented
this: every channel is EE0-owned.  Therefore:

- the daemon path is fully read-only and must stay that way;
- `--write` and `--adc` remain as diagnostics but are known to wedge the
  board on battery PMIC channels - only run them knowing a reboot follows;
- capacity cannot be enabled from userspace: the FG needs an enable write
  sequence that this board cannot survive.  Capacity reads 0/unknown until
  some firmware ever enables the FG, at which point the ALG registers
  become readable for free.

## What the daemon reports

- `battery` - from BATIF INT_RT_STS (0x48/0x10, present = !(bit4|bit5)).
- `charging` - CHGR status (0x06 bits[2:0] in TRICKLE/PRE/FULLON/TAPER).
- `capacity` - from FG SOC_MONOTONIC when the FG reports a live value
  (LSB 0.00390625 %, full = 25525); 0 when the FG is dormant.

## Bring-up verification

Run on the device (all safe, read-only):

```
/drivers/8gen3/powerd --dump
/drivers/8gen3/powerd --probe
/drivers/8gen3/powerd --read <apid> <reg> <len> ...
dev.cmd /dev/power0 status
```

## Notes

- The APID map is reprogrammed by boot firmware per boot; always enumerate
  at runtime, never hardcode apids.
- Sampling period is 1s.  `--dump` and the `status` command re-sample
  synchronously.
- The TW220 battery is a 2S pack; the FG VOLTAGE_NOW plausibility window
  (2500..9800 mV) accepts it.
