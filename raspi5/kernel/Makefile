include config.mk

LDS = mkos.lds.S

OS_IMG = kernel_2712.img
OS_ELF = kernel_2712.elf
QEMU_CMD = qemu-system-aarch64
ARCH_CFLAGS = -Wa,-march=armv8-a
QEMU_FLAGS = -M $(QEMU_MACHINE) -serial null -serial mon:stdio

ifeq ($(ewokos),)
ewokos=~/work/ewokos
endif
EWOKOS_ROOT_DIR = $(ewokos)
MACHINE = raspi5
SYS_DIR = $(EWOKOS_ROOT_DIR)/system/build/$(MACHINE)

ifeq ($(QEMU_DISPLAY_OPTS),cocoa)
QEMU_FLAGS += -display cocoa
endif

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

QEMU_ARGS += -kernel $(OS_IMG)
QEMU_ARGS += -sd $(SYS_DIR)/root_$(ARCH).img

asm:
	$(OBJDUMP) -D $(BUILD_DIR)/$(OS_ELF) > $(BUILD_DIR)/kernel.asm

run:
	$(QEMU_CMD) $(QEMU_FLAGS) $(QEMU_ARGS)

runasm:
	$(QEMU_CMD) $(QEMU_FLAGS) $(QEMU_ARGS) -d in_asm

debug:
	$(QEMU_CMD) $(QEMU_FLAGS) -gdb tcp::26000 -S $(QEMU_ARGS)

debugasm:
	$(QEMU_CMD) $(QEMU_FLAGS) -d in_asm -gdb tcp::26000 -S $(QEMU_ARGS)

gdb:
	echo "target remote :26000" > /tmp/gdbinit
	aarch64-none-elf-gdb $(BUILD_DIR)/$(OS_ELF) -x /tmp/gdbinit
