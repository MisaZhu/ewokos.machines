include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SDK_DIR)
TARGET_DIR = $(BUILD_DIR)/rootfs
APP_DIR = $(TARGET_DIR)/apps/qnndemo
TARGET = $(APP_DIR)/qnndemo
RES_FILES = $(wildcard res/*)

OBJS = $(ARCH)/src/qnndemo.o

# qnn_core.h / qnn_htp_ext.h are installed into the SDK include dir by
# machines/qcom/8gen3/system/libs/adrenonpu.  The link group resolves the
# libadrenonpu -> openlibm/ewoksys/libc references regardless of order.
CFLAGS += -I $(BUILD_DIR)/include
QNNDEMO_LIBS = --start-group -ladrenonpu -lopenlibm -lewoksys -lc -lgloss --end-group

all: $(TARGET) install-res

$(TARGET): $(OBJS) \
		$(BUILD_DIR)/lib/libadrenonpu.a \
		$(BUILD_DIR)/lib/libewoksys.a \
		$(BUILD_DIR)/lib/libopenlibm.a
	@mkdir -p $(dir $@)
	$(LD) -Ttext=100 $(OBJS) -o $@ $(LDFLAGS) $(QNNDEMO_LIBS)

install-res: $(TARGET)
	@mkdir -p $(APP_DIR)/res
	@for f in $(RES_FILES); do cp -p $$f $(APP_DIR)/res/; done

clean:
	rm -f $(OBJS) $(OBJS:.o=.d)
	rm -rf $(APP_DIR)

.PHONY: all install-res clean