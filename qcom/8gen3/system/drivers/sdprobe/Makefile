include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib
CFLAGS += -I $(BUILD_DIR)/include

SDPROBE_OBJS = $(ARCH)/sdprobe.o
SDPROBE = $(TARGET_DIR)/bin/sdprobe

$(SDPROBE): $(SDPROBE_OBJS) $(SDK_DIR)/lib/libewoksys.a \
		$(SDK_DIR)/lib/libbsp.a $(SDK_DIR)/lib/libsd.a
	$(LD) -Ttext=100 $(SDPROBE_OBJS) -o $(SDPROBE) $(LDFLAGS) -lbsp -lsd $(EWOK_LIBC)

clean:
	rm -f $(SDPROBE_OBJS) $(SDPROBE)
