include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

MOUSED_OBJS = moused.o

MOUSED = $(TARGET_DIR)/drivers/x86/moused

$(MOUSED): $(MOUSED_OBJS) \
		$(BUILD_DIR)/lib/libewoksys.a
	$(LD) -Ttext=100 $(MOUSED_OBJS) -o $(MOUSED) $(LDFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(MOUSED_OBJS) $(MOUSED)
