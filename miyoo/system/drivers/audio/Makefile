SYS_DIR=../../../../../system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L$(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

MS_AUD_OBJS = audctrl.o pcm_lib.o miyoo-dais.o reg_ctrl.o

MS_AUD = $(TARGET_DIR)/drivers/miyoo/audctrl

$(MS_AUD): $(MS_AUD_OBJS) \
		$(EWOK_LIBC_A) 
	$(LD) -Ttext=100 $(MS_AUD_OBJS) -o $(MS_AUD) $(LDFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(MS_AUD_OBJS) $(MS_AUD)