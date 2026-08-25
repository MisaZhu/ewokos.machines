include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I. -I $(BUILD_DIR)/include

BRCMFMACD_OBJS = $(ARCH)/sdio/sdhci.o \
				 $(ARCH)/sdio/mmc.o \
				 $(ARCH)/sdio/sdio.o \
				 \
				 $(ARCH)/utils/qbuf.o \
				 $(ARCH)/utils/utils.o \
				 $(ARCH)/utils/log.o \
				 $(ARCH)/utils/skb.o \
				 $(ARCH)/utils/config.o \
				 $(ARCH)/utils/pbkdf2.o \
				 \
				 $(ARCH)/brcm/brcm.o \
				 $(ARCH)/brcm/chip.o \
				 $(ARCH)/brcm/firmware.o \
				 $(ARCH)/brcm/firmware_43430.o \
				 $(ARCH)/brcm/firmware_43436.o \
				 $(ARCH)/brcm/command.o \
				 $(ARCH)/brcm/console.o \
				 $(ARCH)/main.o

 
BRCMFMACD = $(TARGET_DIR)/drivers/raspix/wland

$(BRCMFMACD): $(BRCMFMACD_OBJS) \
		$(SDK_DIR)/lib/libewoksys.a \
		$(SDK_DIR)/lib/libgloss.a  \
		$(SDK_DIR)/lib/libarch_bcm283x.a \
		$(SDK_DIR)/lib/libtinyjson.a
	$(LD) -Ttext=100 $(BRCMFMACD_OBJS) -o $(BRCMFMACD) $(LDFLAGS) -ltinyjson -larch_bcm283x $(EWOK_LIBC) 
	mkdir -p  $(TARGET_DIR)/etc/wlan/
	cp -r network.json $(TARGET_DIR)/etc/wlan/

clean:
	rm -f $(BRCMFMACD_OBJS)
