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
MACHINE_BUILD_DIR = build
TARGET_DIR = $(BUILD_DIR)/rootfs

EXTRA_APPS_DIR = $(EWOKOS_ROOT_DIR)/extra.apps

export BSP_LFLAGS = -lbsp

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

# 平台头文件暂存: x86_platform.h (sys_info_t.platform_data 布局) 供
# 用户态驱动解析, 与 kernel_heads 暂存的共享 sysinfo.h 配对使用
platform_headers:
	mkdir -p $(BUILD_DIR)/include
	cp ../include/x86_platform.h $(BUILD_DIR)/include/

basic_sys: kernel_heads ewok_heads platform_headers
	rm -f $(BUILD_DIR)/lib/libbsp.a
	mkdir -p $(TARGET_DIR)/drivers/$(HW)
	$(MAKE) -C libs
	$(MAKE) -C $(SYS_ROOT_DIR)/basic
	$(MAKE) -C drivers basic
	@mkdir -p ${TARGET_DIR}/etc
	@cp -Rp etc/basic/. ${TARGET_DIR}/etc/

gui_sys: basic_sys
	$(MAKE) -C $(SYS_ROOT_DIR)/gui
	$(MAKE) -C drivers gui
	@mkdir -p ${TARGET_DIR}/etc
	@cp -Rp etc/gui/. ${TARGET_DIR}/etc/

network_sys: gui_sys
	$(MAKE) -C $(SYS_ROOT_DIR)/network
	$(MAKE) -C drivers network
	@mkdir -p ${TARGET_DIR}/etc
	@cp -Rp etc/network/. ${TARGET_DIR}/etc/

x_sys: network_sys
	$(MAKE) -C $(SYS_ROOT_DIR)/xwin
	@mkdir -p ${TARGET_DIR}/etc
	@cp -Rp etc/xwin/. ${TARGET_DIR}/etc/

CLEAN_DIRS = libs drivers $(SYS_ROOT_DIR)/xwin $(SYS_ROOT_DIR)/gui $(SYS_ROOT_DIR)/network $(SYS_ROOT_DIR)/basic
clean:
	@for dir in $(CLEAN_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
	$(MAKE) -C $(EWOKOS_ROOT_DIR)/sw.extra HW=$(HW) clean
	rm -fr $(BUILD_DIR)
	rm -fr $(FINAL_DIR)

ifeq ($(FS),)
FS=ext3
endif

# rootfs 镜像大小 (MB): 单一镜像同时服务 hdd 引导 (内核直读, 可持久化)
# 与 ISO/UEFI 引导 (整个拷入内存盘, 128MB 在 512MB RAM 下实证可跑完整
# GUI 栈)。128 上限 = 内存盘路径的 RAM 预算约束。
ifeq ($(ROOTFS_MB),)
ROOTFS_MB=128
endif

SD = root_$(ARCH).img
# 成像自 staging (FINAL_DIR)。staging 为空时尝试跑 final 从
# $(BUILD_DIR)/rootfs 同步 (final 会消费该目录); 两者皆无则明确报错 ——
# 首次构建请先 `make all`。
sd:
	@if [ -z "$$(ls -A $(FINAL_DIR) 2>/dev/null)" ]; then \
		if [ ! -d "$(BUILD_DIR)/rootfs" ]; then \
			echo "ERROR: no rootfs staging ('$(FINAL_DIR)' empty and '$(BUILD_DIR)/rootfs' missing) -- run 'make all' first"; \
			exit 1; \
		fi; \
		$(MAKE) final; \
	fi
	@if [ -z "$$(ls -A $(FINAL_DIR) 2>/dev/null)" ]; then \
		echo "ERROR: rootfs staging '$(FINAL_DIR)' is empty -- run 'make all' first"; \
		exit 1; \
	fi
	@echo "\n====building ${FS} format sdcard image===="
	dd if=/dev/zero of=${SD} bs=1k count=$(shell expr ${ROOTFS_MB} \* 1024)
#make ext2/ext3 fs with: block_size=4096 inode_size=128
	mke2fs -t ${FS} -b 4096 -I 128 ${SD}

	cd $(FINAL_DIR); \
	find . -type f | e2cp -ap -G0 -O0 -d ../${SD}:/

	@echo "==== ${FS} format sdcard image created  ====\n"

# ISO 与 hdd 共用同一 rootfs 镜像 (root_x86.img); sd-iso 保留为兼容别名
SD_ISO = root_$(ARCH).img
sd-iso:
	@$(MAKE) --no-print-directory sd SD=${SD_ISO} ROOTFS_MB=128