include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

DSI_FBD_OBJS = dsi_fbd.o

DSI_FBD = $(TARGET_DIR)/drivers/raspix/dsi_fbd

$(DSI_FBD): $(DSI_FBD_OBJS) \
		$(EWOK_LIBC_A) \
		$(BUILD_DIR)/lib/libgraph.a \
		$(BUILD_DIR)/lib/libfbd.a \
		$(BUILD_DIR)/lib/libgraph.a \
		$(BUILD_DIR)/lib/libtinyjson.a \
		$(BUILD_DIR)/lib/libarch_bcm283x.a
	$(LD) -Ttext=100 $(DSI_FBD_OBJS) -o $(DSI_FBD) $(LDFLAGS) -lfbd $(EWOK_LIB_GRAPH) $(BSP_LFLAGS) -ltinyjson $(EWOK_LIBC)

clean:
	rm -f $(DSI_FBD_OBJS)
