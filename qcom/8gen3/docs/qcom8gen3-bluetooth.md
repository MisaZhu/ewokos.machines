# QCOM8GEN3 Bluetooth interface (raspi5-compatible)

The WCN6855 combo module on the TW220 carries Wi-Fi on PCIe (owned by
`wlan`) and Bluetooth on a GENI UART, **not** USB: Linux binds the same
port as `serial@988000` (QUP0 SE2) with `hci_uart_qca` and
`qcom,wcn6855-bt`, and no QCA USB device enumerates on this machine. The
Bluetooth *daemon*, HID stack and desktop tools are the raspi5 ones and
are shared unchanged; only the HCI transport in `libbsp`
(`8gen3/system/libs/bsp/src/bsp_bt.c`) is QCOM8GEN3 code.

| Item | raspi5 | QCOM8GEN3 |
|---|---|---|
| Device node | `/dev/bt0` (0666) | same |
| Daemon | `system/gui/drivers/btd` | same source, `HW=8gen3` build |
| HCI transport | 16550 UART + `BT_ON` | GENI UART QUP0 SE2, FIFO mode, HW CTS/RTS |
| Firmware | CYW43455 patchram downloaded by `btd` | QCA serial DFU (TLV EDL) inside `bsp_bt_init` |
| Transport contract | `bt/bsp_bt.h` | same header, same seven hooks |
| Event-driven RX | `bsp_bt_wait_readable` returns 1/0 from the UART IRQ | returns -1; `btd` keeps its history polling cadence |
| Upper tools | `xbt`, `hid_moused`/`hid_keybd`/`hid_joystickd` in `bt` mode | same binaries and arguments |

Because the daemon and the HID subscribers are shared, the device surface an
application sees is identical on both boards.

## Device API

`/dev/bt0` speaks the same plain `key=value` text protocol as raspi5
(`xbt` parses it directly, not as JSON):

```
devcmd /dev/bt0 state          # powered/ready/scanning/devices/pending/le_*
devcmd /dev/bt0 devices        # discovery cache, "devices_done count=N"
devcmd /dev/bt0 known          # stored bonds
devcmd /dev/bt0 scan [secs]
devcmd /dev/bt0 connect <bdaddr>
devcmd /dev/bt0 pair <bdaddr> [pin]
devcmd /dev/bt0 unpair <bdaddr>
devcmd /dev/bt0 disconnect <bdaddr|handle>
devcmd /dev/bt0 open | close
```

`open`/`close`, `scan`, `connect`, `pair`, `unpair` and `disconnect` are
fire-and-forget and answer `*_begin`/`*_ok` at once; progress is reported
through the `state`/`devices` polls and the read-side event stream
(`power_on`, `scan_done`, `connected`, `bonded`, ...). The same node is the
subscriber channel: `hid_moused`, `hid_keybd` and `hid_joystickd` open it in
`bt` mode and publish `/dev/mouse1`, `/dev/keyb1` and `/dev/js1`.

## Boot wiring

`etc/xwin/init.rd` starts the raspi5 stack, plus the X input processes that
consume both the USB and the Bluetooth HID nodes:

```
@/bin/ipcserv /drivers/8gen3/btd           /dev/bt0
@/bin/ipcserv /drivers/8gen3/hid_moused    /dev/mouse1 /dev/bt0 bt
@/bin/ipcserv /drivers/8gen3/hid_keybd     /dev/keyb1  /dev/bt0 bt
@/bin/ipcserv /drivers/8gen3/hid_joystickd /dev/js1    /dev/bt0 bt
...
@/bin/bgrun /sbin/x/xmouse /dev/mouse0
@/bin/bgrun /sbin/x/xmouse /dev/mouse1
@/bin/bgrun /sbin/x/xim_none /dev/keyb0
@/bin/bgrun /sbin/x/xim_none /dev/keyb1
@/bin/bgrun /sbin/x/xim_none /dev/js1
```

The subscriber nodes exist before a device is paired, so the X input
processes open them at boot and simply stay idle until a BT keyboard, mouse
or gamepad connects. USB input (`mouse0`/`keyb0`) remains wired in parallel.

## UART bring-up

`bsp_bt_init()` mirrors `drivers/bluetooth/hci_qca.c` + `btqca.c`:

1. **Clocks/pins.** OR-vote the GCC QUP0 wrapper clocks (vote `0x52008`
   bits 6/7/8/9/12, never reset shared votes), program the S2 RCG2 M/N/D
   from GPLL0_OUT_EVEN (300 MHz) and the SE serializer divider, and mux
   TLMM gpio121-124 to qup2 (CTS keeper, RX pull-up, TX/RTS 2 mA output).
   UEFI already loaded the SE firmware; the driver checks the UART
   protocol revision and fails safely instead of loading it.
2. **Probe at 115200 8N1 + HW flow control**, read the EDL version
   (WCN6855 answers vendor commands inside Command Complete).
3. **Baud switch** to 3.2 M (vendor `0xFC48`, code 17). The M/N values give
   an exact 51.2 MHz root; Linux's clock table cannot hit it and settles
   for 3.125 Mbaud, so this transport is slightly more accurate.
4. **DFU**: rampatch TLV (`hpbtfw21.tlv`, `wcn*` name tried first), board
   id, then the board-specific NVM (`hpnv21.b<bid>`, `.bin` fallback) with
   the HCI tag patched for IBS + operating baud and the deep-sleep tag
   enabled. Segments are <=243 bytes; the patch download mode skips
   per-segment acks, NVM acks every segment.
5. **Finish**: disable SoC logging, `HCI_Reset`, write the BD address
   (the DT `local-bd-address` always wins, byte order reversed on the
   wire — exactly like Linux `qca_set_bdaddr`; the generic NVM tag 2 is
   the placeholder `ad:5a:00:00:00:00` and would break pairing).
6. **IBS in-band sleep** turns on: `0xFE`/`0xFD`/`0xFC` are intercepted at
   H4 frame boundaries only, the controller's `WAKE_IND` is acknowledged
   immediately, and every host TX while asleep runs the bounded
   `WAKE_IND`/`WAKE_ACK` handshake first.

Firmware lives in `/etc/firmware/qca/` (installed by
`8gen3/system/Makefile` from `8gen3/firmware/qca/`, decompressed from the
host `linux-firmware` `.zst` set). Only the blobs this board actually
loads are kept: `wcnhpbtfw21.tlv` + `wcnhpnv21.bin` (plus the non-`wcn`
name fallbacks) — the TW220 reports ROM `0201`, soc `400c0210` (non-`g`)
and board id `0000`, so all `.b<bid>` board variants, `g` variants and
the dead-path USB blobs were pruned. Bring-up progress is logged line by line
to `/tmp/bt0.log` (fsync per line so sdfsd commits the inode); the live
register/counter snapshot is available through `bsp_bt_diag_str`.

## Platform notes

- **Coexistence gate.** The UART and PCIe functions share the module's
  power/reset sequence, and `pcied` cold-cycles the rail before WCN
  startup. `bsp_bt_init()` therefore returns -1 until `wlan` has published
  a valid MAC; `btd` retries every three seconds.
- **No rail power-down.** `bsp_bt_power_off()` (the `close` verb) sends the
  QCA pre-shutdown command and drops the HCI session; the rail is shared
  with Wi-Fi and stays up. A later `open` re-runs bring-up: the probe
  remembers the previous baud, blind-wakes a possibly sleeping warm
  controller, commands it back to 115200 and repeats the full DFU.
- **Firmware hook.** `bsp_bt_firmware()` returns no image, so `btd`'s
  Broadcom patchram path is skipped; all QCA DFU happens inside the
  transport. The legacy USB client `bsp_usbbt.c` stays in the tree (with
  its host tests) but is not wired into this machine's build.
- **LE/HOGP vs classic HIDP**, bonding, discovery and the HID report
  translation all live in the shared daemon, so behaviour matches raspi5.

## Verification

- `8gen3/system/libs/bsp/tests/run-btuart.sh` builds the real `bsp_bt.c`
  against a GENI SE2 register model and a WCN6855 controller emulation
  (EDL-over-CC, TLV ack modes, baud switch, IBS sleep/wake) under
  ASan/UBSan: cold init, register/pinmux/clock values, DFU byte integrity,
  NVM tag patching, BD address order, steady-state TX/RX, spontaneous
  controller wake, mid-frame IBS-byte transparency, ring overrun, flush,
  power-off + warm recovery, and the failure paths (wlan gate, mapping,
  SE protocol, missing/corrupt firmware).
- `run-usbbt.sh` still passes for the legacy USB client and the `bsp_bt`
  client contract tests.
- `btd` links `libbsp.a` statically, so only the `btd` binary needs
  redeploying after transport changes (plus firmware and `init.rd`).