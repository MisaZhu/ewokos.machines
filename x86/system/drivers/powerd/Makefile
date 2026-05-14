include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

PMU_OBJS = powerd.o

PMU = $(TARGET_DIR)/drivers/x86/powerd

$(PMU): $(PMU_OBJS) \
		$(BUILD_DIR)/lib/libewoksys.a
	$(LD) -Ttext=100 $(PMU_OBJS) -o $(PMU) $(LDFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(PMU_OBJS) $(PMU)
