ifeq ($(ARCH),)
export ARCH=aarch64
endif
export ARCH_VER=v8

ifeq ($(ewokos),)
ewokos=~/work/ewokos
endif

export BSP_LFLAGS= -lbsp -larch_bcm2712

ifeq ($(HW),)
export HW=raspi5
endif

EWOKOS_ROOT_DIR = $(ewokos)
SYS_ROOT_DIR = $(EWOKOS_ROOT_DIR)/system
SYS_BUILD_DIR = $(SYS_ROOT_DIR)/build


include $(SYS_ROOT_DIR)/platform/$(ARCH)/make.rule

SDK_DIR= $(SYS_ROOT_DIR)/build/$(HW)
LDFLAGS += -L $(SDK_DIR)/lib
CFLAGS += -I include -I $(SDK_DIR)/include
CXXFLAGS += -I include -I $(SDK_DIR)/include
