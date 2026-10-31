include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET = $(BUILD_DIR)/rootfs/drivers/q8b/wlan
OBJS = $(ARCH)/wlan.o $(ARCH)/wcn_mhi.o $(ARCH)/wcn_qmi.o $(ARCH)/wcn_board.o $(ARCH)/wcn_ce.o $(ARCH)/wcn_htc.o $(ARCH)/wcn_wmi.o $(ARCH)/wcn_htt.o
CFLAGS += -I $(BUILD_DIR)/include
LDFLAGS = -L $(BUILD_DIR)/lib

$(TARGET): $(OBJS) $(BUILD_DIR)/lib/libewoksys.a
	$(LD) -Ttext=100 $(OBJS) -o $@ $(LDFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(TARGET)
.PHONY: clean
