# Orin Nano EFI boot bundle.
#
# Build kernel/ and system/ first.  This Makefile only validates those build
# products and packages them with the EFI application; it does not invoke
# either source tree's build targets.
#
# Deploy to the ESP (FAT32) of the boot medium (SD/USB/NVMe):
#   EFI/BOOT/BOOTAA64.EFI   <- BOOTAA64.EFI
#   orin/KERNEL.BIN         <- kernel.bin
#   orin/INITRAMFS.IMG      <- initramfs.img

KERNEL_INPUT = ../kernel/kernel.bin
SYSTEM_ROOT = ../system/build_aarch64

RAMFS_STAGING = build/initramfs
INITRAMFS_IMAGE = initramfs.img
# 16MB: the ext3 journal (-J size=4) needs room; mke2fs rejects a 4MB
# journal inside an 8MB filesystem.
INITRAMFS_MB ?= 16
# SELF_ROOT=1 rewrites the staged kernel.conf rootfs policy to self, so init
# keeps the boot RAMFS and never switchroots (the packed profile is the
# serial rescue console).  This bring-up port has no persistent-disk drivers,
# so self-root is the default.
SELF_ROOT ?= 1

KERNEL_IMAGE = kernel.bin

INIT_RAMFS_SOURCE = ../system/etc/init.ramfs.rd
# No /etc/init.rd is packed: the boot RAMFS profile is bootstrap-only and
# init starts the serial rescue console directly when the rootfs policy is
# self (see init.c rescue_shell).
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
	$(SYSTEM_ROOT)/drivers/orin.nano/uartd \
	$(SYSTEM_ROOT)/drivers/orin.nano/net \
	$(SYSTEM_ROOT)/drivers/orin.nano/pcied \
	$(SYSTEM_ROOT)/drivers/orin.nano/i2cd \
	$(SYSTEM_ROOT)/drivers/orin.nano/orinmon \
	$(SYSTEM_ROOT)/drivers/orin.nano/gpiod \
	$(SYSTEM_ROOT)/drivers/orin.nano/sdprobe \
	$(SYSTEM_ROOT)/drivers/orin.nano/usbhostd \
	$(SYSTEM_ROOT)/drivers/orin.nano/hid_keybd \
	$(SYSTEM_ROOT)/drivers/orin.nano/hid_moused \
	$(SYSTEM_ROOT)/drivers/orin.nano/usbfat32fsd \
	$(SYSTEM_ROOT)/drivers/orin.nano/bpmpdiag \
	$(SYSTEM_ROOT)/drivers/netd \
	$(SYSTEM_ROOT)/drivers/timerd \
	$(SYSTEM_ROOT)/drivers/piped $(SYSTEM_ROOT)/drivers/ramfsd \
	$(SYSTEM_ROOT)/drivers/nulld

# Bring-up debug/inspection utilities for the serial rescue shell: register
# peek/poke (mmio), kernel state (sysinfo/svcinfo), device commands (devcmd),
# process/memory/fs inspection.  All are shared basic outputs (~4MB total).
DEBUG_TOOLS = mmio sysinfo svcinfo devcmd ps top dump elfinfo \
	cat ls echo grep find head more wc df mount \
	uname pwd rm mkdir cp kill killall sleep date clear vi md5sum test reboot
SYSTEM_FILES += $(addprefix $(SYSTEM_ROOT)/bin/,$(DEBUG_TOOLS))

CC = aarch64-none-elf-gcc
LD = aarch64-none-elf-ld

LOADER_ELF = boot.elf
LOADER_EFI = BOOTAA64.EFI
LOADER_OBJ = main.o

LOADER_CFLAGS = -ffreestanding -fno-stack-protector -fno-stack-check \
	-fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fPIE -fvisibility=hidden -mgeneral-regs-only -mstrict-align \
	-O2 -Wall -Wextra -I.
ifeq ($(WATCHDOG_TEST),1)
LOADER_CFLAGS += -DORIN_WATCHDOG_TEST
endif

LOADER_LDFLAGS = -T linker.ld -e efi_main

.PHONY: all efi kernel initramfs clean FORCE

all: $(LOADER_EFI) $(KERNEL_IMAGE) $(INITRAMFS_IMAGE)

efi: $(LOADER_EFI)
kernel: $(KERNEL_IMAGE)
initramfs: $(INITRAMFS_IMAGE)

$(KERNEL_IMAGE): FORCE
	@if [ ! -f "$(KERNEL_INPUT)" ]; then \
		echo "ERROR: missing $(KERNEL_INPUT)"; \
		echo "       from the repository root, build the kernel first: make -C machines/nvidia/orin.nano/kernel"; \
		exit 1; \
	fi
	cp -p "$(KERNEL_INPUT)" "$@"

$(INITRAMFS_IMAGE): FORCE
	@missing=0; for f in $(SYSTEM_FILES); do \
		if [ ! -f "$$f" ]; then echo "ERROR: missing system output $$f"; missing=1; fi; \
	done; if [ $$missing -ne 0 ]; then \
		echo "       from the repository root, build the system first: make -C machines/nvidia/orin.nano/system"; \
		exit 1; \
	fi
	@rm -rf "$(RAMFS_STAGING)"
	@mkdir -p "$(RAMFS_STAGING)/etc/kernel" "$(RAMFS_STAGING)/sbin" \
		"$(RAMFS_STAGING)/drivers/orin.nano" "$(RAMFS_STAGING)/drivers" \
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
	@cp -p "$(INIT_RAMFS_SOURCE)" "$(RAMFS_STAGING)/etc/init.ramfs.rd"
	@cp -p "$(SYSTEM_ROOT)/sbin/init" "$(SYSTEM_ROOT)/sbin/core" \
		"$(SYSTEM_ROOT)/sbin/vfsd" "$(SYSTEM_ROOT)/sbin/sdfsd" \
		"$(SYSTEM_ROOT)/sbin/switchroot" "$(SYSTEM_ROOT)/sbin/sessiond" \
		"$(RAMFS_STAGING)/sbin/"
	@cp -p "$(SYSTEM_ROOT)/bin/ipcserv" "$(SYSTEM_ROOT)/bin/shell" \
		"$(SYSTEM_ROOT)/bin/login" \
		"$(SYSTEM_ROOT)/bin/session" \
		"$(SYSTEM_ROOT)/bin/bgrun" \
		"$(RAMFS_STAGING)/bin/"
	@for t in $(DEBUG_TOOLS); do \
		cp -p "$(SYSTEM_ROOT)/bin/$$t" "$(RAMFS_STAGING)/bin/"; done
	@cp -p "$(SYSTEM_ROOT)/drivers/orin.nano/bpmpdiag" "$(RAMFS_STAGING)/bin/bp"
	@cp -p "$(SYSTEM_ROOT)/drivers/orin.nano/uartd" \
		"$(SYSTEM_ROOT)/drivers/orin.nano/net" \
		"$(SYSTEM_ROOT)/drivers/orin.nano/pcied" \
		"$(SYSTEM_ROOT)/drivers/orin.nano/i2cd" \
		"$(SYSTEM_ROOT)/drivers/orin.nano/orinmon" \
		"$(SYSTEM_ROOT)/drivers/orin.nano/gpiod" \
		"$(SYSTEM_ROOT)/drivers/orin.nano/sdprobe" \
		"$(SYSTEM_ROOT)/drivers/orin.nano/usbhostd" \
		"$(SYSTEM_ROOT)/drivers/orin.nano/hid_keybd" \
		"$(SYSTEM_ROOT)/drivers/orin.nano/hid_moused" \
		"$(SYSTEM_ROOT)/drivers/orin.nano/usbfat32fsd" \
		"$(SYSTEM_ROOT)/drivers/orin.nano/bpmpdiag" \
		"$(RAMFS_STAGING)/drivers/orin.nano/"
	@cp -p "$(SYSTEM_ROOT)/drivers/netd" \
		"$(SYSTEM_ROOT)/drivers/timerd" \
		"$(SYSTEM_ROOT)/drivers/piped" "$(SYSTEM_ROOT)/drivers/ramfsd" \
		"$(SYSTEM_ROOT)/drivers/nulld" "$(RAMFS_STAGING)/drivers/"
	@dd if=/dev/zero of="$@" bs=1k count=$$(expr $(INITRAMFS_MB) \* 1024)
	@mke2fs -t ext3 -b 4096 -I 128 -J size=4 "$@"
	@cd "$(RAMFS_STAGING)" && find . -type f | e2cp -ap -G0 -O0 -d "$(abspath $@):$(empty)/"
	@echo "==== $@ created from prebuilt kernel/system outputs ===="

$(LOADER_EFI): $(LOADER_ELF) mkpefi.py
	python3 mkpefi.py $(LOADER_ELF) $(LOADER_EFI) 10

$(LOADER_ELF): $(LOADER_OBJ) linker.ld
	$(LD) $(LOADER_OBJ) $(LOADER_LDFLAGS) -o $@

# Always rebuild the loader: WATCHDOG_TEST is a command-line variable make
# cannot track as a dependency, and a stale main.o would silently drop the
# -DORIN_WATCHDOG_TEST code.  The compile is cheap; correctness wins.
$(LOADER_OBJ): main.c efi.h FORCE
	$(CC) $(LOADER_CFLAGS) -c $< -o $@

clean:
	rm -f $(LOADER_OBJ) $(LOADER_ELF) $(LOADER_EFI) \
		$(KERNEL_IMAGE) $(INITRAMFS_IMAGE)
	rm -rf build/initramfs

FORCE: