include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

ADCD_OBJS = $(ARCH)/main.o

ADCD = $(TARGET_DIR)/drivers/ev3/adcd

$(ADCD): $(ADCD_OBJS) \
		$(BUILD_DIR)/lib/libewoksys.a \
		$(BUILD_DIR)/lib/libbsp.a
	$(LD) -Ttext=100 $(ADCD_OBJS) -o $(ADCD) -larch_ev3  $(BSP_LFLAGS) $(LDFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(ADCD_OBJS) $(ADCD)
