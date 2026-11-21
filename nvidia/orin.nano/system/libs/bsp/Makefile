include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)

LDFLAGS = -L $(TARGET_DIR)/lib

# libsd headers come from the source tree: bsp builds in the libs phase,
# before basic installs the sd library headers.
CFLAGS += -I $(SYS_DIR)/basic/libc/libewoksys/ewoksys/include \
	-I $(SYS_DIR)/basic/libs/sd/include \
	-I $(MAKEINC_DIR)/../include \
	-I include -I $(TARGET_DIR)/include

LIB_BSP_OBJS = $(ARCH)/src/bsp_sd.o $(ARCH)/src/bsp_bootfs.o \
	$(ARCH)/src/bsp_gpio.o $(ARCH)/src/bsp_pcie.o $(ARCH)/src/bsp_eth.o \
	$(ARCH)/src/bsp_bpmp.o $(ARCH)/src/bsp_i2c.o $(ARCH)/src/bsp_sdmmc.o \
	$(ARCH)/src/bsp_xusb.o $(ARCH)/src/bsp_wdt.o

BSP = $(TARGET_DIR)/lib/libbsp.a

$(BSP): $(LIB_BSP_OBJS)
	$(AR) rcs $(BSP) $(LIB_BSP_OBJS)
	mkdir -p $(TARGET_DIR)/include
	cp -r include/* $(TARGET_DIR)/include

clean:
	rm -f $(LIB_BSP_OBJS)
	rm -f $(BSP)

.PHONY: clean