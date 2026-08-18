include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

DSI_FBDISPLAYD_OBJS = $(ARCH)/dsi_fbdisplayd.o

DSI_FBDISPLAYD = $(TARGET_DIR)/drivers/raspi5/dsi_fbdisplayd

$(DSI_FBDISPLAYD): $(DSI_FBDISPLAYD_OBJS) \
		$(SDK_DIR)/lib/libewoksys.a \
		$(SDK_DIR)/lib/libdisplay.a \
		$(SDK_DIR)/lib/libgraph.a \
		$(SDK_DIR)/lib/libfont.a \
		$(SDK_DIR)/lib/libdisplayman.a \
		$(SDK_DIR)/lib/libdisplayd.a \
		$(SDK_DIR)/lib/libtinyjson.a \
		$(SDK_DIR)/lib/libarch_bcm2712.a \
		$(SDK_DIR)/lib/libbsp.a
	$(LD) -Ttext=100 $(DSI_FBDISPLAYD_OBJS) -o $(DSI_FBDISPLAYD) $(LDFLAGS) -ldisplayd $(EWOK_LIB_GRAPH) $(BSP_LFLAGS) -ltinyjson $(EWOK_LIBC)

clean:
	rm -f $(DSI_FBDISPLAYD_OBJS)
