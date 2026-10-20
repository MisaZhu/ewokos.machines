# Q8B (Radxa Dragon Q8B / SC8280XP) machine targets
#
#   make payload    kernel + BOOTAA64.EFI (PXE); rootfs stays on SD
#   make deploy     payload + copy to ../../../q8b/boot/tftp/q8b + ESP image
#   make test-qemu  two QEMU/EDK2 smoke tests (loader and kernel boot)
#   make clean
#
# The cross toolchain is expected on PATH (aarch64-none-elf-*); on this
# machine it lives in
#   /Users/tony/toolchain/arm-gnu-toolchain-14.3.rel1-darwin-arm64-aarch64-none-elf/bin

KERNEL_DIR = kernel
SYSTEM_DIR = system
ROOTFS_IMG = root_aarch64.img
KERNEL_BIN = $(KERNEL_DIR)/kernel.bin
STUB_EFI   = $(KERNEL_DIR)/boot/uefi/BOOTAA64.EFI
SD_ROOTFS ?= 1
# During board bring-up, prefer the kernel supplied by the PXE server.  The
# local EFI partition remains a fallback when networking is unavailable.
MTFTP_FIRST ?= 1

.PHONY: all payload kernel stub rootfs test-qemu test-loader deploy clean

all: payload

kernel:
	$(MAKE) -C $(KERNEL_DIR)

stub: kernel
	$(MAKE) -C $(KERNEL_DIR) efi SD_ROOTFS=$(SD_ROOTFS) MTFTP_FIRST=$(MTFTP_FIRST)

rootfs:
	$(MAKE) -C $(SYSTEM_DIR) gui
	$(MAKE) -C $(SYSTEM_DIR) sd

payload: stub rootfs
	@echo ""
	@echo "payload ready:"
	@ls -l $(KERNEL_BIN) $(STUB_EFI)
	@if [ "$(SD_ROOTFS)" != 1 ]; then ls -l $(ROOTFS_IMG); fi

test-loader:
	$(KERNEL_DIR)/boot/uefi/test/run_qemu.sh

test-kernel:
	$(MAKE) -C $(KERNEL_DIR) TEST_QEMU=1
	$(KERNEL_DIR)/boot/uefi/test/run_kernel_qemu.sh

test-qemu: test-loader test-kernel

deploy: payload
	./deploy.sh

clean:
	$(MAKE) -C $(KERNEL_DIR) clean
	$(MAKE) -C $(SYSTEM_DIR) clean
	rm -f $(ROOTFS_IMG)
