include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs
TARGET = $(TARGET_DIR)/drivers/q8b/wlan
OBJS = $(ARCH)/wlan.o $(ARCH)/wcn_mhi.o $(ARCH)/wcn_qmi.o $(ARCH)/wcn_board.o $(ARCH)/wcn_ce.o $(ARCH)/wcn_htc.o $(ARCH)/wcn_wmi.o $(ARCH)/wcn_htt.o $(ARCH)/wcn_dp.o $(ARCH)/wcn_sta.o $(ARCH)/wcn_wpa.o $(ARCH)/wcn_net.o $(ARCH)/wcn_util.o $(ARCH)/wcn_config.o
CFLAGS += -I $(BUILD_DIR)/include -DWOLFSSL_USER_SETTINGS
LDFLAGS = -L $(BUILD_DIR)/lib

# tinyjson backs the pi5-compatible network.json parser and the state/list
# devcmd replies, so the driver links it like the raspi5 wland does.
$(TARGET): $(OBJS) $(BUILD_DIR)/lib/libewoksys.a $(BUILD_DIR)/lib/libwolfssl.a \
		$(BUILD_DIR)/lib/libbsp.a $(BUILD_DIR)/lib/libtinyjson.a
	$(LD) -Ttext=100 $(OBJS) -o $@ $(LDFLAGS) -ltinyjson -lwolfssl -lbsp $(EWOK_LIBC)
	@mkdir -p $(TARGET_DIR)/etc/wlan
	@if [ ! -f $(TARGET_DIR)/etc/wlan/network.json ]; then \
		cp network.json $(TARGET_DIR)/etc/wlan/; \
	fi

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(TARGET)
.PHONY: clean
