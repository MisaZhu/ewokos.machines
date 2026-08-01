include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

G2DD_OBJS = $(ARCH)/g2dd.o $(ARCH)/vc_g2d.o

G2DD = $(TARGET_DIR)/drivers/raspix/g2dd

$(G2DD): $(G2DD_OBJS) \
		$(EWOK_LIBC_A) \
		$(BUILD_DIR)/lib/libgraph.a \
		$(BUILD_DIR)/lib/libtinyjson.a \
		$(BUILD_DIR)/lib/libarch_bcm283x.a \
		$(BUILD_DIR)/lib/libbsp.a
	$(LD) -Ttext=100 $(G2DD_OBJS) -o $(G2DD) $(LDFLAGS) $(EWOK_LIB_GRAPH) -ltinyjson $(BSP_LFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(G2DD_OBJS)
