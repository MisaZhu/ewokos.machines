include config.mk

LDS = mkos.lds.S

# shared machine headers (q8b_platform.h)
CFLAGS += -I../include

ifeq ($(TEST_QEMU),1)
OS_IMG = kernel_qemu.bin
OS_ELF = kernel_qemu.elf
else
OS_IMG = kernel$(KERNEL_VARIANT).bin
OS_ELF = kernel$(KERNEL_VARIANT).elf
endif

QEMU_CMD = qemu-system-aarch64
ARCH_CFLAGS = -Wa,-march=armv8-a
QEMU_FLAGS = -M $(QEMU_MACHINE) -serial null -serial mon:stdio

ifeq ($(ewokos),)
ewokos=~/work/ewokos
endif
EWOKOS_ROOT_DIR = $(ewokos)
FINAL_SYS_DIR = ../system

ARCH_LD_FLAGS = --defsym=start_address=$(LOAD_ADDRESS)
ifeq ($(PAGE_SIZE),16k)
ARCH_LD_FLAGS += --defsym=page_align_size=0x4000
else ifeq ($(PAGE_SIZE),64k)
ARCH_LD_FLAGS += --defsym=page_align_size=0x10000
else
ARCH_LD_FLAGS += --defsym=page_align_size=0x1000
endif

include make.bsp
include $(EWOKOS_ROOT_DIR)/kernel/platform/${ARCH}/make.rule

# The high-half VMA offset must use this machine's (or test build's) RAM base.
ARCH_LD_FLAGS += --defsym=phys_base=$(RAM_BASE)

QEMU_ARGS += -kernel $(OS_IMG)
QEMU_ARGS += -drive file=$(FINAL_SYS_DIR)/root_$(ARCH).img,if=none,id=hd0 \
	-device virtio-blk-device,drive=hd0

# ---------------------------------------------------------------------------
# UEFI bootloader (BOOTAA64.EFI)
#
# The Q8B boots from its on-board EDK2 UEFI (SPI NOR).  Boot Maintenance
# Manager -> Boot From File, a UEFI shell, or PXE/GRUB all end up running
# this PE32+ application.  It loads KERNEL.BIN and either loads ROOTFS.IMG
# (RAM-disk mode) or leaves rootfs loading to the kernel's SDCC2 backend
# (SD_ROOTFS=1), fills in the Q8B bootinfo page and jumps to LOAD_ADDRESS.
# ---------------------------------------------------------------------------
UEFI_DIR    = boot/uefi
UEFI_ELF    = $(UEFI_DIR)/boot.elf
UEFI_EFI    = $(UEFI_DIR)/BOOTAA64.EFI
UEFI_SRCS   = $(UEFI_DIR)/main.c
UEFI_OBJS   = $(patsubst $(UEFI_DIR)/%.c,$(UEFI_DIR)/%.o,$(UEFI_SRCS))
MTFTP_FIRST ?= 1
ifeq ($(KERNEL_ONLY),1)
UEFI_CFLAGS += -DQ8B_KERNEL_ONLY
endif
ifeq ($(SD_ROOTFS),1)
UEFI_CFLAGS += -DQ8B_SD_ROOTFS
endif
ifeq ($(FORCE_UART17),1)
UEFI_CFLAGS += -DQ8B_SERIAL_FORCE_UART17
endif
ifeq ($(MTFTP_FIRST),1)
UEFI_CFLAGS += -DQ8B_MTFTP_FIRST
endif
UEFI_CFLAGS += -ffreestanding -fno-stack-protector -fno-stack-check \
	-fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fPIE -fvisibility=hidden -mgeneral-regs-only -mstrict-align \
	-O2 -Wall -Wextra -I$(UEFI_DIR)
UEFI_LDFLAGS = -T $(UEFI_DIR)/linker.ld -e efi_main

.PHONY: all efi payload asm run clean

all: $(OS_IMG)

efi: $(UEFI_EFI)

payload: $(OS_IMG) $(UEFI_EFI)
	@echo "payload: $(abspath $(OS_IMG)) $(abspath $(UEFI_EFI))"

$(UEFI_EFI): $(UEFI_ELF) $(UEFI_DIR)/mkpefi.py
	python3 $(UEFI_DIR)/mkpefi.py $(UEFI_ELF) $(UEFI_EFI) 10

$(UEFI_ELF): $(UEFI_OBJS) $(UEFI_DIR)/linker.ld
	$(LD) $(UEFI_OBJS) $(UEFI_LDFLAGS) -o $@

$(UEFI_DIR)/%.o: $(UEFI_DIR)/%.c
	$(CC) $(UEFI_CFLAGS) -c $< -o $@

asm:
	$(OBJDUMP) -D $(OS_ELF) > kernel.asm

run:
	@echo "Q8B is a real board: put kernel.bin + BOOTAA64.EFI + rootfs.img"
	@echo "on the ESP/TFTP root and boot it from the Q8B UEFI firmware."
	@echo "QEMU has no sc8280xp machine model."

clean:
	rm -f $(UEFI_OBJS) $(UEFI_ELF) $(UEFI_EFI)
