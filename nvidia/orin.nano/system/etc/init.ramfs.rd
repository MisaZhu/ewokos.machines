# Orin Nano boot RAMFS profile (packed as /etc/init.ramfs.rd).
#
# No console daemon is started here on purpose: the image ships no
# /etc/init.rd and kernel.conf selects rootfs=self, so init's self-root path
# falls through to its serial rescue console right after this profile
# returns.  uartd/sessiond/session therefore start exactly once (root/root
# login on the J14 debug UART, 115200 8N1).
@/bin/ipcserv /drivers/timerd
@/bin/ipcserv /drivers/ramfsd            /tmp
@/bin/ipcserv /drivers/piped             /dev/pipe0
@/bin/ipcserv /drivers/nulld             /dev/null
# PCIe C1 (M.2 Key-E) host owner: cold-owns the root complex via BPMP.
# DISABLED for bring-up: BPMP HSP doorbell access (0x03c50300) triggers a RAS
# bus error / core power-off on this board.  Re-enable once BPMP is debugged.
# @/bin/ipcserv /drivers/orin.nano/pcied   /dev/pcie0
# I2C masters / GPIO: DISABLED for bring-up.  These peripherals need BPMP
# clock enable; BPMP access itself faults (RAS), so the unclocked I2C/GPIO
# register access raises an SError -> EL3 crash.  Re-enable once BPMP works.
# @/bin/ipcserv /drivers/orin.nano/i2cd    /dev/i2c0
# @/bin/ipcserv /drivers/orin.nano/i2cd    /dev/i2c1
# @/bin/ipcserv /drivers/orin.nano/gpiod   /dev/gpio0
# Ethernet (RTL8168 on PCIe C8): cold-own via BPMP, run at boot as the bring-up
# test.  Progress/crash markers land on the serial console; recover with the
# Pico GPIO hardware reset.  MAC confirmed up, so netd (DHCP) now starts too.
@/bin/ipcserv /drivers/orin.nano/net     /dev/eth0
@/bin/ipcserv /drivers/netd              /dev/net0 /dev/eth0
# microSD (SDMMC1) bring-up probe: cold-owns the controller via BPMP, runs the
# SD init sequence and dumps sector 0.  One-shot; progress/result land on the
# serial console via klog.  Recover with the Pico GPIO hardware reset.
@/drivers/orin.nano/sdprobe
# Fan control: BPMP TJ_MAX temperature -> hardware PWM3 (clock/reset owned
# via BPMP before any MMIO).  Auto policy = NVIDIA tj-thermal trips;
# /dev/fan accepts status/auto/manual/run/duty commands.
@/bin/ipcserv /drivers/orin.nano/fand /dev/fan
# VIC 2D engine M1 probe: DISABLED until staged manual testing is approved.
# First VIC MMIO touch is unverified (unclocked/firewalled read would SError);
# run stages by hand from the rescue console: vicprobe regs / host1x / syncpt
# / channel / boot / method.  Enable the one-shot line once M1 is verified.
# @/drivers/orin.nano/vicprobe all
# GA10B GPU 3D bring-up (ga10bd, alias /bin/ga): NEVER auto-started.  First
# GPU MMIO touch is unverified; run stages by hand from the rescue console:
#   ga fw          (blob validation, no MMIO)
#   ga --hw probe  (BPMP pg35/clks/reset19 + boot_0 + priv_sec_en fuse)
#   ga --hw init / acr / ce / gr   (escalating; acr = ACR-GSP HS boot)
# Firmware blobs live at /fs/nvidia/ga10b (packed from GPU_FW_DIR, not in git).
# Resident GPU service: idle at boot (no MMIO until the first client OPEN),
# then the single GPU owner shared by kuda apps and g2dd's 3D offload.
# While gpud holds the GPU the ga10bd CLI stages are refused (one owner).
@/bin/ipcserv /drivers/orin.nano/gpud /dev/gpu
# XUSB host: shared usbhostd (enumeration/hub/HID/MSC) over the machine
# bsp_usb contract; hid_keybd/hid_moused consume HID reports.  MSC sticks
# auto-mount at /mnt/udisk0 via usbfat32fsd.
@/bin/ipcserv /drivers/orin.nano/usbhostd  /dev/usb0
@/bin/ipcserv /drivers/orin.nano/hid_keybd /dev/keyb0 /dev/usb0
@/bin/ipcserv /drivers/orin.nano/hid_moused /dev/mouse0 /dev/usb0