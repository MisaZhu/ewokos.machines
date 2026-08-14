include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

FBDISPLAYD_OBJS = $(ARCH)/fbdisplayd.o $(ARCH)/st7586.o

FBDISPLAYD = $(TARGET_DIR)/drivers/ev3/fbdisplayd

$(FBDISPLAYD): $(FBDISPLAYD_OBJS) \
		$(EWOK_LIBC_A)  \
		$(BUILD_DIR)/lib/libgraph.a \
		$(BUILD_DIR)/lib/libdisplayd.a \
		$(BUILD_DIR)/lib/libgraph.a \
		$(BUILD_DIR)/lib/libtinyjson.a \
		$(BUILD_DIR)/lib/libarch_ev3.a
	$(LD) -Ttext=100 $(FBDISPLAYD_OBJS) -o $(FBDISPLAYD) $(LDFLAGS) -larch_ev3 -ldisplayd $(EWOK_LIB_GRAPH) $(BSP_LFLAGS) -ltinyjson $(EWOK_LIBC)

clean:
	rm -f $(FBDISPLAYD_OBJS)
