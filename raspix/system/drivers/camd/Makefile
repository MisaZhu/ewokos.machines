include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

CAMD_OBJS = $(ARCH)/camd.o $(ARCH)/ov5647.o

CAMD = $(TARGET_DIR)/drivers/raspix/camd

$(CAMD): $(CAMD_OBJS) \
		$(SDK_DIR)/lib/libewoksys.a \
		$(SDK_DIR)/lib/libarch_bcm283x.a
	$(LD) -Ttext=100 $(CAMD_OBJS) -o $(CAMD) $(LDFLAGS) -larch_bcm283x $(EWOK_LIBC)

clean:
	rm -f $(CAMD_OBJS)
