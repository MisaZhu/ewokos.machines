include make.inc

# default to parallel build; sub-makes share the jobserver via $(MAKE)
ifeq ($(MAKELEVEL),0)
ifeq ($(filter -j%,$(MAKEFLAGS)),)
NPROCS := $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
MAKEFLAGS += -j$(NPROCS)
endif
endif

EWOKOS_ROOT_DIR = $(ewokos)
INST_DIR = /usr/local/ewokos
BUILD_DIR = $(SYS_ROOT_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

# Q8B's normal rootfs is the complete graphical system, matching raspi5:
# build the framebuffer stack and the xwin/window-manager stack by default.
# The explicit `gui` target remains available for a console-only image.
all: x
	@echo "all done."

include $(SYS_ROOT_DIR)/platform/make.inc

basic: basic_sys
	@$(MAKE) final

network: network_sys
	@$(MAKE) final

gui: gui_sys
	@$(MAKE) final

x: x_sys
	@$(MAKE) final

# Platform headers consumed by user-space drivers (GOP framebuffer and
# RAM-disk hand-off layout, plus the Adreno 690 GPU physical-address map).
platform_headers:
	@mkdir -p $(BUILD_DIR)/include
	@cp -p ../include/q8b_platform.h $(BUILD_DIR)/include/
	@cp -p ../include/q8b_adreno.h $(BUILD_DIR)/include/

# Adreno 690 firmware (GMU / SQE / board-selected signed zap). Redistributable
# Qualcomm blobs, see ../firmware/adreno/README.md.  Installed into the
# rootfs as /etc/firmware/adreno/ for libadreno's vfs_readfile() loader.
ADRENO_FW_SRC = $(MAKEINC_DIR)/../firmware/adreno
firmware_install:
	@if [ ! -f "$(ADRENO_FW_SRC)/a660_gmu.bin" ]; then \
		echo "ERROR: $(ADRENO_FW_SRC)/a660_gmu.bin missing"; \
		echo "       see machines/q8b/firmware/adreno/README.md"; \
		exit 1; \
	fi
	@mkdir -p $(TARGET_DIR)/etc/firmware/adreno
	@cp -p $(ADRENO_FW_SRC)/a660_gmu.bin $(ADRENO_FW_SRC)/a660_sqe.fw \
		$(TARGET_DIR)/etc/firmware/adreno/
	@cp -p $(ADRENO_FW_SRC)/LICENSE.qcom $(TARGET_DIR)/etc/firmware/adreno/
	@mkdir -p $(TARGET_DIR)/etc/firmware/adreno/sc8280xp/LENOVO/21BX
	@cp -p $(ADRENO_FW_SRC)/sc8280xp/LENOVO/21BX/qcdxkmsuc8280.mbn \
		$(TARGET_DIR)/etc/firmware/adreno/sc8280xp/LENOVO/21BX/

basic_sys: kernel_heads ewok_heads platform_headers firmware_install
	rm -f $(BUILD_DIR)/lib/libbsp.a
	mkdir -p $(TARGET_DIR)/drivers/$(HW)
	$(MAKE) -C libs
	$(MAKE) -C $(SYS_ROOT_DIR)/basic
	$(MAKE) -C drivers basic
	@mkdir -p ${TARGET_DIR}/etc
	@cp -Rp etc/basic/. ${TARGET_DIR}/etc/

network_sys: basic_sys
	$(MAKE) -C $(SYS_ROOT_DIR)/network

# Q8B uses the SC8280XP multi-port DWC3 core through the shared polled xHCI
# stack.  g2dd is the shared 2D/3D offload daemon; on q8b it links libbsp +
# libadreno (BSP_LFLAGS) and forwards into the Adreno 690 A2D/CS back ends,
# enabled by the shared /etc/g2d.rd before any display/desktop clients start.
# Keep Bluetooth out until its board-specific transport lands.
GUI_DRIVERS = displaymand consoled fontd g2dd usbhostd hid_keybd hid_moused \
	usbfat32fsd

gui_sys: basic_sys
	$(MAKE) -C $(SYS_ROOT_DIR)/gui mkdirs
	$(MAKE) -C $(SYS_ROOT_DIR)/gui/libs
	$(MAKE) -C $(SYS_ROOT_DIR)/gui/bin
	$(MAKE) -C $(SYS_ROOT_DIR)/gui/sbin
	for d in $(GUI_DRIVERS); do $(MAKE) -C $(SYS_ROOT_DIR)/gui/drivers/$$d || exit 1; done
	$(MAKE) -C drivers gui
	@mkdir -p ${TARGET_DIR}/etc
	@cp -Rp etc/gui/. ${TARGET_DIR}/etc/

x_sys: gui_sys network_sys
	$(MAKE) -C $(SYS_ROOT_DIR)/xwin
	@mkdir -p ${TARGET_DIR}/etc
	@cp -Rp etc/xwin/. ${TARGET_DIR}/etc/

CLEAN_DIRS = libs drivers $(SYS_ROOT_DIR)/gui $(SYS_ROOT_DIR)/basic \
	$(SYS_ROOT_DIR)/network $(SYS_ROOT_DIR)/xwin
clean:
	@for dir in $(CLEAN_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
	rm -fr $(BUILD_DIR)
	rm -fr $(FINAL_DIR)

ifeq ($(FS),)
FS=ext3
endif

# The RAM-disk rootfs is loaded by the UEFI stub over TFTP/ESP.  112MB
# fits the complete X userland while keeping PXE transfer time below the
# firmware timeout seen with the 128MB image.
ifeq ($(ROOTFS_MB),)
ROOTFS_MB=112
endif

SD = ../root_$(ARCH).img
sd:
	@if [ -z "$$(ls -A $(FINAL_DIR) 2>/dev/null)" ]; then \
		echo "ERROR: $(FINAL_DIR) is empty -- run 'make x', 'make gui' or 'make basic' first"; \
		exit 1; \
	fi
	@echo "\n==== building ${FS} rootfs image ($(ROOTFS_MB) MB) ===="
	dd if=/dev/zero of=${SD} bs=1k count=$(shell expr $(ROOTFS_MB) \* 1024)
	mke2fs -t ${FS} -b 4096 -I 128 ${SD}
	cd $(FINAL_DIR); \
	find . -type f | e2cp -ap -G0 -O0 -d ../${SD}:/
	@echo "==== ${FS} rootfs image created (${SD}) ===="

.PHONY: all basic network gui x basic_sys network_sys gui_sys x_sys platform_headers firmware_install clean sd
