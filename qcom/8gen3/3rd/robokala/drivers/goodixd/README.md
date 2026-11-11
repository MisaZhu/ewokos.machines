# Robo & Kala TW220 Goodix Berlin touchscreen

`goodixd` serves the TW220's SPI-connected Goodix Berlin touchscreen as
`/dev/touch0`. Hardware reports patch PID `7986P` (ACPI `GXTS7986`, Windows
`GTBerlinB`); the board DTS uses compatible `goodix,gt9916`.

## Hardware and initialization

| Signal | Board connection |
| --- | --- |
| SPI mode | Mode 0, 8 bits, maximum 3 MHz |
| MISO / MOSI | TLMM GPIO85 / GPIO86 |
| CLK / CS | TLMM GPIO83 / GPIO84 |
| IRQ | GPIO175, active low, input pull-up |
| Reset | GPIO99, active low |
| VDDIO / AVDD enable | GPIO90 / GPIO56, active high |
| Shared VCC3B enable | PMC8280_1 GPIO1, SID1 peripheral `0x8800`, output high |
| Coordinates | 2560 × 1600 |

The SPI roles above were confirmed by register echo, firmware checksum and
IC_INFO reads. The TW220 DTS pin-role comments list the wrong ordering.
Missing VCC3B initialization and that incorrect ordering caused the original
nonresponse.

The driver uses GPIO SPI only. It maps TLMM separately at virtual
`0x58200000`, outside the framebuffer and xHCI mappings. GPIO writes retain
completion barriers/readback and explicit setup/hold delays. It never maps
or accesses GCC/GENI: that controller's clock, power and interconnect setup
has not been validated, and unprepared MMIO accesses caused system hangs.

Before cycling the local touch rails, the driver enables shared VCC3B through
the SPMI v5 arbiter. It checks arbiter version, APID ownership, LV pad type
and existing configuration, then verifies each write using the observer
channel. VIN, drive strength, pulls and master enable are preserved. Shared
VCC3B is never switched off.

Reset remains low while local rails are off for 1 ms. VDDIO settles for 3 ms,
then AVDD for 15 ms; CS returns high before reset is released, followed by
4 ms settling. Probe uses AA register echo, firmware checksum and packed
Berlin IC_INFO. Failed probes retry with exponential backoff up to 30 s and
throttled stage-specific errors. Diagnostic command-line modes and alternate
transport/pin assignments are removed.

## Startup and event contract

The Q8B graphical `init.rd` starts:

```
@/bin/ipcserv /drivers/q8b/goodixd /dev/touch0
@/bin/bgrun /sbin/x/xtouch -c /etc/x/xtouch-tw220.json /dev/touch0
@/bin/bgrun /sbin/x/xtouch -m
```

Only `goodixd` owns `/dev/touch0`; the USB touchpad has a separate daemon and
node. The absolute touchscreen configuration uses `y_rev: true`, verified
against the display orientation. The `-m` client handles the separate touchpad.

Records contain three `uint16_t` fields: state (1 pressed, 0 released), X and
Y. The driver reports one contact. Zero-contact release frames have a valid
header but no payload checksum; stale bytes after their header are ignored.
Release retains the last contact coordinates so `xtouch`/`xserverd` can
recognize clicks. Header and nonempty contact payload checksums remain required.
Normal operation does not log touch coordinates or register dumps.

## Validation

```
make -C machines/q8b/system/drivers goodixd -j4
sh machines/q8b/system/drivers/goodixd/tests/run.sh
```

Host tests cover SPI framing, checksums, packed IC_INFO and contact decoding.
On TW220, firmware ROM PID `BERLIN`, patch PID `7986P` and touch-data address
`0x00010274` were read successfully. The user verified movement, Y direction,
selection and activation with both release fixes applied. Cold-boot validation
remains outstanding.
