include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

I2CD_OBJS = $(ARCH)/i2cd.o

I2CD = $(TARGET_DIR)/drivers/ev3/i2cd

$(I2CD): $(I2CD_OBJS) \
		$(SDK_DIR)/lib/libewoksys.a \
		$(SDK_DIR)/lib/libbsp.a
	$(LD) -Ttext=100 $(I2CD_OBJS) -o $(I2CD) -larch_ev3  $(BSP_LFLAGS) $(LDFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(I2CD_OBJS) $(I2CD)
