include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET = $(BUILD_DIR)/rootfs/drivers/q8b/wlan
OBJS = $(ARCH)/wlan.o $(ARCH)/wcn_mhi.o $(ARCH)/wcn_qmi.o $(ARCH)/wcn_board.o $(ARCH)/wcn_ce.o $(ARCH)/wcn_htc.o $(ARCH)/wcn_wmi.o $(ARCH)/wcn_htt.o $(ARCH)/wcn_dp.o $(ARCH)/wcn_sta.o $(ARCH)/wcn_wpa.o $(ARCH)/wcn_net.o
CFLAGS += -I $(BUILD_DIR)/include -DWOLFSSL_USER_SETTINGS
LDFLAGS = -L $(BUILD_DIR)/lib

$(TARGET): $(OBJS) $(BUILD_DIR)/lib/libewoksys.a $(BUILD_DIR)/lib/libwolfssl.a
	$(LD) -Ttext=100 $(OBJS) -o $@ $(LDFLAGS) -lwolfssl $(EWOK_LIBC)

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(TARGET)
.PHONY: clean
