include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

CPUD_OBJS = $(ARCH)/cpud.o

CPUD = $(TARGET_DIR)/drivers/raspix/cpud

$(CPUD): $(CPUD_OBJS) \
		$(BUILD_DIR)/lib/libarch_bcm283x.a \
		$(EWOK_LIBC_A)
	$(LD) -Ttext=100 $(CPUD_OBJS) -o $(CPUD) $(LDFLAGS) -larch_bcm283x $(EWOK_LIBC)

clean:
	rm -f $(CPUD_OBJS) $(CPUD)
