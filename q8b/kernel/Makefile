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

.PHONY: all asm run clean

all: $(OS_IMG)


asm:
	$(OBJDUMP) -D $(OS_ELF) > kernel.asm

run:
	@echo "Q8B is a real board: put the kernel image and rootfs on the boot media."
	@echo "QEMU has no sc8280xp machine model."
