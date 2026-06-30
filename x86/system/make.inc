export HW=x86
export ARCH=x86
export ARCH_VER=x64

# Compute absolute path relative to this make.inc file location
MAKEINC_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
ewokos := $(shell cd $(MAKEINC_DIR)/../../.. && pwd)

export BSP_LFLAGS= -lbsp
