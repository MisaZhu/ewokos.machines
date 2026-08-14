include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

DSI_FBDISPLAYD_OBJS = $(ARCH)/dsi_fbdisplayd.o

DSI_FBDISPLAYD = $(TARGET_DIR)/drivers/raspix/dsi_fbdisplayd

$(DSI_FBDISPLAYD): $(DSI_FBDISPLAYD_OBJS) \
		$(EWOK_LIBC_A) \
		$(BUILD_DIR)/lib/libgraph.a \
		$(BUILD_DIR)/lib/libdisplayd.a \
		$(BUILD_DIR)/lib/libgraph.a \
		$(BUILD_DIR)/lib/libtinyjson.a \
		$(BUILD_DIR)/lib/libarch_bcm283x.a
	$(LD) -Ttext=100 $(DSI_FBDISPLAYD_OBJS) -o $(DSI_FBDISPLAYD) $(LDFLAGS) -ldisplayd $(EWOK_LIB_GRAPH) $(BSP_LFLAGS) -ltinyjson $(EWOK_LIBC)

clean:
	rm -f $(DSI_FBDISPLAYD_OBJS)
