include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib -Llib 

CFLAGS += -I $(BUILD_DIR)/include

KBD_OBJS = $(ARCH)/joystickd.o

KBD = $(TARGET_DIR)/drivers/ev3/gpio_joystickd

$(KBD): $(KBD_OBJS) \
		$(BUILD_DIR)/lib/libewoksys.a \
		$(BUILD_DIR)/lib/libbsp.a
	$(LD) -Ttext=100 $(KBD_OBJS) -o $(KBD) $(LDFLAGS) -larch_ev3  $(BSP_LFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(KBD_OBJS)
