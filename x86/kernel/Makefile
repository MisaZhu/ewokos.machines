include config.mk

LDS = mkos.lds.S

OS_IMG = kernel.img
OS_ELF = kernel.elf
OS_BOOT = kernel.boot
BOOT_DIR = boot
BOOT_SRC = $(BOOT_DIR)/bios_boot.S
BOOT_CFG = $(BOOT_DIR)/bootcfg.inc
BOOT_OBJ = $(BOOT_DIR)/bootsect.o
BOOT_BIN = $(BOOT_DIR)/bootsect.bin
QEMU_CMD = qemu-system-x86_64
QEMU_FLAGS = -M $(QEMU_MACHINE) -m 512 -serial mon:stdio -no-reboot -no-shutdown
QEMU_ARGS += -boot a
QEMU_ARGS += -drive file=$(OS_BOOT),format=raw,if=floppy
QEMU_ARGS += -drive file=../system/root_$(ARCH).ext2,format=raw,if=ide,index=0,media=disk

MACHINE = x86

ifeq ($(ewokos),)
ewokos=~/work/ewokos
endif
EWOKOS_ROOT_DIR = $(ewokos)

include make.bsp
include $(EWOKOS_ROOT_DIR)/kernel/platform/$(ARCH)/make.rule


ARCH_LD_FLAGS = --defsym=start_address=$(LOAD_ADDRESS)


$(BOOT_DIR):
	mkdir -p $(BOOT_DIR)

$(BOOT_CFG): $(OS_IMG) $(OS_ELF) | $(BOOT_DIR)
	@python3 $(BOOT_DIR)/mkboot.py cfg $(LOAD_ADDRESS) $(CROSS_COMPILE)readelf $(OS_ELF) $(OS_IMG) > $(BOOT_CFG)

$(BOOT_OBJ): $(BOOT_SRC) $(BOOT_CFG)
	$(AS) --32 -I $(BOOT_DIR) -o $(BOOT_OBJ) $(BOOT_SRC)

$(BOOT_BIN): $(BOOT_OBJ)
	$(LD) -m elf_i386 -Ttext 0x7c00 --oformat binary -nostdlib -e _start -o $(BOOT_BIN) $(BOOT_OBJ)

$(OS_BOOT): $(BOOT_BIN) $(OS_IMG)
	@python3 $(BOOT_DIR)/mkboot.py image $(BOOT_BIN) $(OS_IMG) $(OS_BOOT)

asm:
	$(OBJDUMP) -D $(BUILD_DIR)/$(OS_ELF) > $(BUILD_DIR)/kernel.asm

run: $(OS_BOOT)
	$(QEMU_CMD) $(QEMU_FLAGS) $(QEMU_ARGS)

runasm: $(OS_BOOT)
	$(QEMU_CMD) $(QEMU_FLAGS) $(QEMU_ARGS) -d in_asm

debug: $(OS_BOOT)
	$(QEMU_CMD) $(QEMU_FLAGS) -gdb tcp::26000 -S $(QEMU_ARGS)

gdb:
	echo "target remote :26000" > /tmp/gdbinit
	x86_64-elf-gdb $(BUILD_DIR)/$(OS_ELF) -x /tmp/gdbinit
