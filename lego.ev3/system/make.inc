export HW=ev3
export ARCH=arm
export ARCH_VER=v5

ifeq ($(ewokos),)
ewokos=~/work/ewokos
endif

export BSP_LFLAGS= -lbsp -larch_ev3
