include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

KLOGD_OBJS = $(ARCH)/klogd.o

KLOGD = $(TARGET_DIR)/drivers/x86/klogd

$(KLOGD): $(KLOGD_OBJS) \
		$(SDK_DIR)/lib/libewoksys.a
	$(LD) -Ttext=100 $(KLOGD_OBJS) -o $(KLOGD) $(LDFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(KLOGD_OBJS) $(KLOGD)
