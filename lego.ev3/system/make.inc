export HW=ev3
export ARCH=arm
export ARCH_VER=v5

ifeq ($(ewokos),)
ewokos=~/work/ewokos
endif

SYS_ROOT_DIR = ${ewokos}/system

SDK_DIR= $(SYS_ROOT_DIR)/build_$(ARCH)/$(HW)
LDFLAGS += -L $(SDK_DIR)/lib
CFLAGS += -I include -I $(SDK_DIR)/include
CXXFLAGS += -I include -I $(SDK_DIR)/include

export BSP_LFLAGS= -lbsp -larch_ev3
