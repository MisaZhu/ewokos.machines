ifeq ($(ARCH),)
export ARCH=arm
endif

export BSP_LFLAGS= -larch_bcm283x
export HW=raspix


#SDK_DIR= /usr/local/ewokos/$(ARCH)
#ROOTFS_DIR= /usr/local/ewokos/$(ARCH)/$(HW)

SDK_DIR= $(SYS_ROOT_DIR)/build

include $(SYS_ROOT_DIR)/platform/$(ARCH)/make.rule

LDFLAGS = -L$(SDK_DIR)/lib
CFLAGS += -I include -I $(SDK_DIR)/include
CXXFLAGS += -I include -I $(SDK_DIR)/include

EWOK_LIBC = -lewoksys -lc -lgloss -lsoftfloat
