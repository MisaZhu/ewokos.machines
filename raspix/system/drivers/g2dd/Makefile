include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

G2DD_OBJS = $(ARCH)/g2dd.o

G2DD = $(TARGET_DIR)/drivers/raspix/g2dd

$(G2DD): $(G2DD_OBJS) \
		$(SDK_DIR)/lib/libewoksys.a \
		$(SDK_DIR)/lib/libtinyjson.a \
		$(SDK_DIR)/lib/libarch_bcm283x.a \
		$(SDK_DIR)/lib/libbsp.a
	$(LD) -Ttext=100 $(G2DD_OBJS) -o $(G2DD) $(LDFLAGS) $(EWOK_LIB_GRAPH) $(BSP_LFLAGS) -ltinyjson $(EWOK_LIBC)

clean:
	rm -f $(G2DD_OBJS)
	rm -f $(G2DD)
