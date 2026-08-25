include ../../make.inc
SYS_DIR=$(ewokos)/system/
include $(SYS_DIR)/platform/arm/make.rule

BUILD_DIR = $(SYS_DIR)/build/$(HW)
TARGET_DIR = $(BUILD_DIR)

LDFLAGS = -L $(TARGET_DIR)/lib

CFLAGS += -I $(SYS_DIR)/basic/libc/libewoksys/ewoksys/include \
	-I $(SYS_DIR)/basic/libs/sd/include \
	-I $(SYS_DIR)/gui/libs/graph/include \
	-I $(SYS_DIR)/basic/libs/usb/include \
	-Iinclude -I $(TARGET_DIR)/include

ifeq ($(G_2D_BOOST),yes)
CFLAGS += -mfpu=neon-vfpv4 -DGRAPH_2D_BOOST
CXXFLAGS += -mfpu=neon-vfpv4 -DGRAPH_2D_BOOST
endif

LIB_BSP_OBJS = $(ARCH)/src/bsp_sd.o $(ARCH)/src/bsp_fb.o $(ARCH)/src/bsp_usb.o

BSP = $(TARGET_DIR)/lib/libbsp.a 

$(BSP): $(LIB_BSP_OBJS) \
		$(BUILD_DIR)/lib/libarch_orangepi.a
	$(AR) rcs $(BSP) $(LIB_BSP_OBJS)  $(BUILD_DIR)/lib/libarch_orangepi.a
	cp -r include/* $(TARGET_DIR)/include

clean:
	rm -f $(LIB_BSP_OBJS)
	rm -f $(BSP)

