export HW=x86
export ARCH=x86
export ARCH_VER=x64

# Compute absolute path relative to this make.inc file location
MAKEINC_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
ewokos := $(shell cd $(MAKEINC_DIR)/../../.. && pwd)

SYS_ROOT_DIR = ${ewokos}/system

SDK_DIR= $(SYS_ROOT_DIR)/build_$(ARCH)/$(HW)
LDFLAGS += -L $(SDK_DIR)/lib
CFLAGS += -I include -I $(SDK_DIR)/include
CXXFLAGS += -I include -I $(SDK_DIR)/include

export BSP_LFLAGS= -lbsp
