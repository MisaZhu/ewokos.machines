# Q8B Wi-Fi interface (raspi5-compatible)

The Q8B `wlan` driver owns the WCN6855 (QCNFA765) PCIe endpoint. Since
2026-11 its user interface matches the Raspberry Pi 5 `wland` driver, so the
same desktop app, scripts and configuration work on both boards. The
Bluetooth function of the same combo module is covered in
[q8b-bluetooth.md](q8b-bluetooth.md).

Both QCNFA765 subsystem IDs (`17cb:0108` and `105b:e130`) use the
device-specific WCN6855 hw2.1 `nfa765` image
(`WLAN.HSP.1.1-04866.5-QCAHSPSWPL_V1_V2_SILICONZ_IOE-1`) for AMSS and M3.
The generic image must not be substituted. Selecting `nfa765` is necessary
for this card, but it does not by itself prove that a permanent MAC was
exported: the current firmware can still return a `00:03:7f:*` WMI address.
Other supported WCN6855 cards retain the common image. Linux ath11k selects
the same `nfa765` subdirectory for its WCN6855 hw2.1 use-case firmware; the
board and regulatory containers remain the shared WCN6855 `board-2.bin` and
`regdb.bin` files.

Some QCNFA765 firmware builds report a `00:03:7f:*` generated address when
the module calibration does not provide its permanent address. The address
source priority is WMI extra/module address, PCI VPD, and then a deterministic
hardware identity fallback. `pcied` reads the endpoint's optional PCIe Device
Serial Number capability and VPD; the serial is mixed with the immutable PCI
function identity to derive a locally administered unicast address. This
fallback is computed at every boot and does not depend on `/etc/wlan.mac`.
When the endpoint has neither serial nor VPD, the PCI identity remains stable
on one board but can collide between identical unprogrammed cards; only module
OTP/NVM calibration can provide a globally unique address. `/etc/wlan.mac` is
accepted only as a last-resort host override when no hardware identity is
available. PCI reads stay in `pcied`, so the WLAN driver never changes PCI
configuration state.

| Item | raspi5 `wland` | Q8B `wlan` |
|---|---|---|
| Device node | `/dev/wl0` | `/dev/wl0` |
| Started by | `ipcserv /drivers/raspi5/wland /dev/wl0` | `ipcserv /drivers/q8b/wlan /dev/wl0 --start` |
| Config | `/etc/wlan/network.json` | `/etc/wlan/network.json` |
| devcmd | `help log state scan list connect` | same, plus Q8B diagnostics |
| Network stack | `netd /dev/net0 /dev/wl0` | `netd /dev/net0 /dev/wl0` |

`--start` boots MHI/firmware at init; a `connect` request boots the controller
too, so the device works on a node started without `--start`.

## Configuration

`/etc/wlan/network.json` uses the pi5 schema:

```json
{
    "network":[
        {"ssid":"home","passwd":"secret","priority":1},
        {"ssid":"office","pmk":"<64 hex chars>"}
    ]
}
```

- `ssid` and one of `passwd` (8..63 chars) or `pmk` (64 hex = precomputed
  pairwise master key) are required. `pmk` wins when both exist.
- `priority` is optional (default 0). On boot the driver scans and joins the
  highest-priority entry that is visible and usable.
- A successful manual `connect` is persisted back into this file: an existing
  ssid only gets its credential refreshed, duplicate ssids are dropped, and a
  stale credential of the other kind (`pmk` vs `passwd`) is removed.
- The build installs an empty template only when the file is absent, so an
  existing configuration is never overwritten by a rootfs update.

If no JSON entry exists, the pre-pi5 profile `/etc/wlan.conf` is still read
(`ssid` LF `passphrase` LF 64-hex seed, the seed line is now optional and
ignored). This keeps SD cards provisioned before the update working. The
driver also accepts `--connect <file>` with either format.

## devcmd

```
devcmd /dev/wl0 help
devcmd /dev/wl0 log                 # recent driver log (ring buffer)
devcmd /dev/wl0 state               # JSON, same keys as pi5
devcmd /dev/wl0 scan                # "scan started" / "scan failed: -EBUSY"
devcmd /dev/wl0 list                # JSON array of scan results
devcmd /dev/wl0 connect <ssid> <passwd>
```

`state` publishes `state` (`idle`/`scanning`/`connecting`/`connected`/
`disconnected`), `ssid`, `mac`, `bssid`, `rssi`, `channel`, `type`, `auth`,
`cipher`, `ip` (always empty: netd/dhcp owns it), `scan_ready`, `reason`,
plus the q8b `link_state`/`connect_attempts` counters. `list` entries carry
`id`, `ssid`, `selected`, `bssid`, `rssi`, `channel`, `type`, `auth`,
`cipher`.

Q8B extensions: `start`, `stop`/`disconnect`, `reconnect`, `country <AA>`,
`scan-stop`, `scan-results` (text), `station` (station/WPA state), `associate
<ssid>` (associate without a supplicant) and `rings` (data-path counters).

## Behaviour

- Only WPA2-PSK/CCMP is implemented. Open, WEP, WPA1, WPA2-EAP and WPA3-SAE
  networks are listed with their security labels but cannot be joined;
  configured entries with unsupported security are skipped at scan time.
- The controller is single-owner and single-threaded. `connect` queues a
  request that the device loop executes; joining a different network tears the
  controller down and boots it again.
- Link loss or a dead control plane arms a restart ladder with 1/2/4/8/16/30 s
  backoff. A controller that never reaches WMI+DP within 60 s is restarted.
- Scanning while associated is not supported by this station implementation;
  `scan` returns `-EBUSY` then and the app keeps its cached list.
- Session entropy comes from the libc PRNG mixed with the kernel tick; the
  legacy 32-byte seed file field is no longer required.
- A truncated or malformed `network.json` is rejected before parsing. tinyjson
  never returns from an unterminated object/array/string (it grows the tree
  until the allocator fails), and a corrupt file on the persistent rootfs must
  not keep the device node from mounting. The driver logs the reason and
  continues with an empty configuration.

## Verification

`aarch64-elf-gcc` + `aarch64-elf-ld` build and link the driver against
`system/build_aarch64/q8b/lib` (including `libtinyjson.a`). Hardware bring-up
on the Q8B has not been re-run for this change; the MHI/WMI/DP/WPA paths are
unchanged apart from the join API and logging.
