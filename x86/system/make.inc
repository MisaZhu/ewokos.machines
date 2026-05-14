export HW=x86
export ARCH=x86
export ARCH_VER=x64

ifeq ($(ewokos),)
ewokos=~/work/ewokos
endif

export BSP_LFLAGS= -lbsp -larch_x86
