include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include \
          -I $(SYS_DIR)/basic/libs/hid/include

BTD_OBJS = $(ARCH)/btd.o

BTD = $(TARGET_DIR)/drivers/raspi5/btd

$(BTD): $(BTD_OBJS) \
		$(SDK_DIR)/lib/libewoksys.a \
		$(SDK_DIR)/lib/libhid.a
	$(LD) -Ttext=100 $(BTD_OBJS) -o $(BTD) $(LDFLAGS) -lhid $(EWOK_LIBC)

clean:
	rm -f $(BTD_OBJS) $(BTD)
