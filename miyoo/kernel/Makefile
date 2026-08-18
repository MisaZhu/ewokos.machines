include config.mk

LDS = mkos.lds.S

OS_IMG = kernel7.img
OS_ELF = kernel7.elf

ifeq ($(ewokos),)
ewokos=~/work/ewokos
endif
EWOKOS_ROOT_DIR = $(ewokos)

include make.bsp
include $(EWOKOS_ROOT_DIR)/kernel/platform/arm/make.rule
