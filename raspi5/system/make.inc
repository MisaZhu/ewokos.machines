export HW=raspi5
export ARCH=aarch64
export ARCH_VER=v8

ifeq ($(ewokos),)
ewokos=~/work/ewokos
endif

SYS_ROOT_DIR = ${ewokos}/system

export BSP_LFLAGS= -lbsp -larch_bcm2712

SDK_DIR= $(SYS_ROOT_DIR)/build/$(HW)
LDFLAGS += -L $(SDK_DIR)/lib
CFLAGS += -I include -I $(SDK_DIR)/include
CXXFLAGS += -I include -I $(SDK_DIR)/include