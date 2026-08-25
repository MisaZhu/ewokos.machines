include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

MS_AUD_OBJS = $(ARCH)/audctrl.o $(ARCH)/pcm_lib.o $(ARCH)/miyoo-dais.o $(ARCH)/reg_ctrl.o

MS_AUD = $(TARGET_DIR)/drivers/miyoo/audctrl

$(MS_AUD): $(MS_AUD_OBJS) \
		$(SDK_DIR)/lib/libewoksys.a
	$(LD) -Ttext=100 $(MS_AUD_OBJS) -o $(MS_AUD) $(LDFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(MS_AUD_OBJS) $(MS_AUD)