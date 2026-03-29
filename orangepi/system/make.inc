export HW=orangepi
export ARCH=arm
export ARCH_VER=v7

ifeq ($(ewokos),)
ewokos=~/work/ewokos
endif

export BSP_LFLAGS= -lbsp -larch_orangepi
