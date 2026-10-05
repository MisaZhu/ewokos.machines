include config.mk

LDS = mkos.lds.S

OS_IMG = kernel.img
OS_ELF = kernel.elf
OS_BOOT = kernel.boot
OS_HDD = kernel.hdd
BOOT_DIR = boot
BOOT_SRC = $(BOOT_DIR)/bios_boot.S
BOOT_CFG = $(BOOT_DIR)/bootcfg.inc
BOOT_OBJ = $(ARCH)/$(BOOT_DIR)/bootsect.o
BOOT_BIN = $(BOOT_DIR)/bootsect.bin
CD_STAGE1 = $(BOOT_DIR)/cd_stage1.bin
CD_STAGE2 = $(BOOT_DIR)/cd_stage2.bin

# ---- UEFI bootloader (BOOTX64.EFI, 移植自 kernel-testsuite) ----
UEFI_DIR = $(BOOT_DIR)/uefi
UEFI_SRCS = $(UEFI_DIR)/main.c $(UEFI_DIR)/efi_stub.S
UEFI_OBJS = $(patsubst $(UEFI_DIR)/%.c,$(ARCH)/$(UEFI_DIR)/%.c.o,$(patsubst $(UEFI_DIR)/%.S,$(ARCH)/$(UEFI_DIR)/%.S.o,$(UEFI_SRCS)))
UEFI_ELF = $(UEFI_DIR)/boot.elf
UEFI_EFI = $(UEFI_DIR)/BOOTX64.EFI
UEFI_ESP = $(UEFI_DIR)/esp.img
UEFI_CFLAGS = -ffreestanding -fno-stack-protector -fno-stack-check \
	-fno-omit-frame-pointer -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
	-mno-80387 -fPIE -O2 -Wall -Wextra -I$(UEFI_DIR)
OVMF ?= /opt/homebrew/share/qemu/edk2-x86_64-code.fd
OVMF_FD = $(UEFI_DIR)/OVMF.fd
ESP_SIZE_MB = 64

# ---- 可引导 ISO (Ventoy/CDROM; UEFI 路径把 rootfs 拷入内存盘) ----
OS_ISO = ewokos.iso
ISO_ROOTFS = $(SYS_DIR)/root_$(ARCH).img
# ESP 4MB 足够 (只放 BOOTX64.EFI): mformat 默认生成合法 FAT12;
# 不可超 32MiB —— El Torito EFI 条目 SectorCount 为 16 位 (≤65535 个
# 512B 扇区)。注意: mformat 配方改动后必须 rm esp_iso.img 强制重建,
# 否则残留旧 FAT 类型的镜像导致 OVMF 挂载失败掉 UEFI Shell
ISO_ESP_SIZE_MB = 4
ISO_ESP = $(UEFI_DIR)/esp_iso.img

QEMU_CMD = qemu-system-x86_64
# 加速器: tcg(默认, 全平台可用) | kvm(Linux x64 宿主, make run ACCEL=kvm)
ACCEL ?= tcg
ifeq ($(ACCEL),kvm)
QEMU_ACCEL = -accel kvm -cpu host
else
QEMU_ACCEL = -accel tcg
endif
QEMU_FLAGS = $(QEMU_ACCEL) -M $(QEMU_MACHINE) -m 512 -serial mon:stdio -no-reboot -no-shutdown
ifeq ($(SMP),yes)
QEMU_FLAGS += -smp $(QEMU_SMP_CORES)
endif
QEMU_ARGS += -boot c
QEMU_ARGS += -drive file=$(OS_HDD),format=raw,if=ide,index=0,media=disk
QEMU_ARGS += -usb
QEMU_ARGS += -device usb-kbd,port=1,usb_version=1
QEMU_ARGS += -device usb-mouse,port=2,usb_version=1
QEMU_ARGS += -netdev user,id=net0,hostfwd=tcp::2222-:22,hostfwd=tcp::2223-:23
QEMU_ARGS += -device rtl8139,netdev=net0
# UEFI: OVMF 固件作为 pflash, 同一块双路径磁盘
QEMU_UEFI = -drive if=pflash,format=raw,file=$(OVMF_FD),readonly=on
# q35 机型: 无 legacy IDE, 根盘走 AHCI (内核 sd.c 自动回退 AHCI 路径)
QEMU_ARGS_Q35 = -drive file=$(OS_HDD),format=raw,if=none,id=hd0 -device ich9-ahci,id=ahci0 -device ide-hd,drive=hd0,bus=ahci0.0
QEMU_ARGS_Q35 += -usb
QEMU_ARGS_Q35 += -device usb-kbd,port=1,usb_version=1 -device usb-mouse,port=2,usb_version=1
# xHCI 机型变体: qemu-xhci 挂键鼠, 无 UHCI —— 验证用户态 xHCI HCD 路径
QEMU_ARGS_XHCI = -drive file=$(OS_HDD),format=raw,if=ide,index=0,media=disk
QEMU_ARGS_XHCI += -device qemu-xhci,id=xhci0
QEMU_ARGS_XHCI += -device usb-kbd,bus=xhci0.0 -device usb-mouse,bus=xhci0.0

MACHINE = x86

ifeq ($(ewokos),)
ewokos=~/work/ewokos
endif
EWOKOS_ROOT_DIR = $(ewokos)

MACHINE = x86
SYS_DIR = ../system
SYS_ROOTFS = $(SYS_DIR)/root_$(ARCH).img

CFLAGS += -I ../include
export CFLAGS

include make.bsp
include $(EWOKOS_ROOT_DIR)/kernel/platform/$(ARCH)/make.rule


ARCH_LD_FLAGS = --defsym=start_address=$(LOAD_ADDRESS)


$(BOOT_DIR):
	mkdir -p $(BOOT_DIR)

$(BOOT_CFG): $(OS_IMG) $(OS_ELF) | $(BOOT_DIR)
	@python3 $(BOOT_DIR)/mkboot.py cfg $(LOAD_ADDRESS) $(CROSS_COMPILE)readelf $(OS_ELF) $(OS_IMG) > $(BOOT_CFG)

$(BOOT_OBJ): $(BOOT_SRC) $(BOOT_CFG)
	@mkdir -p $(dir $(BOOT_OBJ))
	$(AS) --32 -I $(BOOT_DIR) -o $(BOOT_OBJ) $(BOOT_SRC)

$(BOOT_BIN): $(BOOT_OBJ)
	$(LD) -m elf_i386 -Ttext 0x7c00 --oformat binary -nostdlib -e _start -o $(BOOT_BIN) $(BOOT_OBJ)

$(ARCH)/$(BOOT_DIR)/cd_stage1.o: $(BOOT_DIR)/cd_stage1.S $(BOOT_CFG)
	@mkdir -p $(dir $@)
	$(AS) --32 -I $(BOOT_DIR) -o $@ $<

$(CD_STAGE1): $(ARCH)/$(BOOT_DIR)/cd_stage1.o
# 链接于 0x7C00 (装载线性地址): no-emulation 下 BIOS 把 stage1 装到
# 0x7C00, DS=0 绝对地址即线性地址 (与 bios_boot.S 同一手法)
	$(LD) -m elf_i386 -Ttext 0x7c00 --oformat binary -nostdlib -e _start -o $@ $<
	@sz=$$(stat -f%z $@); [ $$sz -le 2048 ] || { echo "cd_stage1 too large: $$sz"; rm -f $@; exit 1; }


$(OS_BOOT): $(BOOT_BIN) $(OS_IMG)
	@python3 $(BOOT_DIR)/mkboot.py image $(BOOT_BIN) $(OS_IMG) $(OS_BOOT)

$(OS_HDD): $(BOOT_BIN) $(OS_IMG) $(SYS_ROOTFS) $(UEFI_ESP)
	@python3 $(BOOT_DIR)/mkboot.py hdd $(BOOT_BIN) $(OS_IMG) $(SYS_ROOTFS) $(OS_HDD) $(UEFI_ESP)

# ---- UEFI bootloader ----
$(ARCH)/$(UEFI_DIR)/%.c.o: $(UEFI_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UEFI_CFLAGS) -c $< -o $@

$(ARCH)/$(UEFI_DIR)/%.S.o: $(UEFI_DIR)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(UEFI_CFLAGS) -c $< -o $@

$(UEFI_ELF): $(UEFI_OBJS) $(UEFI_DIR)/linker.ld
	$(LD) -nostdlib -pie -T $(UEFI_DIR)/linker.ld -o $@ $(UEFI_OBJS)

$(UEFI_EFI): $(UEFI_ELF) $(UEFI_DIR)/mkpefi.py
	python3 $(UEFI_DIR)/mkpefi.py $< $@ 10

$(UEFI_ESP): $(UEFI_EFI) $(OS_ELF)
	@rm -f $@
	@dd if=/dev/zero of=$@ bs=1048576 count=$(ESP_SIZE_MB) 2>/dev/null
	@mformat -F -i $@ ::
	@mmd -i $@ ::/EFI
	@mmd -i $@ ::/EFI/BOOT
	@mcopy -i $@ $(UEFI_EFI) ::/EFI/BOOT/BOOTX64.EFI
	@mcopy -i $@ $(OS_ELF) ::/KERNEL.ELF
	@mdir -i $@ ::

$(OVMF_FD): $(OVMF)
	@mkdir -p $(UEFI_DIR)
	cp $< $@

$(ISO_ESP): $(UEFI_EFI)
	@rm -f $@
	@dd if=/dev/zero of=$@ bs=1048576 count=$(ISO_ESP_SIZE_MB) 2>/dev/null
	@mformat -i $@ ::
	@mmd -i $@ ::/EFI
	@mmd -i $@ ::/EFI/BOOT
	@mcopy -i $@ $(UEFI_EFI) ::/EFI/BOOT/BOOTX64.EFI
	@mdir -i $@ ::

$(OS_ISO): $(BOOT_DIR)/mkboot.py $(BOOT_BIN) $(CD_STAGE1) $(OS_IMG) $(OS_ELF) $(ISO_ROOTFS) $(ISO_ESP)
	@python3 $(BOOT_DIR)/mkboot.py iso $(BOOT_BIN) $(CD_STAGE1) $(OS_IMG) $(OS_ELF) $(ISO_ROOTFS) $(ISO_ESP) $(OS_ISO)

iso: $(OS_ISO)

asm:
	$(OBJDUMP) -D $(BUILD_DIR)/$(OS_ELF) > $(BUILD_DIR)/kernel.asm

run: $(OS_HDD)
	$(QEMU_CMD) $(QEMU_FLAGS) $(QEMU_ARGS)

run-uefi: $(OS_HDD) $(OVMF_FD)
	$(QEMU_CMD) $(QEMU_FLAGS) $(QEMU_UEFI) $(QEMU_ARGS)

run-q35: $(OS_HDD)
	$(QEMU_CMD) -M q35 $(QEMU_FLAGS) $(QEMU_ARGS_Q35)

run-uefi-q35: $(OS_HDD) $(OVMF_FD)
	$(QEMU_CMD) -M q35 $(QEMU_FLAGS) $(QEMU_UEFI) $(QEMU_ARGS_Q35)

run-xhci: $(OS_HDD)
	$(QEMU_CMD) $(QEMU_FLAGS) $(QEMU_ARGS_XHCI)

run-xhci-uefi: $(OS_HDD) $(OVMF_FD)
	$(QEMU_CMD) $(QEMU_FLAGS) $(QEMU_UEFI) $(QEMU_ARGS_XHCI)

run-kvm:
	@$(MAKE) --no-print-directory run ACCEL=kvm

run-uefi-kvm:
	@$(MAKE) --no-print-directory run-uefi ACCEL=kvm

runasm: $(OS_HDD)
	$(QEMU_CMD) $(QEMU_FLAGS) $(QEMU_ARGS) -d in_asm

# ISO 引导: BIOS 走 El Torito hd 仿真 (仅内核, 无 rootfs), UEFI 走
# EFI 入口 ESP 内 BOOTX64.EFI + 内存盘 rootfs
ISO_QEMU = $(QEMU_FLAGS) -boot d -cdrom $(OS_ISO) -usb \
	-device usb-kbd,port=1,usb_version=1 -device usb-mouse,port=2,usb_version=1

run-iso: $(OS_ISO)
	$(QEMU_CMD) $(ISO_QEMU)

run-iso-uefi: $(OS_ISO) $(OVMF_FD)
	$(QEMU_CMD) $(ISO_QEMU) $(QEMU_UEFI)

debug: $(OS_HDD)
	$(QEMU_CMD) $(QEMU_FLAGS) -gdb tcp::26000 -S $(QEMU_ARGS)

debug-uefi: $(OS_HDD) $(OVMF_FD)
	$(QEMU_CMD) $(QEMU_FLAGS) $(QEMU_UEFI) -gdb tcp::26000 -S $(QEMU_ARGS)

gdb:
	echo "target remote :26000" > /tmp/gdbinit
	x86_64-elf-gdb $(BUILD_DIR)/$(OS_ELF) -x /tmp/gdbinit

clean: clean_boot clean_uefi

clean_boot:
	rm -f $(BOOT_OBJ) $(BOOT_BIN) $(BOOT_CFG) $(OS_BOOT) $(OS_HDD)

clean_uefi:
	rm -rf $(ARCH)/$(UEFI_DIR) $(UEFI_ELF) $(UEFI_EFI) $(UEFI_ESP)

clean_iso:
	rm -f $(OS_ISO) $(ISO_ESP) $(CD_STAGE1) $(CD_STAGE2)
