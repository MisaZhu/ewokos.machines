include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib
CFLAGS += -I $(BUILD_DIR)/include

PCIED_OBJS = $(ARCH)/pcied.o
PCIED = $(TARGET_DIR)/drivers/$(HW)/pcied

$(PCIED): $(PCIED_OBJS) $(SDK_DIR)/lib/libewoksys.a $(SDK_DIR)/lib/libbsp.a
	$(LD) -Ttext=100 $(PCIED_OBJS) -o $(PCIED) $(LDFLAGS) $(BSP_LFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(PCIED_OBJS) $(PCIED)