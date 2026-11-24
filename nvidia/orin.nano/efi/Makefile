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

# GA10B GPU firmware blobs (NVIDIA proprietary, extracted from the L4T
# nvidia-l4t-firmware deb).  Kept OUTSIDE the repo; packed into the ramfs
# at /fs/nvidia/ga10b (ga10bd --fw-dir default) only when the dir exists.
GPU_FW_DIR ?= /Users/tony/work/gpu-fw/ga10b
# nvcc sm_87 test kernels for `ga10bd compute` (/fs/cuda), when present.
CUDA_CUBIN_DIR ?= ../../../../projects/cuda/cubin

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
	$(SYSTEM_ROOT)/drivers/orin.nano/dceprobe \
	$(SYSTEM_ROOT)/drivers/orin.nano/fand \
	$(SYSTEM_ROOT)/drivers/orin.nano/vicprobe \
	$(SYSTEM_ROOT)/drivers/orin.nano/ga10bd \
	$(SYSTEM_ROOT)/drivers/orin.nano/g2dd \
	$(SYSTEM_ROOT)/drivers/orin.nano/g3dtest \
	$(SYSTEM_ROOT)/drivers/orin.nano/firmware/vic.bin \
	$(SYSTEM_ROOT)/drivers/orin.nano/firmware/LICENCE.nvidia \
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

# GUI=1 packs the framebuffer GUI: the display stack (displaymand/fbdisplayd/
# fontd/consoled), the self-root GUI profile (/etc/init.rd, which init runs
# straight from the RAMFS) and the font/splash assets.  Build the system with
# `make gui` first.  Default 0 keeps the console-only bring-up image.
GUI ?= 0
ifeq ($(GUI),1)
SYSTEM_FILES += \
	$(SYSTEM_ROOT)/etc/init.rd \
	$(SYSTEM_ROOT)/drivers/displaymand \
	$(SYSTEM_ROOT)/drivers/fontd \
	$(SYSTEM_ROOT)/drivers/consoled \
	$(SYSTEM_ROOT)/drivers/orin.nano/fbdisplayd \
	$(SYSTEM_ROOT)/usr/system/fonts/system.ttf \
	$(SYSTEM_ROOT)/usr/system/fonts.json \
	$(SYSTEM_ROOT)/usr/system/icons/64bits.png
endif

# X=1 packs the full xwin desktop: instead of the explicit bring-up file list,
# the ENTIRE built rootfs (basic + usb + gui + xwin + assets) is staged, so
# every desktop binary, theme, icon and font is present without enumerating
# the xwin asset closure by hand.  Build the system with `make x` first.
# Bulky app-only data Orin cannot use is trimmed (see the recipe).  X=1
# supersedes GUI=1 (the whole rootfs already contains the GUI stack).
X ?= 0

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

ifeq ($(X),1)
# Full xwin desktop image: stage the whole built rootfs, then trim the bulky
# app-only data Orin cannot use -- no audio (data/sounds, usr/system/sounds),
# no books/games (data/), and the 10MB Chinese font system-cn.ttf (fontd loads
# fonts lazily on request, so dropping it only affects clients that ask for
# Chinese glyphs; the desktop uses the "system" font).  The ext3 image is sized
# to the trimmed content + 8MB margin (journal + metadata).
$(INITRAMFS_IMAGE): FORCE
	@rm -rf "$(RAMFS_STAGING)"
	@mkdir -p "$(RAMFS_STAGING)"
	@cp -Rp "$(SYSTEM_ROOT)/." "$(RAMFS_STAGING)/"
	@rm -rf "$(RAMFS_STAGING)/data" "$(RAMFS_STAGING)/usr/system/sounds"
	@rm -f "$(RAMFS_STAGING)/usr/system/fonts/system-cn.ttf"
	@mkdir -p "$(RAMFS_STAGING)/home/misa" "$(RAMFS_STAGING)/home/root"
	# init.ramfs.rd is a machine etc SOURCE (not a build output), so the
	# whole-rootfs cp above does not carry it.  It is the boot bootstrap
	# (timerd/ramfsd/piped/nulld + USB HID + net) that init runs BEFORE the
	# self-root UI profile -- without it the desktop has no timers, /tmp or
	# keyboard/mouse.  Copy it explicitly, like the bring-up recipe does.
	@cp -p "$(INIT_RAMFS_SOURCE)" "$(RAMFS_STAGING)/etc/init.ramfs.rd"
	# GA10B firmware for g2dd's opt-in GPU bring-up (GA10B_ENABLE=1); the
	# directory lives outside the repository and is packed only when present.
	@if [ -d "$(GPU_FW_DIR)" ]; then \
		mkdir -p "$(RAMFS_STAGING)/fs/nvidia/ga10b"; \
		cp -p "$(GPU_FW_DIR)"/*.bin "$(GPU_FW_DIR)"/*.prod \
			"$(RAMFS_STAGING)/fs/nvidia/ga10b/"; \
	fi
	@sed -i '' -E 's/^[[:space:]]*rootfs[[:space:]]*=[[:space:]]*[A-Za-z]+.*/rootfs = self/' \
		"$(RAMFS_STAGING)/etc/kernel/kernel.conf"
	@grep -q '^rootfs = self' "$(RAMFS_STAGING)/etc/kernel/kernel.conf" || { \
		echo "ERROR: failed to force rootfs=self in kernel.conf"; exit 1; }
	@mb=$$(du -sm "$(RAMFS_STAGING)" | cut -f1); mb=$$((mb + 8)); \
	 echo "==== staging full xwin rootfs ($${mb} MB) ===="; \
	 dd if=/dev/zero of="$@" bs=1k count=$$((mb * 1024)); \
	 mke2fs -t ext3 -b 4096 -I 128 -J size=4 "$@"; \
	 cd "$(RAMFS_STAGING)" && find . -type f | e2cp -ap -G0 -O0 -d "$(abspath $@):$(empty)/"
	@echo "==== $@ created (full xwin desktop rootfs) ===="
	@python3 mklz4.py "$@" "$@.lz4"
else
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
	@cp -p "$(SYSTEM_ROOT)/drivers/orin.nano/dceprobe" "$(RAMFS_STAGING)/bin/dp"
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
		"$(SYSTEM_ROOT)/drivers/orin.nano/dceprobe" \
		"$(SYSTEM_ROOT)/drivers/orin.nano/fand" \
		"$(SYSTEM_ROOT)/drivers/orin.nano/vicprobe" \
		"$(SYSTEM_ROOT)/drivers/orin.nano/ga10bd" \
		"$(SYSTEM_ROOT)/drivers/orin.nano/g2dd" \
		"$(SYSTEM_ROOT)/drivers/orin.nano/g3dtest" \
		"$(RAMFS_STAGING)/drivers/orin.nano/"
	@cp -p "$(SYSTEM_ROOT)/drivers/orin.nano/ga10bd" "$(RAMFS_STAGING)/bin/ga"
	@mkdir -p "$(RAMFS_STAGING)/drivers/orin.nano/firmware"
	@cp -p "$(SYSTEM_ROOT)/drivers/orin.nano/firmware/vic.bin" \
		"$(SYSTEM_ROOT)/drivers/orin.nano/firmware/LICENCE.nvidia" \
		"$(RAMFS_STAGING)/drivers/orin.nano/firmware/"
	@if [ -d "$(GPU_FW_DIR)" ]; then \
		mkdir -p "$(RAMFS_STAGING)/fs/nvidia/ga10b"; \
		cp -p "$(GPU_FW_DIR)"/*.bin "$(GPU_FW_DIR)"/*.prod \
			"$(RAMFS_STAGING)/fs/nvidia/ga10b/"; \
		echo "==== packed GA10B firmware from $(GPU_FW_DIR) ===="; \
	else \
		echo "NOTE: GPU_FW_DIR=$(GPU_FW_DIR) absent -- ga10bd runs dry-run only"; \
	fi
	@if [ -d "$(CUDA_CUBIN_DIR)" ]; then \
		mkdir -p "$(RAMFS_STAGING)/fs/cuda"; \
		cp -p "$(CUDA_CUBIN_DIR)"/*.cubin "$(RAMFS_STAGING)/fs/cuda/"; \
	fi
	@mkdir -p "$(RAMFS_STAGING)/fs/cuda"
	@cp -p ../system/libs/ga10b/kernels/*.cubin "$(RAMFS_STAGING)/fs/cuda/"
	@cp -p "$(SYSTEM_ROOT)/drivers/netd" \
		"$(SYSTEM_ROOT)/drivers/timerd" \
		"$(SYSTEM_ROOT)/drivers/piped" "$(SYSTEM_ROOT)/drivers/ramfsd" \
		"$(SYSTEM_ROOT)/drivers/nulld" "$(RAMFS_STAGING)/drivers/"
	@if [ "$(GUI)" = 1 ]; then \
		mkdir -p "$(RAMFS_STAGING)/usr/system/fonts" \
			"$(RAMFS_STAGING)/usr/system/icons"; \
		cp -p "$(SYSTEM_ROOT)/etc/init.rd" "$(RAMFS_STAGING)/etc/"; \
		cp -p "$(SYSTEM_ROOT)/drivers/displaymand" \
			"$(SYSTEM_ROOT)/drivers/fontd" \
			"$(SYSTEM_ROOT)/drivers/consoled" "$(RAMFS_STAGING)/drivers/"; \
		cp -p "$(SYSTEM_ROOT)/drivers/orin.nano/fbdisplayd" \
			"$(RAMFS_STAGING)/drivers/orin.nano/"; \
		cp -p "$(SYSTEM_ROOT)/usr/system/fonts/system.ttf" \
			"$(RAMFS_STAGING)/usr/system/fonts/"; \
		cp -p "$(SYSTEM_ROOT)/usr/system/fonts.json" \
			"$(RAMFS_STAGING)/usr/system/"; \
		cp -p "$(SYSTEM_ROOT)/usr/system/icons/64bits.png" \
			"$(RAMFS_STAGING)/usr/system/icons/"; \
	fi
	@dd if=/dev/zero of="$@" bs=1k count=$$(expr $(INITRAMFS_MB) \* 1024)
	@mke2fs -t ext3 -b 4096 -I 128 -J size=4 "$@"
	@cd "$(RAMFS_STAGING)" && find . -type f | e2cp -ap -G0 -O0 -d "$(abspath $@):$(empty)/"
	@echo "==== $@ created from prebuilt kernel/system outputs ===="
	@python3 mklz4.py "$@" "$@.lz4"
endif

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
		$(KERNEL_IMAGE) $(INITRAMFS_IMAGE) $(INITRAMFS_IMAGE).lz4
	rm -rf build/initramfs

FORCE: