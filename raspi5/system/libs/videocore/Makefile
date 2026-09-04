include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)

LDFLAGS = -L $(TARGET_DIR)/lib

CFLAGS += -I $(SYS_DIR)/basic/libc/libewoksys/ewoksys/include \
	-I $(SYS_DIR)/platform/$(ARCH)/include/$(ARCH) \
	-Iinclude -I $(TARGET_DIR)/include

LIB_VIDEOCORE_OBJS = $(ARCH)/src/vc_g2d.o $(ARCH)/src/v3d_g2d.o

VIDEOCORE = $(TARGET_DIR)/lib/libvideocore.a

$(VIDEOCORE): $(LIB_VIDEOCORE_OBJS)
	$(AR) rcs $(VIDEOCORE) $(LIB_VIDEOCORE_OBJS)
	cp -r include/* $(TARGET_DIR)/include

clean:
	rm -f $(LIB_VIDEOCORE_OBJS)
	rm -f $(VIDEOCORE)
