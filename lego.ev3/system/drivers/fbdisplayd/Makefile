include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

FBDISPLAYD_OBJS = $(ARCH)/fbdisplayd.o $(ARCH)/st7586.o

FBDISPLAYD = $(TARGET_DIR)/drivers/ev3/fbdisplayd

$(FBDISPLAYD): $(FBDISPLAYD_OBJS) \
		$(SDK_DIR)/lib/libewoksys.a \
		$(SDK_DIR)/lib/libdisplay.a \
		$(SDK_DIR)/lib/libgraph.a \
		$(SDK_DIR)/lib/libfont.a \
		$(SDK_DIR)/lib/libdisplayman.a \
		$(SDK_DIR)/lib/libdisplayd.a \
		$(SDK_DIR)/lib/libtinyjson.a \
		$(SDK_DIR)/lib/libbsp.a
	$(LD) -Ttext=100 $(FBDISPLAYD_OBJS) -o $(FBDISPLAYD) $(LDFLAGS) -larch_ev3 -ldisplayd $(EWOK_LIB_GRAPH) $(BSP_LFLAGS) -ltinyjson $(EWOK_LIBC)

clean:
	rm -f $(FBDISPLAYD_OBJS)
