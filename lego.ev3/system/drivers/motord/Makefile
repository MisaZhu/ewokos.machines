include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

MOTORD_OBJS = $(ARCH)/motord.o

MOTORD = $(TARGET_DIR)/drivers/ev3/motord

$(MOTORD): $(MOTORD_OBJS) \
		$(SDK_DIR)/lib/libewoksys.a \
		$(SDK_DIR)/lib/libbsp.a
	$(LD) -Ttext=100 $(MOTORD_OBJS) -o $(MOTORD) -larch_ev3  $(BSP_LFLAGS) $(LDFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(MOTORD_OBJS) $(MOTORD)
