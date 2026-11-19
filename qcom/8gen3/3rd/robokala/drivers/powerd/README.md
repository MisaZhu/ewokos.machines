# powerd - TW220 power daemon (battery + power key + keyboard lid)

The single power daemon for the Robo & Kala TW220 (Qualcomm SC8280XP).
It replaces the former `system/drivers/powerd` (battery only) and owns
everything power-related on this board:

- **battery status** on `/dev/power0`, the same 3-byte protocol the
  uConsole powerd serves and statusbar's PowerInfo widget consumes:
  `byte0 = battery present, byte1 = charging, byte2 = capacity percent`;
- **power key**: short press toggles display sleep/wake, 3 s hold
  requests PSCI SYSTEM_OFF;
- **keyboard lid**: hall sensor closed -> display sleep, opened -> wake;
- root-gated `reboot` / `shutdown` dev commands (PSCI).

Built standalone like goodixd; the binary lands in
`3rd/robokala/build/drivers/robokala/powerd` and is deployed to
`/drivers/robokala/powerd` on the persistent rootfs.  robokala carries
its own boot config (`3rd/robokala/etc/init.rd`); both it and the machine
profile launch exactly one powerd:

```
@/bin/adspas
@/bin/ipcserv /drivers/robokala/powerd /dev/power0
```

The ADSP boot (`adspas`) must precede powerd: the real battery capacity
comes from the ADSP firmware battery manager (PMIC GLINK battmgr).

The key and the lid are polled on a 50 ms tick, the battery once per
second on the same tick, independently of xserverd, so they always wake
the screen again.  Sleep is panel-off + paused compositor/input
(`sleep`/`wake` dev commands to `/dev/x`), NOT suspend-to-RAM: this WoA
firmware exposes no PSCI SYSTEM_SUSPEND door, so the SoC keeps running
while the screen is dark.

## Battery: hardware paths (verified on device)

The reference design (uConsole) reads an AXP223 PMU over I2C.  The TW220
has no usable I2C/GENI path - goodixd established that only TLMM GPIO and
the SPMI arbiter are validated on this board - so `power_pmu.c` reaches
the power hardware over SPMI with the same core/cfg/obs window pattern
goodixd uses.

| Fact | Status |
| --- | --- |
| SPMI arbiter v5 windows (core `0x0c440000`, cfg `0x0c40a000`, obs `0x0e600000`) | Verified by goodixd and by this driver |
| APID map: 499 channels in 512 slots, SIDs 0/1/2/3/4/6/8/10/12, ALL EE0-owned | Verified every boot (map contents shift between boots - enumerate at runtime) |
| Observer reads on all channels | Safe, proven by `--dump`/`--probe`/`--read` sweeps; SID6 channels transaction-fail cleanly (st=3), never hang |
| Charger status CHGR `0x15`/`0x46` reg 0x06 bits[2:0], BATIF `0x48` reg 0x10 | Read on device: charger INHIBIT (0x06=0x00, EN_CMD off), battery present |
| Fuel gauge FG `0x40` SOC 0x09 / VOLT 0x0A | Read on device: both FGs dormant (all zero) - never enabled by boot firmware |
| RW-window writes to battery PMIC channels | **PROVEN UNSAFE** (see below) |
| RW-window writes to GPIO `0x88` on SID1 | Work (goodixd's enable_vcc3b runs every boot) |

### Write path is proven unsafe - removed from the driver

Two controlled experiments froze the board (network and UI dead,
power-cycle required), while the write command format was verified
byte-for-byte against mainline `spmi-pmic-arb.c` v5 and all channels are
EE0-owned:

1. ADC5@SID8 (apid 269): the first RW-window write ever issued froze the
   system silently mid-write.
2. FG@SID1 (apid 82, reg 0x09): a write-back-same-value probe (write the
   value just read) froze the system again - output stopped at the
   `writing` line, post-read never printed.

goodixd writes the same RW window on the same board (GPIO@SID1) and works,
so the stall is specific to the FG/ADC blocks of the battery PMICs in this
firmware state.  Therefore the driver has NO SPMI write path at all (no RW
window is mapped); every mode is strictly read-only.  Real capacity is
owned by the ADSP firmware battery manager (PMIC GLINK battmgr,
`battmgr_wire.h`) - the mechanism Windows uses on this WoA laptop.

### What the daemon reports

- `battery` - ADSP battmgr presence when available, else BATIF INT_RT_STS
  (0x48/0x10, present = !(bit4|bit5)).
- `charging` - battmgr charging flag, else CHGR status (0x06 bits[2:0] in
  TRICKLE/PRE/FULL/FAST).
- `capacity` - the best available source, in order:
  1. ADSP battmgr BATT_CAPACITY (the real SOC);
  2. FG SOC_MONOTONIC when the FG reports a live value (LSB 0.00390625 %,
     full = 25525);
  3. charger FULL_CHARGE state -> 100% (while the adapter holds the pack
     full);
  4. the last known capacity from `/etc/powerd.state` - the only source
     left while unplugged with no ADSP, where every register-based source
     is dead (proven by sweeps).
  The `status` reply tags the source: `(battmgr (ADSP))`, `(fuel gauge)`,
  `(charger full)` or `(last known)`.

### Persistent state (`/etc/powerd.state`)

Unplugged, nothing on the SPMI bus distinguishes the wired charger from
the other charger PMICs (all read state 0) and no capacity register is
live.  The daemon therefore persists the last known truth in
`/etc/powerd.state` (the root is a persistent ext3 volume):

```
chg_sid 10
cap 100
```

- `chg_sid` - SID of the wired charger, remembered whenever a live charge
  state is observed; used to pick the charger when no candidate is active.
- `cap` - last capacity reported by a live source (battmgr, FG or
  FULL_CHARGE).

The file rewrites only when a live source reports a changed value.
Bootstrap it once at deployment with the known ground truth:

```
/drivers/robokala/powerd --seed <chg_sid> <cap>
```

## Power key: short press sleep/wake, 3 s hold power off

The power key is read exactly the way mainline Linux reads it on this
board (`pm8941-pwrkey.c`): the PMIC PON peripheral's debounced KPDPWR
real-time status bit in `PON_INT_RT_STS` (base + 0x10) - bit 7 on gen3
PONs (pmk8350), bit 0 on gen1/gen2.  Everything stays observer-read-only
over the SPMI arbiter: no PON interrupt, debounce or reset configuration
is written, and no RW window is mapped at all.

Discovery (runtime, never hardcoded - the APID map shifts between boots):

- candidates: EE0 channels at peripheral `0x13` (gen3 HLOS `pon@1300`)
  and `0x08` (`pon@800`, gen1/gen2 and the gen3 PBS side);
- each candidate is verified by `PERPH_TYPE` (0x04) == 0x01 and its
  `PERPH_SUBTYPE` (0x05); secondary PONs (subtypes 2/5, RESIN-only) are
  not power-key candidates;
- preference: gen3 HLOS (subtype 9) > gen2 primary (4) > primary (1) >
  gen3 PBS (8) > 1REG (3).  On SC8280XP-class boards the wired key is the
  pmk8350 gen3 HLOS PON, which is what Linux's DT enables;
- a candidate that reads "pressed" before any press is an unwired/floating
  KPDPWR input and is skipped (the board has several PMICs but one key).

Behavior:

- **short press** (held 100 ms .. 3 s, released): toggles display sleep;
- **hold 3 s**: `sync()` + PSCI SYSTEM_OFF.  Fires while held (no release
  needed), also while asleep.  The PMIC's own hardware long-press
  force-off (firmware-configured, longer) remains the backstop;
- a press already down at daemon start (the boot press) is ignored until
  released.

## Keyboard lid: hall sensor sleep/wake

The TW220's keyboard/lid switch is a hall sensor on **TLMM GPIO 107,
active-low** - exactly the pad the mainline Ntmer TW220 device tree wires
to `gpio-keys` as `SW_LID` (`GPIO_ACTIVE_LOW`, bias-disable).  Folding
the keyboard onto the panel pulls the line low (closed); open reads
high.  `power_lid.c` maps the same TLMM window goodixd proved safe
(`0x0f100000`) and configures the pad as a GPIO input with a weak
pull-up: the pull-up only matters when the sensor is unpowered or the
pad floats, and it then reads HIGH = open, so a dead sensor can never
force the display asleep.  No interrupt configuration is written.

Behavior:

- **closed** (level stable for 200 ms): send `sleep` to `/dev/x`;
- **opened** (stable 200 ms): send `wake`;
- the first valid read applies the boot state, so a device started with
  the keyboard closed comes up asleep;
- the power key and the lid share one sleep state: a short power-key
  press can wake a lid-closed device, and only a real lid transition
  changes the state afterwards (no re-sleep while it stays closed);
- the 200 ms debounce absorbs the magnet bouncing past the sensor while
  the keyboard is attached/folded.

Any one source (battery, key, lid) is enough for the daemon to run; each
step stays idle when its hardware is unavailable.  Without the charger
PMIC the battery protocol reports [0,0,0], which hides the widget.

## Commands and bring-up verification

```
dev.cmd /dev/power0 status    # battery line + key= lid= sleeping=
dev.cmd /dev/power0 help
dev.cmd /dev/power0 reboot    # root only: PSCI SYSTEM_RESET
dev.cmd /dev/power0 shutdown  # root only: PSCI SYSTEM_OFF
/drivers/robokala/powerd --dump     # SPMI map summary (read-only)
/drivers/robokala/powerd --probe    # + observer-read sweep
/drivers/robokala/powerd --sample   # + one live battery sample
/drivers/robokala/powerd --mapall   # full APID map listing
/drivers/robokala/powerd --read <apid> <reg> <len> ...
/drivers/robokala/powerd --key 20   # live PON monitor (read-only)
/drivers/robokala/powerd --lid 20   # live hall monitor (TLMM only)
```

`--key` prints the PON candidates, the selected key (apid/sid/subtype)
and every down/up transition - press and release the power key during
the window.  `--lid` prints every open/closed transition - fold and open
the keyboard during the window to verify GPIO 107 and the polarity.
`devcmd /dev/x sleep` / `devcmd /dev/x wake` test the display-sleep path
without the daemon.

Expected battery status on the TW220: plugged -> `capacity=100%
(charger full)` with `charging=yes`, unplugged -> `capacity=100%
(last known)` with `charging=no` and `sid(chg=10)`; with the ADSP up,
`(battmgr (ADSP))` replaces both.

## Notes

- The APID map is reprogrammed by boot firmware per boot; always enumerate
  at runtime, never hardcode apids.
- `power_pmu.c` and `power_key.c` map the same SPMI VA slots in this one
  process; `SYS_MEM_MAP` on an already-mapped VA/PA pair just rewrites the
  same PTEs, so the second init is a no-op.
- Battery sampling period is 1 s; `status` re-samples synchronously.
- The TW220 battery is a 2S pack; the FG VOLTAGE_NOW plausibility window
  (2500..9800 mV) accepts it.