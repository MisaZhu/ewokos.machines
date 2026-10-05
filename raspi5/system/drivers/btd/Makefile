include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include \
          -I $(SYS_DIR)/basic/libs/hid/include

BTD_OBJS = $(ARCH)/btd.o $(ARCH)/btd_known.o $(ARCH)/btd_classic.o \
           $(ARCH)/btd_l2cap.o $(ARCH)/btd_hid.o $(ARCH)/btd_le.o

BTD = $(TARGET_DIR)/drivers/raspi5/btd

$(BTD): $(BTD_OBJS) \
		$(SDK_DIR)/lib/libewoksys.a \
		$(SDK_DIR)/lib/libhid.a
	$(LD) -Ttext=100 $(BTD_OBJS) -o $(BTD) $(LDFLAGS) -lhid $(EWOK_LIBC)

# All modules share the channel/session layout. Rebuild every object when it changes.
$(BTD_OBJS): btd_int.h
$(ARCH)/btd.o: firmware_4345c0.h

.PHONY: test-host
test-host:
	python3 tests/test_handshake.py

clean:
	rm -f $(BTD_OBJS) $(BTD)
