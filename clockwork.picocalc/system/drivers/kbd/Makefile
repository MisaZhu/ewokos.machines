SYS_DIR=../../../../../system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS += -L$(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

KBD_OBJS = kbd.o

KBD = $(TARGET_DIR)/drivers/rk3506/kbd

$(KBD): $(KBD_OBJS) \
		$(EWOK_LIBC_A)  \
	 	$(BUILD_DIR)/lib/libarch_rk3506.a
		$(LD) -Ttext=100 $(KBD_OBJS) -o $(KBD) $(LDFLAGS) -larch_rk3506 $(EWOK_LIBC)

clean:
	rm -f $(KBD_OBJS)
