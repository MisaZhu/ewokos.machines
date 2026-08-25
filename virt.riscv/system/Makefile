export HW=virt
export ARCH=riscv
#export ARCH_VER=v7

EWOKOS_ROOT_DIR = ../../..
SYS_ROOT_DIR = ${EWOKOS_ROOT_DIR}/system
INST_DIR = /usr/local/ewokos
BUILD_DIR = $(SYS_ROOT_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

export BSP_LFLAGS= -lbsp -larch_virt

all: x_sys
	@echo "all done."

include $(SYS_ROOT_DIR)/platform/riscv/make.inc

DIRS = libs $(SYS_ROOT_DIR)/basic $(SYS_ROOT_DIR)/gui drivers $(SYS_ROOT_DIR)/xwin
x_sys: kernel_heads 
	rm -f $(BUILD_DIR)/lib/libbsp.a

	mkdir -p $(TARGET_DIR)/drivers/$(HW)
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done
	cp -r etc $(TARGET_DIR)/
	@$(MAKE) final

CLEAN_DIRS = libs drivers $(SYS_ROOT_DIR)/xwin $(SYS_ROOT_DIR)/gui $(SYS_ROOT_DIR)/basic
clean:
	@for dir in $(CLEAN_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
	rm -fr $(BUILD_DIR)
	rm -fr $(FINAL_DIR)
