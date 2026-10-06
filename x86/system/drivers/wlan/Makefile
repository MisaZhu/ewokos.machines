include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I. -Icompat -Iiwl -I $(BUILD_DIR)/include

WLAN_OBJS = $(ARCH)/iwl/pcie.o \
	$(ARCH)/iwl/trans.o \
	$(ARCH)/iwl/fwfile.o \
	\
	$(ARCH)/mvm/mvm.o \
	$(ARCH)/mvm/scan.o \
	$(ARCH)/mvm/mlme.o \
	$(ARCH)/mvm/data.o \
	\
	$(ARCH)/utils/qbuf.o \
	$(ARCH)/utils/utils.o \
	$(ARCH)/utils/log.o \
	$(ARCH)/utils/skb.o \
	$(ARCH)/utils/config.o \
	$(ARCH)/utils/pbkdf2.o \
	$(ARCH)/utils/aes.o \
	\
	$(ARCH)/main.o

WLAN = $(TARGET_DIR)/drivers/x86/wland

$(WLAN): $(WLAN_OBJS) \
		$(SDK_DIR)/lib/libewoksys.a \
		$(SDK_DIR)/lib/libbsp.a \
		$(SDK_DIR)/lib/libtinyjson.a
	$(LD) -Ttext=100 $(WLAN_OBJS) -o $(WLAN) $(LDFLAGS) -ltinyjson $(BSP_LFLAGS) $(EWOK_LIBC)
	mkdir -p $(TARGET_DIR)/etc/wlan/
	cp -r network.json $(TARGET_DIR)/etc/wlan/

clean:
	rm -f $(WLAN_OBJS) $(WLAN)
