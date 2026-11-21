include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib
CFLAGS += -I $(BUILD_DIR)/include

OBJS = $(ARCH)/sdprobe.o
BIN = $(TARGET_DIR)/drivers/$(HW)/sdprobe

$(BIN): $(OBJS) $(SDK_DIR)/lib/libewoksys.a $(SDK_DIR)/lib/libbsp.a
	$(LD) -Ttext=100 $(OBJS) -o $(BIN) $(LDFLAGS) $(BSP_LFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(OBJS) $(BIN)