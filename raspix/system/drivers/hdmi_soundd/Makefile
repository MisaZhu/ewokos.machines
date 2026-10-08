include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

HDMI_SOUNDD_OBJS = $(ARCH)/hdmi_soundd.o

HDMI_SOUNDD = $(TARGET_DIR)/drivers/raspix/hdmi_soundd

$(HDMI_SOUNDD): $(HDMI_SOUNDD_OBJS) \
		$(SDK_DIR)/lib/libewoksys.a \
		$(SDK_DIR)/lib/libarch_bcm283x.a
	$(LD) -Ttext=100 $(HDMI_SOUNDD_OBJS) -o $(HDMI_SOUNDD) $(LDFLAGS) -larch_bcm283x $(EWOK_LIBC)

clean:
	rm -f $(HDMI_SOUNDD_OBJS)
