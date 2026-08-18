include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

FBDISPLAYD_OBJS = $(ARCH)/fbdisplayd.o 

FBDISPLAYD = $(TARGET_DIR)/drivers/versatilepb/fbdisplayd

$(FBDISPLAYD): $(FBDISPLAYD_OBJS) \
		$(SDK_DIR)/lib/libewoksys.a \
		$(SDK_DIR)/lib/libdisplay.a \
		$(SDK_DIR)/lib/libgraph.a \
		$(SDK_DIR)/lib/libfont.a \
		$(SDK_DIR)/lib/libdisplayman.a \
		$(SDK_DIR)/lib/libgterminal.a \
		$(SDK_DIR)/lib/libtextgrid.a \
		$(SDK_DIR)/lib/libdisplayd.a \
		$(SDK_DIR)/lib/libtinyjson.a
	$(LD) -Ttext=100 $(FBDISPLAYD_OBJS) -o $(FBDISPLAYD) $(LDFLAGS) -lgterminal -ltextgrid -ldisplayd -ltinyjson $(EWOK_LIB_GRAPH) $(EWOK_LIBC)

clean:
	rm -f $(FBDISPLAYD_OBJS)
