ifeq ($(ARCH),)
export ARCH=arm
endif

export BSP_LFLAGS= -larch_bcm283x
export HW=raspix

SDK_DIR= /usr/local/ewokos/$(ARCH)
ROOTFS_DIR= /usr/local/ewokos/$(ARCH)/$(HW)

include $(SDK_DIR)/make.rule

LDFLAGS = -L$(ROOTFS_DIR)/lib
CFLAGS += -I include -I $(ROOTFS_DIR)/include
CXXFLAGS += -I include -I $(ROOTFS_DIR)/include

EWOK_LIBC = -lewoksys -lc -lgloss -lsoftfloat
