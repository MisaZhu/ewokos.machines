include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)

LDFLAGS = -L $(TARGET_DIR)/lib

CFLAGS += -I $(SYS_DIR)/basic/libc/libewoksys/ewoksys/include \
	-I $(SYS_DIR)/platform/$(ARCH)/include/$(ARCH) \
	-I $(SYS_DIR)/basic/libs/sd/include \
	-I $(SYS_DIR)/basic/libs/gpio/include \
	-I $(SYS_DIR)/basic/libs/usb/include \
	-Iinclude -I $(TARGET_DIR)/include

# bsp_g2d.c is the public g2d API only: the VideoCore back end it calls
# (vc_g2d/v3d_g2d and the generated QPU kernels) lives in libs/videocore and
# reaches the binaries through -lvideocore in BSP_LFLAGS.
LIB_BSP_OBJS = $(ARCH)/src/bsp_sd.o $(ARCH)/src/bsp_fb.o $(ARCH)/src/bsp_gpio.o $(ARCH)/src/bsp_spi.o \
	$(ARCH)/src/bsp_nvme.o $(ARCH)/src/bsp_usb.o \
	$(ARCH)/src/g2d/bsp_g2d.o

BSP = $(TARGET_DIR)/lib/libbsp.a

$(BSP): $(LIB_BSP_OBJS)
	$(AR) rcs $(BSP) $(LIB_BSP_OBJS) $(BUILD_DIR)/lib/libarch_bcm2712.a
	cp -r include/* $(TARGET_DIR)/include

clean:
	rm -f $(LIB_BSP_OBJS)
	rm -f $(BSP)
