include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

NET_OBJS = $(ARCH)/net.o

NET = $(TARGET_DIR)/drivers/x86/net

$(NET): $(NET_OBJS) \
                $(SDK_DIR)/lib/libewoksys.a \
                $(SDK_DIR)/lib/libbsp.a
	$(LD) -Ttext=100 $(NET_OBJS) -o $(NET) $(LDFLAGS) $(BSP_LFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(NET_OBJS) $(NET)
