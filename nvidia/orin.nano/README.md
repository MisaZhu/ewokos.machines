# EwokOS on NVIDIA Jetson Orin Nano (Tegra234)

Minimal bring-up port of EwokOS to the Jetson Orin Nano Developer Kit
(p3767 module + p3768 carrier, Tegra234/T234 SoC, 6x Cortex-A78AE, aarch64).
Modeled on `machines/qcom/8gen3`: NVIDIA EDK2 UEFI loads `BOOTAA64.EFI`,
which hands the kernel a bootinfo page and a boot RAMFS; the system runs
self-rooted from that RAMFS with a serial rescue shell.

**Scope:** kernel BSP (GICv3, UARTC, ARM generic timer, PSCI SMP) + UEFI
loader + ramdisk-only root filesystem with serial shell + Ethernet
(RTL8168 on PCIe C8) + BPMP/PCIe foundation + I2C/GPIO/SDHCI peripherals +
USB xHCI controller bring-up. No NVMe/display/WiFi data path yet; USB device
classes and SDHCI switchroot integration are follow-ups.

## Hardware map (sources: upstream `tegra234.dtsi`, hvisor Orin board config)

| Device | Address / value |
| --- | --- |
| DRAM | `0x80000000` .. `0x280000000` (8GB module) |
| GICv3 GICD / GICR | `0x0f400000` / `0x0f440000` (2MB, stride `0x20000`) |
| Debug UART (UARTC, AON) | `0x0c280000`, 16550-compatible, reg-shift=2, 115200 8N1 |
| Timer | ARM generic virtual timer (CNTV), PPI 27, CNTFRQ 31.25MHz |
| Cores | 6, MPIDR affinity {0x0, 0x100, 0x200, 0x300, 0x10200, 0x10300} |
| PSCI | TF-A BL31, SMC `0xC4000003` (CPU_ON) |

Tegra234 MPIDR layout is **Aff2=cluster, Aff1=core-in-cluster, Aff0=0**
(cluster0: Aff1 0..3; cluster1: Aff1 2..3). The generic Aff0|Aff1 core-id
merge drops Aff2 and collides across clusters, so the BSP resolves logical
ids through an MPIDR table (`bsp/gic_v3.c`, `bsp/early_vectors.S`) and GIC
SGIs carry Aff2.

Kernel VM: `KERNEL_BASE=0x4000000000`, 16K pages. The shared kernel caps the
user MMIO window at 128MB, so the window is anchored on the AON cluster
(`0x0c000000`, 4MB; UARTC at offset `0x280000`) with a dedicated GIC window
at virtual offset `0x04000000`.

## Boot chain

```
MB1/MB2 -> TF-A BL31 (EL3, PSCI) -> NVIDIA EDK2 UEFI (EL2)
  -> \EFI\BOOT\BOOTAA64.EFI
     loads \orin\KERNEL.BIN + \orin\INITRAMFS.IMG from the ESP,
     fills bootinfo at 0x90070000 (magic "EORN", UEFI memory map,
     reserved regions, ramdisk, GOP fb, DTB /model),
     ExitBootServices, disables the EL2 MMU, jumps to 0x90080000
  -> kernel .init (physical) -> high-half kernel -> /sbin/init
     rootfs=self: keep boot RAMFS, serial rescue shell on /dev/tty0
```

Jetson UEFI runs at **EL2**: the loader clears `SCTLR_EL2` (not just
`SCTLR_EL1`) before the jump; the shared `boot.S` then performs the
EL2->EL1 drop itself. `boot.S` also sets `ICC_SRE_EL2` (SRE|DIB|DFB) on the
EL2 path before `eret`: `ICC_SRE_EL1.SRE` is RAZ/WI while `ICC_SRE_EL2.SRE=0`,
so without this the kernel's GICv3 system-register accesses would trap.
This covers both the boot core and PSCI secondaries (which TF-A also enters
at EL2).

## First-boot serial diagnostics

The kernel prints a bring-up trace on UARTC (gated by the loader's UART
probe, `bootinfo.serial_flags` bit0). Expected lines, in order:

```
orin: model=<DTB /model> cores=6 ram=<N>MB serial=1
orin: bootinfo rd=0x.../<N>KB flags=0x1 resv=<N> fb=0x...
orin: gic boot-core=0 gicr=6/6 spi-aff=0x0
orin: PSCI CPU_ON core 1 mpidr=0x100 ok
... (cores 2..5)
```

`gicr=6/6` confirms the redistributor affinity walk matched all cores;
`WARNING gicr affinity match incomplete` means the MPIDR table disagrees
with the hardware. A missing `PSCI ... ok` line (or `failed: 0x...`) pins a
secondary bring-up fault. If nothing appears after the loader's
`post-EBS UARTC raw K`, the kernel stalled before `sys_info_init_arch`
(MMU/vector bring-up) — check the EL2 jump and early page tables first.

If the bootinfo hand-off is corrupt (bad magic/version), the kernel still
enables UARTC (`orin_serial_on=1`) and prints `WARNING bootinfo invalid`, so
a broken hand-off is diagnosable rather than silent. Unhandled exceptions
use the shared vector dump (`sync_exception_handle`/`dump_ctx`): PID/CMD,
EC/FSC names, pc/lr/sp/spsr, ESR/FAR and all GPRs over UARTC.

## Rescue-shell debug tools

The initramfs packs the shared inspection utilities alongside the console,
so the rescue shell is useful on hardware without a persistent disk:

- `mmio read <hex>` / `mmio write <hex> <val>` — peek/poke the AON MMIO
  window (`sysinfo.mmio`, `0x0c000000`+4MB). UARTC is at `0xc280000`, e.g.
  `mmio read c280014` reads the LSR (`0x60` = TX idle/alive). The tool is
  bounded to that one window; GIC state is reported by the kernel boot
  diagnostics instead.
- `sysinfo` / `svcinfo` — kernel `sys_info` (cores, memory pools, MMIO,
  platform hand-off) and registered services.
- `devcmd` — send commands to a device node (e.g. uartd).
- `ps` / `top` / `dump` / `elfinfo` — process and memory inspection.
- `cat ls echo grep find head more wc df mount vi ...` — file/fs editing.

`SYS_MEM_MAP` is whitelisted by `check_mem_map_arch` to the AON window, the
GIC window, the ramdisk and the framebuffer only; arbitrary SoC registers
(pinmux `0x2430000`, GPIO `0x2200000`) are intentionally not mapable from
user space.

## Build

Requires the `aarch64-none-elf` toolchain (GCC 14.3 tested), python3, and
e2fsprogs (`mke2fs`, `e2cp`; Homebrew paths on macOS).

```sh
export PATH=/path/to/aarch64-none-elf/bin:/opt/homebrew/bin:$PATH

make -C machines/nvidia/orin.nano/kernel          # kernel.bin (SMP, 6 cores)
make -C machines/nvidia/orin.nano/kernel SMP=no   # kernel_up.bin (optional)
make -C machines/nvidia/orin.nano/system          # build_aarch64/ rootfs
make -C machines/nvidia/orin.nano/efi             # BOOTAA64.EFI + kernel.bin
                                                  # + initramfs.img (16MB ext3)
```

## Deploy

Copy to the FAT32 ESP of the boot medium (SD card / USB / NVMe):

```
EFI/BOOT/BOOTAA64.EFI   <- efi/BOOTAA64.EFI
orin/KERNEL.BIN         <- efi/kernel.bin
orin/INITRAMFS.IMG      <- efi/initramfs.img
```

The loader also accepts `\KERNEL.BIN` / `\EFI\BOOT\KERNEL.BIN` (same for
INITRAMFS.IMG) and a GRUB-style LoadFile2 initrd. Boot the medium from the
UEFI boot menu (or the UEFI shell: `fs0:\EFI\BOOT\BOOTAA64.EFI`).

## Serial console

**J14 debug header** (under the module, SODIMM pins 236/238 =
UART2_TXD/RXD), 115200 8N1, 3.3V TTL. **Not** the 40-pin header (that one is
UARTA). Login: `root` / `root`.

The firmware (SPE/TCU + UEFI) leaves UARTC clocked and programmed; neither
the loader nor the kernel reprograms the baud rate. The loader probes the
UART (EFI Serial I/O first, raw LSR/TX probe otherwise) and tells the kernel
through `bootinfo.serial_flags` whether UARTC is usable.

## Verification status

Verified (build-only, no Orin hardware attached):

- SMP and UP kernels build clean; `__entry` is the first image byte;
  boot.S core-id hooks and `__core_id -> orin_core_id` confirmed by objdump.
- Loader builds to PE32+ (subsystem 10); EL2-aware MMU-off jump confirmed
  by objdump; bootinfo layout matches `bsp/hw_arch.h` field-for-field.
- `boot.S` EL2 path sets `ICC_SRE_EL2` before `eret` (objdump: `msr
  icc_sre_el2` at `__entry+0x1c`); bring-up diagnostics (bootinfo/GIC/PSCI)
  confirmed present in the linked image.
- Full system tree builds; init resolves `/drivers/orin.nano/uartd`
  (machine_conf.h); initramfs packs rootfs=self, no `/etc/init.rd`, so the
  self-root rescue-shell path is taken.
- MPIDRs, UART/GIC/DRAM addresses cross-checked against upstream
  `tegra234.dtsi` and the hvisor Orin Nano board config (both agree).

NOT verified (needs the board):

- Actual boot on hardware: MB1/MB2/UEFI hand-off, EL2 jump, MMU bring-up,
  GICv3 redistributor discovery, timer tick, PSCI secondary bring-up.
- UARTC wiring/clocking assumptions (J14 header, firmware-initialized baud).
- UEFI memory-map/reserved-region handling on real Jetson firmware (the
  32-entry resv table merges adjacent regions; overflow logs a warning).

Assumptions to check first on hardware:

- UEFI boots on MPIDR `0x0` (logical core 0); the shared kernel hardcodes
  boot-core 0 in places.
- The kernel window `0x90080000` (RAM base + 256MB) is free DRAM on the
  production firmware (above the low TZ/OP-TEE carveouts); the loader
  claims it with AllocatePages and refuses to overwrite live buffers.
- PSCI CPU_ON with the table MPIDRs wakes all 6 cores.

Firmware state (cross-checked against upstream `tegra234.dtsi`):

- The TCU node (`nvidia,tegra234-tcu`) is `status = "disabled"` upstream:
  Linux drives UARTC directly with the 8250 driver, so SPE/TCU does not own
  the console after boot and raw 16550 access is correct.
- There is **no AP-side watchdog node** in `tegra234.dtsi`; the T234 WDT is
  BPMP-managed. The loader therefore does not poke any watchdog (no verified
  address/sequence; a blind write could trigger a reset). If a production
  firmware leaves a WDT armed, that is a separate, board-specific fix.
- The generic timer is the virtual timer (GIC_PPI 11 = INTID 27, level-low),
  matching `bsp/timer.c` / `ORIN_GIC_TIMER_IRQ`.

## Networking (Ethernet, bring-up)

The devkit RJ45 is a **Realtek RTL8168** gigabit NIC on **PCIe C8**
(`pcie@140a0000`, pci-domain 8) — *not* the SoC MGBE (those are wired only on
AGX Orin). The driver stack mirrors the qcom port:

```
libbsp/bsp_pcie.c  enumerate C8 via ECAM (inherit UEFI), return RTL8168 BAR0
libbsp/bsp_eth.c   RTL8168 MAC, polled TX/RX rings (r8169 register layout)
drivers/net        /dev/eth0 vdevice glue (shared net.c)
drivers/netd       shared TCP/IP stack on /dev/net0 (DHCP by default)
```

`init.ramfs.rd` starts `net /dev/eth0` then `netd /dev/net0 /dev/eth0`
(best-effort: if the NIC is absent the serial console is unaffected). Inspect
with `devcmd /dev/eth0` (prints `bsp_eth_stats`: link/speed, rx/tx counters,
PHYstatus) and `sysinfo`.

**Inherit-UEFI approach (no BPMP / root-complex bring-up).** Jetson UEFI
enumerates C8 + RTL8168 for PXE; the driver assumes that state survives
ExitBootServices and only reads config space through ECAM (`0x3530000000`)
and drives the MAC. This deliberately avoids the large BPMP IVC + Tegra PCIe
RC foundation. Assumptions to verify on hardware:

- UEFI leaves C8 powered/clocked and the RTL8168 enumerated (ECAM readable,
  BAR assigned in the non-prefetchable window `0x3528000000`+128MB).
- The PCIe SMMU for C8 is bypass/identity so the NIC bus-masters to the
  kernel DMA32 pool physical addresses directly.
- BAR0 is mapped as a fixed 256KB region (live BARs are never resized).

If UEFI does *not* leave C8 up, `bsp_eth_init` falls back to **cold-own**:
`bsp_pcie_cold_own(C8)` brings the root complex up via BPMP and re-enumerates
(see the PCIe foundation below).

## PCIe foundation (BPMP IVC + cold-own RC)

The full PCIe bring-up path is implemented for when firmware does not leave a
controller up, and as the prerequisite for any C1 (WiFi) work:

```
libbsp/bsp_bpmp.c  BPMP IVC client: HSP_TOP0 doorbells (0x03c00000) + shared
                   SRAM ring (tx 0x40070000 / rx 0x40071000, non-cacheable).
                   MRQ_CLK(22)/RESET(20)/PG(66)/UPHY(69). Polled (no IRQ).
libbsp/bsp_pcie.c  cold-own: UPHY ctrl on -> PG on -> clk enable -> APB reset
                   deassert -> P2U PHY -> appl (RP mode, DBI/iATU base) ->
                   core reset deassert -> DBI root-port -> LTSSM link (L0).
                   C1/C8 window + BPMP resource IDs from tegra234.dtsi.
drivers/pcied      C1 host-bus owner: cold-owns Key-E, enumerates the WiFi
                   card, publishes /dev/pcie0 snapshot (qcom ownership model).
```

`bsp_pcie_cold_own` logs each stage (BPMP / appl-DBI / LTSSM) so a hardware
bring-up failure can be pinned. **This path is hardware-unverified**: the IVC
handshake, P2U calibration, DBI root-port init and LTSSM timing are taken
from upstream `ivc.c` / `bpmp-tegra186.c` / `pcie-tegra194.c` /
`phy-tegra194-p2u.c` but have not run on silicon. The inherit-UEFI Ethernet
path remains the primary, lower-risk route.

## WiFi: feasibility conclusion

The Orin Nano Super ships an Azurewave AW-CB375NF (Realtek **RTL8822CE**,
PCI ID `10ec:c822`, registers in BAR2) on PCIe C1; BT is on USB2. The
original devkit has an empty Key-E slot.

**RTL8822CE is a softmac device.** The rtw88 driver (~167k lines whole tree,
~30k+ for the 8822C PCIe subset) `depends on MAC80211`: scan / authentication
/ association / 4-way handshake / key management all live in the host
mac80211/cfg80211 stack, which **EwokOS does not have** (the qcom WCN6855 is
fullmac ath11k — firmware does the MLME). There is no fullmac firmware for
the RTL8822CE.

So the PCIe foundation can *enumerate* the card (`pcied` does this), but a
WiFi data path requires either porting mac80211+cfg80211 (not realistic) or
writing a bare-metal 802.11 MLME against the rtw88 core (months, high risk).
**Recommended alternatives:** a fullmac SDIO module (brcmfmac — firmware does
the MLME) or an ESP32 SPI/UART coprocessor. WiFi is therefore deferred; the
C1 PCIe bring-up is in place for whichever direction is chosen.

## Peripheral drivers (I2C / GPIO / SDHCI / USB)

All register layouts are from upstream Linux (`i2c-tegra.c`,
`gpio-tegra186.c`, `sdhci-tegra.c`, `xhci-tegra.c` / `xusb-tegra186.c`);
addresses/BPMP IDs in `include/orin_net.h`.  All are **build-verified,
hardware-unverified**, with bounded polls so a missing device never hangs.

- **I2C** (`bsp_i2c.c`, packet-mode PIO): gen1 `0x3160000` (module EEPROM
  `0x50`, carrier `0x57`), gen2 `0x0c240000` (INA3221 `0x40`).  `i2cd
  /dev/i2c0|/dev/i2c1` exposes read/write transfers; `orinmon` reads the
  INA3221 rails (VDD_IN / VDD_CPU_GPU_CV / VDD_SOC: mV/mA/mW, 5mΩ shunt) and
  dumps the module EEPROM.  The fan is a PWM fan (PWM3), not I2C.
- **GPIO / pinmux** (`bsp_gpio.c`): tegra186-style controller, main
  `0x2210000` + AON `0xc2f1000`, port->bank/port table from
  `tegra234_main_ports`.  `gpio_no = port_index*8+pin` (AON at +256); secure
  SCR access checked.  `gpiod /dev/gpio0` reads/writes/configs GPIOs and reads
  pinmux registers.  Pull is left as firmware set it (Tegra pull lives in
  pinmux).  SD card detect = G7 (`gpio_no` 55).
- **SDHCI microSD** (`bsp_sdmmc.c`): SDMMC1 `0x3400000`, BPMP clk 120 / tmclk
  219@12M / reset 82, Tegra vendor tap/trim + auto-cal, standard SDHCI PIO.
  Card init CMD0/8/ACMD41/2/3/9/7, legacy/HS 25MHz (no UHS/1.8V tuning),
  SDHC block addressing.  `bsp_sd_init_physical()` wires it as a libsd
  backend for a persistent rootfs; `sdprobe` reports capacity + MBR partitions.
- **USB xHCI** (`bsp_xusb.c`): Tegra XUSB bring-up only — BPMP powergate
  XUSBA/XUSBC + clocks, padctl USB2 host PHY (devkit hub on usb2-1), FPCI
  BAR0/BAR2, wait `USBSTS.CNR` clear; T234 Falcon uses IFR firmware (no
  xusb.bin).  `usbhostd /dev/usb0` brings the controller up and reports
  root-port PORTSC.  **The xHCI ring/endpoint driver + USB device classes
  (the shared/qcom xhci core + gui USB stack) are a follow-up** — the core is
  reusable (`xhci_init(cap_base)`), the Tegra-specific bring-up is done here.

## Test watchdog (TEST-ONLY, not for mainline)

A bring-up safety net: a hardware watchdog that resets the board if any stage
hangs. **Off by default** (`WATCHDOG_TEST=0`); the normal image is
watchdog-free. Build the test image with:

```sh
make -C machines/nvidia/orin.nano/efi WATCHDOG_TEST=1
```

Hardware: Tegra234 exposes an **ARM SBSA Generic Watchdog**
(`nvidia,tegra-wdt-t234`, control frame `0x2190000`, refresh frame
`0x2090000`; confirmed by NVIDIA). Driver `bsp_wdt.c` follows upstream
`sbsa_gwdt.c` (single-stage: WS0 ignored, WS1 resets at the programmed
timeout; `WOR = CNTFRQ/2 * timeout`, pet = write WRR).

Behaviour:
- The **loader** arms the watchdog (60s) right before the kernel jump
  (`-DORIN_WATCHDOG_TEST`), so a hang in kernel boot / early userspace — before
  the feeder starts — resets the board.
- **watchdogd** (`/dev/watchdog0`, started from the test init profile) takes
  over with a 30s timeout and pets every ~10s **only while the serial console
  (uartd `/dev/tty0`) is alive**. If the console/login subsystem dies, or the
  system hangs so the feeder loop stops, petting stops and the hardware resets
  the board. This is the "feed after serial login, reset on any failure"
  behaviour.

**Removal (after the port is done):** build with the default `WATCHDOG_TEST=0`
(the loader is always recompiled, so the flag takes effect without `clean`).
To delete the feature entirely: remove `system/drivers/watchdogd/`,
`system/libs/bsp/src/bsp_wdt.c` + its header/object entry, the
`ORIN_WATCHDOG_TEST` block in `efi/main.c`, the `WATCHDOG_TEST` logic in
`efi/Makefile`, the WDT whitelist lines in `kernel/bsp/hw_info_arch.c`, and the
WDT defines in `include/orin_net.h`. None of it is referenced by the normal
boot path.

**Hardware-unverified:** the SBSA reset path (WS1 → SoC reset) is confirmed by
NVIDIA to fire on a CPU hang, but has not run on this board. The forum also
notes the UEFI GenericWatchdogDxe did not reset on a null-write *exception*
(only on a hang) — for this test the feeder simply stops on any hang/crash, so
the watchdog fires; exception-handler behaviour should be checked on hardware.

## Layout

```
kernel/    BSP: start.c (16K boot tables), gic_v3.c, timer.c, irq.c, ipi.c,
           core.c (PSCI), sd.c (ramdisk block backend), hw_info_arch.c
           (bootinfo/memory map), dbg.c/uart.c (UARTC), early_vectors.S
efi/       BOOTAA64.EFI loader (main.c, efi.h, mkpefi.py) + image packaging
system/    minimal userland: libs/bsp (ramdisk sd + bootfs, bsp_bpmp IVC,
           bsp_pcie inherit/cold-own, bsp_eth RTL8168, bsp_i2c, bsp_gpio,
           bsp_sdmmc SDHCI, bsp_xusb bring-up), drivers/uartd (UARTC),
           net (/dev/eth0), pcied (C1 owner), i2cd, gpiod, usbhostd,
           tools orinmon (power/EEPROM) + sdprobe (microSD),
           etc (kernel.conf rootfs=self, init.ramfs.rd)
include/   qcom8gen3_platform.h (shared boot hand-off layout, reused verbatim),
           machine_conf.h (INIT_DRIVER_DIR), orin_net.h (PCIe/BPMP/I2C/GPIO/
           SDMMC/XUSB address + ID map)
```

Shared-tree changes for this port (all gated by `ORIN_BOARD` /
`__has_include`): `boot.S`/`system.S` core-id hooks, init's
`INIT_DRIVER_DIR` (defaults to "8gen3" elsewhere).