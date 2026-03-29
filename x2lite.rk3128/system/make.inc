export HW=rk3128
export ARCH=arm
export ARCH_VER=v7

ifeq ($(ewokos),)
ewokos=~/work/ewokos
endif

export BSP_LFLAGS= -lbsp -larch_rk3128
