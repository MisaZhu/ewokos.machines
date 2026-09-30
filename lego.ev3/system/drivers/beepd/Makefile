include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

BEEPD_OBJS = $(ARCH)/beepd.o

BEEPD = $(TARGET_DIR)/drivers/ev3/beepd

$(BEEPD): $(BEEPD_OBJS) \
		$(SDK_DIR)/lib/libewoksys.a \
		$(SDK_DIR)/lib/libbsp.a
	$(LD) -Ttext=100 $(BEEPD_OBJS) -o $(BEEPD) -larch_ev3  $(BSP_LFLAGS) $(LDFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(BEEPD_OBJS) $(BEEPD)
