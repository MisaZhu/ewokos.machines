include make.inc

# default to parallel build; sub-makes share the jobserver via $(MAKE)
ifeq ($(MAKELEVEL),0)
ifeq ($(filter -j%,$(MAKEFLAGS)),)
NPROCS := $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
MAKEFLAGS += -j$(NPROCS)
endif
endif

EWOKOS_ROOT_DIR = $(ewokos)
SYS_ROOT_DIR = ${EWOKOS_ROOT_DIR}/system
INST_DIR = /usr/local/ewokos
BUILD_DIR = $(SYS_ROOT_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

all: x_sys
	@echo "all done."


include $(SYS_ROOT_DIR)/platform/make.inc

DIRS = libs $(SYS_ROOT_DIR)/basic $(SYS_ROOT_DIR)/gui $(SYS_ROOT_DIR)/network drivers $(SYS_ROOT_DIR)/xwin

# parallel phases with the same ordering the old serial loop guaranteed
prep:
	rm -f $(BUILD_DIR)/lib/libbsp.a
	mkdir -p $(TARGET_DIR)/drivers/$(HW)

libs: prep kernel_heads ewok_heads
	@$(MAKE) -C libs
$(SYS_ROOT_DIR)/basic: libs
	@$(MAKE) -C $(SYS_ROOT_DIR)/basic
$(SYS_ROOT_DIR)/gui: $(SYS_ROOT_DIR)/basic
	@$(MAKE) -C $(SYS_ROOT_DIR)/gui
$(SYS_ROOT_DIR)/network: $(SYS_ROOT_DIR)/gui
	@$(MAKE) -C $(SYS_ROOT_DIR)/network
drivers: $(SYS_ROOT_DIR)/gui
	@$(MAKE) -C drivers
$(SYS_ROOT_DIR)/xwin: $(SYS_ROOT_DIR)/gui
	@$(MAKE) -C $(SYS_ROOT_DIR)/xwin

x_sys: $(DIRS)
	@mkdir -p ${TARGET_DIR}/etc
	@cp -Rp etc/. ${TARGET_DIR}/etc/
	@$(MAKE) final

.PHONY: x_sys prep libs drivers $(DIRS)

CLEAN_DIRS = libs drivers $(SYS_ROOT_DIR)/xwin $(SYS_ROOT_DIR)/gui $(SYS_ROOT_DIR)/network $(SYS_ROOT_DIR)/basic
clean:
	@for dir in $(CLEAN_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
	$(MAKE) -C $(EWOKOS_ROOT_DIR)/sw.extra HW=$(HW) clean
	rm -fr $(BUILD_DIR)
	rm -fr $(FINAL_DIR)
