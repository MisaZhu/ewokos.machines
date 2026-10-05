include ../../make.inc
SYS_DIR=$(ewokos)/system/
include $(SYS_DIR)/platform/arm/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)

LDFLAGS = -L $(TARGET_DIR)/lib

# bsp_g2d.c is the pure-software dispatch layer copied from machine.virt:
# it forwards every bsp_g2d_* op to the arch_g2d_* back end. It needs
# g2d_arch.h (platform/$(ARCH)/include) and g2dclient.h (gui/libs) from the
# source tree, because bsp is built in the libs phase before gui installs
# the g2dclient headers.
#
# arch_g2d_* must be resolvable within libbsp so bsp_g2d.o's deps do not
# depend on libgraph being pulled first. We compile g2d_arch.c into an
# ev3-LOCAL object ($(ARCH)/g2d_arch.o under this bsp dir) rather than
# reusing the shared flat platform/$(ARCH)/src/g2d_arch.o: that flat object
# is also built by machine.virt/picocalc (arm v7 + NEON + ARCH_BOOST) and
# make would reuse it stale here, injecting illegal NEON into an arm v5
# image. Building it in-tree with ev3 CFLAGS (v5, no ARCH_BOOST -> the
# portable fallback branch) keeps it isolated and correct.
CFLAGS += -I $(SYS_DIR)/basic/libc/libewoksys/ewoksys/include \
	-I $(SYS_DIR)/platform/$(ARCH)/include/$(ARCH) \
	-I $(SYS_DIR)/basic/libs/sd/include \
	-I $(SYS_DIR)/basic/libs/gpio/include \
	-I $(SYS_DIR)/gui/libs/usb/include \
	-I $(SYS_DIR)/gui/libs/hid/include \
	-I $(SYS_DIR)/gui/libs/bt/include \
	-I $(SYS_DIR)/gui/libs/g2dclient/include \
	-Iinclude -I $(TARGET_DIR)/include

G2D_ARCH_OBJ = $(ARCH)/g2d_arch.o

LIB_BSP_OBJS = $(ARCH)/src/bsp_sd.o $(ARCH)/src/bsp_fb.o $(ARCH)/src/bsp_gpio.o $(ARCH)/src/bsp_i2c.o $(ARCH)/src/bsp_usb.o $(ARCH)/src/bsp_bt.o \
	$(ARCH)/src/bsp_g2d.o $(G2D_ARCH_OBJ)

BSP = $(TARGET_DIR)/lib/libbsp.a 

$(BSP): $(LIB_BSP_OBJS) \
		$(BUILD_DIR)/lib/libarch_ev3.a
	$(AR) rcs $(BSP) $(LIB_BSP_OBJS)  $(BUILD_DIR)/lib/libarch_ev3.a
	cp -r include/* $(TARGET_DIR)/include

$(G2D_ARCH_OBJ): $(SYS_DIR)/platform/$(ARCH)/src/g2d_arch.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(LIB_BSP_OBJS)
	rm -f $(BSP)

