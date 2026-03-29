ifeq ($(ARCH),)
export ARCH=arm
endif

ifeq ($(ARCH),arm)
export ARCH_VER=v7
endif

ifeq ($(ewokos),)
ewokos = ~/work/ewokos
endif

export BSP_LFLAGS= -lbsp -larch_bcm283x
export HW=raspix

EWOKOS_ROOT_DIR = $(ewokos)
SYS_ROOT_DIR = $(EWOKOS_ROOT_DIR)/system
SYS_BUILD_DIR = $(SYS_ROOT_DIR)/build/$(HW)

SDK_DIR= $(SYS_BUILD_DIR)

include $(SYS_ROOT_DIR)/platform/$(ARCH)/make.rule

LDFLAGS = -L $(SDK_DIR)/lib
CFLAGS += -I include -I $(SDK_DIR)/include
CXXFLAGS += -I include -I $(SDK_DIR)/include
