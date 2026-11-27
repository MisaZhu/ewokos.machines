# QCOM8GEN3 EFI boot bundle.
#
# Build kernel/ and system/ first.  This Makefile only validates those build
# products and packages them with the EFI application; it does not invoke
# either source tree's build targets.

KERNEL_INPUT = ../kernel/kernel.bin
SYSTEM_ROOT = ../system/build_aarch64

# WIFI_TEST=1 builds the minimal self-root Wi-Fi image: the boot RAMFS carries
# the WLAN driver, netd, devcmd, the QCNFA765 (nfa765) firmware set and
# /etc/wlan/network.json; init brings WLAN up from that RAMFS and stays there
# (rootfs is forced to self, switchroot never runs).  The serial rescue console
# is started by init's self-root path, so the image needs no persistent disk.
# WIFI_TEST=0 keeps the production bootstrap image and its policy untouched.
WIFI_TEST ?= 0
WIFI_BAND ?= auto
ifeq ($(WIFI_TEST),1)
ifneq ($(words $(WIFI_BAND)),1)
$(error WIFI_BAND must be auto, 2.4 or 5)
endif
ifeq ($(filter $(WIFI_BAND),auto 2.4 5),)
$(error WIFI_BAND must be auto, 2.4 or 5)
endif
RAMFS_STAGING = build/initramfs-wifi
INITRAMFS_IMAGE = initramfs-wifi.img
INITRAMFS_MB ?= 24
SELF_ROOT = 1
else
# Bootstrap-only image: just the drivers needed to find and read the rootfs
# plus a serial rescue console.
RAMFS_STAGING = build/initramfs
INITRAMFS_IMAGE = initramfs.img
# 16MB: the ext3 journal (-J size=4) needs room; mke2fs rejects a 4MB
# journal inside an 8MB filesystem (128-byte inodes).
INITRAMFS_MB ?= 16
# SELF_ROOT=1 rewrites the staged kernel.conf rootfs policy to self, so init
# keeps the boot RAMFS and never switchroots (maintenance mode; the packed
# profile is the serial rescue console).  Default 0: normal boot selects the
# persistent rootfs and switchroots to it.
SELF_ROOT ?= 0
endif

KERNEL_IMAGE = kernel.bin

NORMAL_RAMFS_SOURCE = ../system/etc/xwin/init.ramfs.rd
# Wi-Fi test boot profile.  It starts WLAN (and netd) from the boot RAMFS and
# deliberately starts no console daemons: init's self-root path runs the serial
# rescue console after this profile returns, so uartd/sessiond/session are
# started exactly once.
WIFI_RAMFS_SOURCE = wifi.ramfs.rd
INIT_RAMFS_SOURCE = $(if $(filter 1,$(WIFI_TEST)),$(WIFI_RAMFS_SOURCE),$(NORMAL_RAMFS_SOURCE))
# No /etc/init.rd is packed: the boot RAMFS profile is bootstrap-only and
# init starts the serial rescue console directly when the rootfs cannot be
# mounted (see init.c rescue_shell).
SYSTEM_FILES = \
	$(SYSTEM_ROOT)/etc/kernel/kernel.conf \
	$(SYSTEM_ROOT)/etc/passwd $(SYSTEM_ROOT)/etc/group \
	$(INIT_RAMFS_SOURCE) \
	$(SYSTEM_ROOT)/sbin/init $(SYSTEM_ROOT)/sbin/core \
	$(SYSTEM_ROOT)/sbin/vfsd $(SYSTEM_ROOT)/sbin/sdfsd \
	$(SYSTEM_ROOT)/sbin/switchroot $(SYSTEM_ROOT)/sbin/sessiond \
	$(SYSTEM_ROOT)/bin/ipcserv $(SYSTEM_ROOT)/bin/shell \
	$(SYSTEM_ROOT)/bin/login \
	$(SYSTEM_ROOT)/bin/session \
	$(SYSTEM_ROOT)/bin/bgrun \
	$(SYSTEM_ROOT)/drivers/8gen3/usbhostd \
	$(SYSTEM_ROOT)/drivers/8gen3/usbfat32fsd \
	$(SYSTEM_ROOT)/drivers/8gen3/net \
	$(SYSTEM_ROOT)/drivers/8gen3/uartd \
	$(SYSTEM_ROOT)/drivers/8gen3/pcied \
	$(SYSTEM_ROOT)/drivers/timerd \
	$(SYSTEM_ROOT)/drivers/piped $(SYSTEM_ROOT)/drivers/ramfsd \
	$(SYSTEM_ROOT)/drivers/nulld

WIFI_FIRMWARE = ../firmware/ath11k/WCN6855/hw2.1
# QCNFA765 (17cb:0108 / 105b:e130) always selects the nfa765 AMSS/M3 payload;
# board-2.bin and regdb.bin remain the shared WCN6855 containers.  The
# byte-identical top-level amss/m3 copies are not packed.
ifeq ($(WIFI_TEST),1)
SYSTEM_FILES += \
	$(SYSTEM_ROOT)/drivers/8gen3/wlan \
	$(SYSTEM_ROOT)/drivers/netd \
	$(SYSTEM_ROOT)/bin/devcmd \
	$(SYSTEM_ROOT)/etc/wlan/network.json \
	$(SYSTEM_ROOT)/etc/wlan.country \
	$(WIFI_FIRMWARE)/board-2.bin \
	$(WIFI_FIRMWARE)/regdb.bin \
	$(WIFI_FIRMWARE)/nfa765/amss.bin \
	$(WIFI_FIRMWARE)/nfa765/m3.bin
endif

CC = aarch64-none-elf-gcc
LD = aarch64-none-elf-ld

LOADER_ELF = boot.elf
LOADER_EFI = BOOTAA64.EFI
LOADER_OBJ = main.o

LOADER_CFLAGS = -ffreestanding -fno-stack-protector -fno-stack-check \
	-fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fPIE -fvisibility=hidden -mgeneral-regs-only -mstrict-align \
	-O2 -Wall -Wextra -I.

ifeq ($(TEST_QEMU),1)
LOADER_CFLAGS += -DQCOM8GEN3_QEMU_TEST
endif
ifeq ($(KERNEL_ONLY),1)
LOADER_CFLAGS += -DQCOM8GEN3_KERNEL_ONLY
endif
ifeq ($(SD_ROOTFS),1)
LOADER_CFLAGS += -DQCOM8GEN3_SD_ROOTFS
endif
ifeq ($(FORCE_UART17),1)
LOADER_CFLAGS += -DQCOM8GEN3_SERIAL_FORCE_UART17
endif
# TEMP: no serial console at all (E Go / TW220 bring-up).  The loader then
# never touches UART17 and the GOP framebuffer console is the only output.
ifeq ($(NO_SERIAL),1)
LOADER_CFLAGS += -DQCOM8GEN3_NO_SERIAL
endif
ifeq ($(MTFTP_FIRST),1)
LOADER_CFLAGS += -DQCOM8GEN3_MTFTP_FIRST
endif

LOADER_LDFLAGS = -T linker.ld -e efi_main

.PHONY: all efi kernel initramfs wifi clean FORCE

all: $(LOADER_EFI) $(KERNEL_IMAGE) $(INITRAMFS_IMAGE)

efi: $(LOADER_EFI)
kernel: $(KERNEL_IMAGE)
initramfs: $(INITRAMFS_IMAGE)

# Minimal self-root WLAN test image (initramfs-wifi.img).  It never replaces
# the production initramfs.img; deploy it as INITRAMFS.IMG only when testing.
wifi:
	@$(MAKE) WIFI_TEST=1 initramfs

$(KERNEL_IMAGE): FORCE
	@if [ ! -f "$(KERNEL_INPUT)" ]; then \
		echo "ERROR: missing $(KERNEL_INPUT)"; \
		echo "       from the repository root, build the kernel first: make -C machines/qcom/8gen3/kernel"; \
		exit 1; \
	fi
	cp -p "$(KERNEL_INPUT)" "$@"

$(INITRAMFS_IMAGE): FORCE
	@missing=0; for f in $(SYSTEM_FILES); do \
		if [ ! -f "$$f" ]; then echo "ERROR: missing system output $$f"; missing=1; fi; \
	done; if [ $$missing -ne 0 ]; then \
		echo "       from the repository root, build the system first: make -C machines/qcom/8gen3/system"; \
		exit 1; \
	fi
	@rm -rf "$(RAMFS_STAGING)"
	@mkdir -p "$(RAMFS_STAGING)/etc/kernel" "$(RAMFS_STAGING)/sbin" \
		"$(RAMFS_STAGING)/drivers/8gen3" "$(RAMFS_STAGING)/drivers" \
		"$(RAMFS_STAGING)/bin"
	@cp -p "$(SYSTEM_ROOT)/etc/kernel/kernel.conf" "$(RAMFS_STAGING)/etc/kernel/"
	@if [ "$(SELF_ROOT)" = 1 ]; then \
		sed -i '' -E 's/^[[:space:]]*rootfs[[:space:]]*=[[:space:]]*[A-Za-z]+.*/rootfs = self/' \
			"$(RAMFS_STAGING)/etc/kernel/kernel.conf"; \
		grep -q '^rootfs = self' "$(RAMFS_STAGING)/etc/kernel/kernel.conf" || { \
			echo "ERROR: failed to force rootfs=self in kernel.conf"; exit 1; }; \
	fi
	@cp -p "$(SYSTEM_ROOT)/etc/passwd" "$(SYSTEM_ROOT)/etc/group" \
		"$(RAMFS_STAGING)/etc/"
	@if [ -f "$(SYSTEM_ROOT)/etc/shadow" ]; then \
		cp -p "$(SYSTEM_ROOT)/etc/shadow" "$(RAMFS_STAGING)/etc/"; fi
	@cp -p "$(INIT_RAMFS_SOURCE)" "$(RAMFS_STAGING)/etc/init.ramfs.rd"
ifeq ($(WIFI_TEST),1)
	@sed -i '' 's@/dev/wl0 --start$$@/dev/wl0 --start --band $(WIFI_BAND)@' \
		"$(RAMFS_STAGING)/etc/init.ramfs.rd"
endif
	@cp -p "$(SYSTEM_ROOT)/sbin/init" "$(SYSTEM_ROOT)/sbin/core" \
		"$(SYSTEM_ROOT)/sbin/vfsd" "$(SYSTEM_ROOT)/sbin/sdfsd" \
		"$(SYSTEM_ROOT)/sbin/switchroot" "$(SYSTEM_ROOT)/sbin/sessiond" \
		"$(RAMFS_STAGING)/sbin/"
	@cp -p "$(SYSTEM_ROOT)/bin/ipcserv" "$(SYSTEM_ROOT)/bin/shell" \
		"$(SYSTEM_ROOT)/bin/login" \
		"$(SYSTEM_ROOT)/bin/session" \
		"$(SYSTEM_ROOT)/bin/bgrun" \
		"$(RAMFS_STAGING)/bin/"
	@cp -p "$(SYSTEM_ROOT)/drivers/8gen3/usbhostd" \
		"$(SYSTEM_ROOT)/drivers/8gen3/usbfat32fsd" \
		"$(SYSTEM_ROOT)/drivers/8gen3/net" \
		"$(SYSTEM_ROOT)/drivers/8gen3/uartd" \
		"$(SYSTEM_ROOT)/drivers/8gen3/pcied" \
		"$(RAMFS_STAGING)/drivers/8gen3/"
	@cp -p "$(SYSTEM_ROOT)/drivers/timerd" \
		"$(SYSTEM_ROOT)/drivers/piped" "$(SYSTEM_ROOT)/drivers/ramfsd" \
		"$(SYSTEM_ROOT)/drivers/nulld" "$(RAMFS_STAGING)/drivers/"
ifeq ($(WIFI_TEST),1)
	@mkdir -p "$(RAMFS_STAGING)/etc/wlan" \
		"$(RAMFS_STAGING)/etc/firmware/ath11k/WCN6855/hw2.1/nfa765"
	@cp -p "$(SYSTEM_ROOT)/drivers/8gen3/wlan" \
		"$(RAMFS_STAGING)/drivers/8gen3/"
	@cp -p "$(SYSTEM_ROOT)/drivers/netd" "$(RAMFS_STAGING)/drivers/"
	@cp -p "$(SYSTEM_ROOT)/bin/devcmd" "$(RAMFS_STAGING)/bin/"
	@# Wi-Fi test images use the dedicated test credentials, not the
	@# persistent-rootfs network.json.
	@cp -p "wifi.network.json" \
		"$(RAMFS_STAGING)/etc/wlan/network.json"
	@cp -p "$(SYSTEM_ROOT)/etc/wlan.country" "$(RAMFS_STAGING)/etc/"
	@cp -p "$(WIFI_FIRMWARE)/board-2.bin" "$(WIFI_FIRMWARE)/regdb.bin" \
		"$(RAMFS_STAGING)/etc/firmware/ath11k/WCN6855/hw2.1/"
	@cp -p "$(WIFI_FIRMWARE)/nfa765/amss.bin" \
		"$(WIFI_FIRMWARE)/nfa765/m3.bin" \
		"$(RAMFS_STAGING)/etc/firmware/ath11k/WCN6855/hw2.1/nfa765/"
endif
	@dd if=/dev/zero of="$@" bs=1k count=$$(expr $(INITRAMFS_MB) \* 1024)
	@mke2fs -t ext3 -b 4096 -I 128 -J size=4 "$@"
	@cd "$(RAMFS_STAGING)" && find . -type f | e2cp -ap -G0 -O0 -d "$(abspath $@):$(empty)/"
	@echo "==== $@ created from prebuilt kernel/system outputs ===="
	@python3 mklz4.py "$@" "$@.lz4"

$(LOADER_EFI): $(LOADER_ELF) mkpefi.py
	python3 mkpefi.py $(LOADER_ELF) $(LOADER_EFI) 10

$(LOADER_ELF): $(LOADER_OBJ) linker.ld
	$(LD) $(LOADER_OBJ) $(LOADER_LDFLAGS) -o $@

$(LOADER_OBJ): main.c efi.h
	$(CC) $(LOADER_CFLAGS) -c $< -o $@

clean:
	rm -f $(LOADER_OBJ) $(LOADER_ELF) $(LOADER_EFI) \
		$(KERNEL_IMAGE) initramfs.img initramfs-wifi.img \
		initramfs.img.lz4 initramfs-wifi.img.lz4
	rm -rf build/initramfs build/initramfs-wifi

FORCE:
