include config.mk

LDS_QEMU = mkos.lds.qemu.S
LDS = mkos.lds.S

OS_IMG = kernel7.img
OS_ELF = kernel7.elf
OS_QEMU_IMG = kernel7.qemu.img
OS_QEMU_ELF = kernel7.qemu.elf

EWOKOS_ROOT_DIR = ../../..

include make.bsp
include $(EWOKOS_ROOT_DIR)/kernel/platform/arm/make.rule
