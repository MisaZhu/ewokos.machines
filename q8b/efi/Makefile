# Q8B EFI boot bundle.
#
# Build kernel/ and system/ first.  This Makefile only validates those build
# products and packages them with the EFI application; it does not invoke
# either source tree's build targets.

KERNEL_INPUT = ../kernel/kernel.bin
SYSTEM_ROOT = ../system/build_aarch64
RAMFS_STAGING = build/initramfs
# The wlan/net test payload (nfa765 amss + board-2/regdb) needs more than the
# bare switchroot image did.
INITRAMFS_MB ?= 24
# SELF_ROOT=1 (default) rewrites rootfs=auto to rootfs=self in the staged
# kernel.conf, so init keeps the boot RAMFS and never switchroots to SD.
# Pass SELF_ROOT=0 to restore the previous SD/auto behaviour.
SELF_ROOT ?= 1

KERNEL_IMAGE = kernel.bin
INITRAMFS_IMAGE = initramfs.img

INIT_RAMFS_SOURCE = $(SYSTEM_ROOT)/etc/init.ramfs.rd
# Only executed while the boot RAMFS stays root (SELF_ROOT=1); an SD-root
# boot replaces the root before /etc/init.rd is read.
INIT_SOURCE = $(SYSTEM_ROOT)/etc/init.self.rd
SYSTEM_FILES = \
	$(SYSTEM_ROOT)/etc/kernel/kernel.conf \
	$(SYSTEM_ROOT)/etc/passwd $(SYSTEM_ROOT)/etc/group \
	$(SYSTEM_ROOT)/etc/wlan/network.json \
	$(SYSTEM_ROOT)/etc/wlan.country \
	$(SYSTEM_ROOT)/etc/firmware/ath11k/WCN6855/hw2.1/nfa765/amss.bin \
	$(SYSTEM_ROOT)/etc/firmware/ath11k/WCN6855/hw2.1/nfa765/m3.bin \
	$(SYSTEM_ROOT)/etc/firmware/ath11k/WCN6855/hw2.1/board-2.bin \
	$(SYSTEM_ROOT)/etc/firmware/ath11k/WCN6855/hw2.1/regdb.bin \
	$(INIT_RAMFS_SOURCE) $(INIT_SOURCE) \
	$(SYSTEM_ROOT)/sbin/init $(SYSTEM_ROOT)/sbin/core \
	$(SYSTEM_ROOT)/sbin/vfsd $(SYSTEM_ROOT)/sbin/sdfsd \
	$(SYSTEM_ROOT)/sbin/switchroot $(SYSTEM_ROOT)/sbin/sessiond \
	$(SYSTEM_ROOT)/sbin/sshd \
	$(SYSTEM_ROOT)/bin/ipcserv $(SYSTEM_ROOT)/bin/shell \
	$(SYSTEM_ROOT)/bin/login \
	$(SYSTEM_ROOT)/bin/devcmd $(SYSTEM_ROOT)/bin/session \
	$(SYSTEM_ROOT)/bin/bgrun \
	$(SYSTEM_ROOT)/drivers/q8b/usbhostd \
	$(SYSTEM_ROOT)/drivers/q8b/usbfat32fsd \
	$(SYSTEM_ROOT)/drivers/q8b/wlan \
	$(SYSTEM_ROOT)/drivers/netd \
	$(SYSTEM_ROOT)/drivers/q8b/uartd \
	$(SYSTEM_ROOT)/drivers/q8b/pcied \
	$(SYSTEM_ROOT)/drivers/timerd \
	$(SYSTEM_ROOT)/drivers/piped $(SYSTEM_ROOT)/drivers/ramfsd \
	$(SYSTEM_ROOT)/drivers/nulld

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
LOADER_CFLAGS += -DQ8B_QEMU_TEST
endif
ifeq ($(KERNEL_ONLY),1)
LOADER_CFLAGS += -DQ8B_KERNEL_ONLY
endif
ifeq ($(SD_ROOTFS),1)
LOADER_CFLAGS += -DQ8B_SD_ROOTFS
endif
ifeq ($(FORCE_UART17),1)
LOADER_CFLAGS += -DQ8B_SERIAL_FORCE_UART17
endif
# TEMP: no serial console at all (E Go / TW220 bring-up).  The loader then
# never touches UART17 and the GOP framebuffer console is the only output.
ifeq ($(NO_SERIAL),1)
LOADER_CFLAGS += -DQ8B_NO_SERIAL
endif
ifeq ($(MTFTP_FIRST),1)
LOADER_CFLAGS += -DQ8B_MTFTP_FIRST
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
		echo "       from the repository root, build the kernel first: make -C machines/q8b/kernel"; \
		exit 1; \
	fi
	cp -p "$(KERNEL_INPUT)" "$@"

$(INITRAMFS_IMAGE): FORCE
	@missing=0; for f in $(SYSTEM_FILES); do \
		if [ ! -f "$$f" ]; then echo "ERROR: missing system output $$f"; missing=1; fi; \
	done; if [ $$missing -ne 0 ]; then \
		echo "       from the repository root, build the system first: make -C machines/q8b/system"; \
		exit 1; \
	fi
	@rm -rf "$(RAMFS_STAGING)"
	@mkdir -p "$(RAMFS_STAGING)/etc/kernel" "$(RAMFS_STAGING)/sbin" \
		"$(RAMFS_STAGING)/drivers/q8b" "$(RAMFS_STAGING)/drivers" \
		"$(RAMFS_STAGING)/bin" \
		"$(RAMFS_STAGING)/etc/firmware/ath11k/WCN6855/hw2.1/nfa765" \
		"$(RAMFS_STAGING)/etc/wlan"
	@cp -p "$(SYSTEM_ROOT)/etc/kernel/kernel.conf" "$(RAMFS_STAGING)/etc/kernel/"
	@if [ "$(SELF_ROOT)" = 1 ]; then \
		sed -i '' -e 's/^rootfs[ 	]*=.*auto/rootfs = self/' \
			"$(RAMFS_STAGING)/etc/kernel/kernel.conf"; \
	fi
	@cp -p "$(SYSTEM_ROOT)/etc/wlan/network.json" "$(RAMFS_STAGING)/etc/wlan/"
	@cp -p "$(SYSTEM_ROOT)/etc/wlan.country" "$(RAMFS_STAGING)/etc/"
	@cp -p "$(SYSTEM_ROOT)/etc/passwd" "$(SYSTEM_ROOT)/etc/group" \
		"$(RAMFS_STAGING)/etc/"
	@if [ -f "$(SYSTEM_ROOT)/etc/shadow" ]; then \
		cp -p "$(SYSTEM_ROOT)/etc/shadow" "$(RAMFS_STAGING)/etc/"; fi
	@cp -p "$(SYSTEM_ROOT)/etc/firmware/ath11k/WCN6855/hw2.1/nfa765/amss.bin" \
		"$(SYSTEM_ROOT)/etc/firmware/ath11k/WCN6855/hw2.1/nfa765/m3.bin" \
		"$(RAMFS_STAGING)/etc/firmware/ath11k/WCN6855/hw2.1/nfa765/"
	@cp -p "$(SYSTEM_ROOT)/etc/firmware/ath11k/WCN6855/hw2.1/board-2.bin" \
		"$(SYSTEM_ROOT)/etc/firmware/ath11k/WCN6855/hw2.1/regdb.bin" \
		"$(SYSTEM_ROOT)/etc/firmware/ath11k/WCN6855/hw2.1/m3.bin" \
		"$(RAMFS_STAGING)/etc/firmware/ath11k/WCN6855/hw2.1/"
	@cp -p "$(INIT_RAMFS_SOURCE)" "$(RAMFS_STAGING)/etc/init.ramfs.rd"
	@cp -p "$(INIT_SOURCE)" "$(RAMFS_STAGING)/etc/init.rd"
	@cp -p "$(SYSTEM_ROOT)/sbin/init" "$(SYSTEM_ROOT)/sbin/core" \
		"$(SYSTEM_ROOT)/sbin/vfsd" "$(SYSTEM_ROOT)/sbin/sdfsd" \
		"$(SYSTEM_ROOT)/sbin/switchroot" "$(SYSTEM_ROOT)/sbin/sessiond" \
		"$(SYSTEM_ROOT)/sbin/sshd" \
		"$(RAMFS_STAGING)/sbin/"
	@cp -p "$(SYSTEM_ROOT)/bin/ipcserv" "$(SYSTEM_ROOT)/bin/shell" \
		"$(SYSTEM_ROOT)/bin/login" \
		"$(SYSTEM_ROOT)/bin/devcmd" "$(SYSTEM_ROOT)/bin/session" \
		"$(SYSTEM_ROOT)/bin/bgrun" \
		"$(RAMFS_STAGING)/bin/"
	@cp -p "$(SYSTEM_ROOT)/drivers/q8b/usbhostd" \
		"$(SYSTEM_ROOT)/drivers/q8b/usbfat32fsd" \
		"$(SYSTEM_ROOT)/drivers/q8b/net" \
		"$(SYSTEM_ROOT)/drivers/q8b/uartd" \
		"$(SYSTEM_ROOT)/drivers/q8b/pcied" "$(SYSTEM_ROOT)/drivers/q8b/wlan" \
		"$(RAMFS_STAGING)/drivers/q8b/"
	@cp -p "$(SYSTEM_ROOT)/drivers/netd" "$(RAMFS_STAGING)/drivers/"
	@cp -p "$(SYSTEM_ROOT)/drivers/logd" "$(RAMFS_STAGING)/drivers/"
	@cp -p "$(SYSTEM_ROOT)/drivers/timerd" \
		"$(SYSTEM_ROOT)/drivers/piped" "$(SYSTEM_ROOT)/drivers/ramfsd" \
		"$(SYSTEM_ROOT)/drivers/nulld" "$(RAMFS_STAGING)/drivers/"
	@dd if=/dev/zero of="$@" bs=1k count=$$(expr $(INITRAMFS_MB) \* 1024)
	@mke2fs -t ext3 -b 4096 -I 128 "$@"
	@cd "$(RAMFS_STAGING)" && find . -type f | e2cp -ap -G0 -O0 -d "$(abspath $@):$(empty)/"
	@echo "==== $@ created from prebuilt kernel/system outputs ===="

$(LOADER_EFI): $(LOADER_ELF) mkpefi.py
	python3 mkpefi.py $(LOADER_ELF) $(LOADER_EFI) 10

$(LOADER_ELF): $(LOADER_OBJ) linker.ld
	$(LD) $(LOADER_OBJ) $(LOADER_LDFLAGS) -o $@

$(LOADER_OBJ): main.c efi.h
	$(CC) $(LOADER_CFLAGS) -c $< -o $@

clean:
	rm -f $(LOADER_OBJ) $(LOADER_ELF) $(LOADER_EFI) \
		$(KERNEL_IMAGE) $(INITRAMFS_IMAGE)
	rm -rf $(RAMFS_STAGING)

FORCE:
