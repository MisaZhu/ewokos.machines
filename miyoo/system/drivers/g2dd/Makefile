include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

ifdef G2DD_ENABLE_MI_GFX
TOOLCHAIN_TRIPLE := $(shell $(CC) -dumpmachine 2>/dev/null)
ifneq ($(findstring linux,$(TOOLCHAIN_TRIPLE)),linux)
$(error G2DD_ENABLE_MI_GFX requires a Linux ARM hard-float toolchain and glibc-compatible SigmaStar MI libraries; current toolchain '$(TOOLCHAIN_TRIPLE)' is incompatible)
endif
CFLAGS += -DG2DD_ENABLE_MI_GFX
ifneq ($(SIGMASTAR_MI_LIB_DIR),)
LDFLAGS += -L $(SIGMASTAR_MI_LIB_DIR)
endif
SIGMASTAR_MI_LIBS += -lmi_gfx -lmi_sys
endif

G2DD_OBJS = $(ARCH)/g2dd.o

G2DD = $(TARGET_DIR)/drivers/miyoo/g2dd

$(G2DD): $(G2DD_OBJS) \
		$(SDK_DIR)/lib/libewoksys.a \
		$(SDK_DIR)/lib/libdisplay.a \
		$(SDK_DIR)/lib/libgraph.a \
		$(SDK_DIR)/lib/libfont.a \
		$(SDK_DIR)/lib/libdisplayman.a \
		$(SDK_DIR)/lib/libarch_miyoo.a \
		$(SDK_DIR)/lib/libbsp.a
	$(LD) -Ttext=100 $(G2DD_OBJS) -o $(G2DD) $(LDFLAGS) $(SIGMASTAR_MI_LIBS) $(EWOK_LIB_GRAPH) $(BSP_LFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(G2DD_OBJS)
