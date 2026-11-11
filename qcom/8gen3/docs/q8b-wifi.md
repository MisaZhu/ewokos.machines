# Q8B Wi-Fi interface (raspi5-compatible)

The Q8B `wlan` driver owns the WCN6855 (QCNFA765) PCIe endpoint. Since
2026-11 its user interface matches the Raspberry Pi 5 `wland` driver, so the
same desktop app, scripts and configuration work on both boards. The
Bluetooth function of the same combo module is covered in
[q8b-bluetooth.md](q8b-bluetooth.md).

Both QCNFA765 subsystem IDs (`17cb:0108` and `105b:e130`) use the
device-specific WCN6855 `nfa765` image. The PCI hardware revision may report
hw2.0 (`HW=0x400c0210`) or hw2.1 (`HW=0x400c1211`); the corresponding
linux-firmware hw2.0 and hw2.1 `nfa765` AMSS/M3/regdb/board payloads are
byte-identical, so the repository keeps the canonical hw2.1 copy.
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
fallback is computed at every boot and does not depend on `/etc/wlan.mac`. The
seed uses the PCI function identity (and the PCIe Device Serial Number when
available); it deliberately excludes BAR0 because the PCI allocator may move
BAR0 after a WLAN stop/start. When
the endpoint has neither serial nor VPD, the PCI identity remains stable on one
board but **can collide between identical unprogrammed cards** (every unprogrammed
`17cb:1103/17cb:0108` endpoint on the same PCIe root port derives the same MAC);
only module OTP/NVM calibration can provide a globally unique address. Because
of that, an explicit `/etc/wlan.mac` (17-char `aa:bb:cc:dd:ee:ff`, unicast,
nonzero) now **overrides the deterministic derivation** so a colliding per-board
address can be provisioned — a real module address (permanent MAC or WMI extra
address) still wins over the host file. This is a separate provisioning issue:
the TW220 failure also reproduces with one isolated hotspot, so clearing the AP
station table or changing the MAC does not explain that failure. PCI reads stay
in `pcied`, so the WLAN driver never changes PCI configuration state.

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
unchanged apart from the join API and diagnostics. The unsupported band-based
TCL LMAC experiment described below has been withdrawn.

## TW220 2.4 GHz WPA timeout

The TW220 captures associated successfully on 2.4 GHz (for example, 2447 MHz)
but then reports `WPA failed=handshake-timeout` with `eapol_rx=0`. A phone
hotspot reproduces the result, so an old session on the original AP is not a
sufficient explanation. PCIe, MHI, QMI, WMI, scan, authentication and association
complete in this capture, but no EAPOL frame reaches the supplicant. Management
RX through WMI can work while data RX through DP has no completions; these
counters do not establish whether the AP transmitted M1 over the air.

The earlier experiment selected TCL LMAC1 for 2.4 GHz. That interpretation was
incorrect and has been withdrawn: Linux ath11k marks WCN6855 hw2.0/hw2.1 as
`single_pdev_only`, maps host pdev index 0 to TCL LMAC0, and uses that index for
data TX on both bands. The TCL field selects the AST lookup namespace; the
physical radio chosen for a channel does not determine this field. The driver
now restores `s.lmac=0`. This removes an unsupported change, but does not yet
explain the original failure, which also occurred with LMAC0.

Rebuild the driver with:

```text
make -C machines/q8b/system/drivers/wlan -j4
```

After deploying the resulting `/drivers/q8b/wlan` and retrying the hotspot,
`devcmd /dev/wl0 station` should show `mhz=2447 ... lmac=0`. Collect the new
`data TX complete` log together with `status` and `rings`: TX completion source,
reason and raw words distinguish descriptor recycling from an acknowledged
transmission. Firmware consumption of refill ring 128 with no REO/error-ring
completions remains an unresolved observation, not proof of an RF failure.

## Reading the TW220 status snapshot

During the 1–2 second QMI/BDF phase, `stage=MHI-discovery`, `pending=25`,
`bdf=0`, `m3=0`, `wmi_ready=0` is expected: request `0x25` is the board-file
upload. It must later become `bdf=1`, `m3=1`, `wmi_ready=1`, and `dp=1` before
WPA is evaluated. The recurring `error=cafecace` in this build is the raw BHI
error register's idle sentinel; it also appears in a healthy `WMI-ready`
status and is not, by itself, an MHI failure. `169.254.x.x/16` with no gateway
means netd has only assigned a link-local address because WPA/DHCP has not
completed.

The fallback MAC must remain stable across recovery. BAR0 is a runtime PCIe
allocation and may change between stop/start, so it is excluded from the MAC
seed. If the module has no OTP/VPD/serial identity, provision a unique
`/etc/wlan.mac` per board to avoid collisions between identical modules.

The BDF variant is selected from QMI identity. For the current TW220 identity
(`chip-id=2`, `board-id=255`), the driver automatically selects the
`HW_GK3` entry; no `/etc/wlan.board` file is required. Q8B reports chip-id
`18` and therefore follows its own generic/board entry. An explicit
`/etc/wlan.board` remains an optional override, and a variant is never reused
across a different QMI chip.

## Authentication timeout diagnostics

In the latest capture, `station failure=timeout status=9` is the local
`AUTH_WAIT` state number, not an AP status code. The driver has sent an Open
System Authentication request and has not accepted the response before the
8-second deadline. This attempt has not reached association or WPA. For
`AP-disconnected`, `status` instead contains the received deauthentication or
disassociation reason code; reason 6 means a Class 2 frame from a station the
AP considers unauthenticated. Confirm the original reason value if the log
was copied through OCR.

The driver now logs the selected BDF lookup and byte count, join channel/BSSID,
VDEV start result, authentication/association TX completions and RX responses.
Each station failure first records its phase, management/data/EAPOL counters and
DP status including the last data TX completion,
so automatic recovery does not erase the diagnostic evidence. Responses from
the target AP with an unexpected reported frequency are logged before the
existing channel check. These additions preserve the connection behavior.

Deploy the rebuilt `system/build_aarch64/drivers/q8b/wlan` from this machine's
system tree to `/drivers/q8b/wlan` on the persistent rootfs. The EFI initramfs
contains `pcied` but does not contain `wlan`; updating only `initramfs.img`
does not install a new WLAN driver. After one failed connection, collect
`devcmd /dev/wl0 log`, `station`, and `rings` output.
