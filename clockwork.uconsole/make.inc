ifeq ($(ARCH),)
export ARCH=arm
endif

ifeq ($(ARCH),arm)
export ARCH_VER=v7
endif

export BSP_LFLAGS= -lbsp -larch_bcm283x
export HW=raspix

EWOKOS_ROOT_DIR = $(MF_ROOT_DIR)/../..
SYS_ROOT_DIR = $(EWOKOS_ROOT_DIR)/system
SYS_BUILD_DIR = $(SYS_ROOT_DIR)/build/$(HW)

#SDK_DIR= /usr/local/ewokos/$(ARCH)
#ROOTFS_DIR= /usr/local/ewokos/$(ARCH)/$(HW)

SDK_DIR= $(SYS_ROOT_DIR)/build/$(HW)

include $(SYS_ROOT_DIR)/platform/$(ARCH)/make.rule

LDFLAGS = -L$(SDK_DIR)/lib
CFLAGS += -I include -I $(SDK_DIR)/include
CXXFLAGS += -I include -I $(SDK_DIR)/include
