include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)

LDFLAGS = -L $(TARGET_DIR)/lib

# libsd headers come from the source tree: bsp builds in the libs phase,
# before basic installs the sd library headers.  Same for the shared USB
# contract headers (gui/libs/usb), which the gui phase installs later.
# g2dclient/g2d_arch headers likewise come from the source tree: the g2d
# backend (bsp_g2d.c) builds here, before the gui phase installs them.
CFLAGS += -I $(SYS_DIR)/basic/libc/libewoksys/ewoksys/include \
	-I $(SYS_DIR)/basic/libs/sd/include \
	-I $(SYS_DIR)/gui/libs/usb/include \
	-I $(SYS_DIR)/gui/libs/hid/include \
	-I $(SYS_DIR)/gui/libs/g2dclient/include \
	-I $(SYS_DIR)/platform/$(ARCH)/include/$(ARCH) \
	-I $(MAKEINC_DIR)/../include \
	-I include -I $(TARGET_DIR)/include

LIB_BSP_OBJS = $(ARCH)/src/bsp_sd.o $(ARCH)/src/bsp_bootfs.o \
	$(ARCH)/src/bsp_gpio.o $(ARCH)/src/bsp_pcie.o $(ARCH)/src/bsp_eth.o \
	$(ARCH)/src/bsp_bpmp.o $(ARCH)/src/bsp_i2c.o $(ARCH)/src/bsp_sdmmc.o \
	$(ARCH)/src/bsp_xusb.o $(ARCH)/src/xhci.o $(ARCH)/src/bsp_usb.o \
	$(ARCH)/src/bsp_uvc.o \
	$(ARCH)/src/bsp_wdt.o $(ARCH)/src/bsp_fb.o $(ARCH)/src/bsp_vic.o \
	$(ARCH)/src/bsp_g2d.o $(ARCH)/src/bsp_g3d.o $(ARCH)/src/bsp_cl.o

BSP = $(TARGET_DIR)/lib/libbsp.a

$(BSP): $(LIB_BSP_OBJS)
	$(AR) rcs $(BSP) $(LIB_BSP_OBJS)
	mkdir -p $(TARGET_DIR)/include
	cp -r include/* $(TARGET_DIR)/include

clean:
	rm -f $(LIB_BSP_OBJS)
	rm -f $(BSP)

.PHONY: clean