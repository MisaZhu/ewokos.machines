DSI_COMMON_DIR = ../dsi
DSI_COMMON_OBJS = $(ARCH)/dsi/uc_time.o $(ARCH)/dsi/uc_backlight.o $(ARCH)/dsi/uc_clock.o $(ARCH)/dsi/uc_fb.o $(ARCH)/dsi/uc_hvs.o $(ARCH)/dsi/uc_power.o $(ARCH)/dsi/uc_pmu.o
TASK_OBJS = $(ARCH)/fbd.o $(ARCH)/uc_panel.o $(ARCH)/uc_dsi.o $(ARCH)/uc_cwu50.o $(ARCH)/uc_cwd686.o $(ARCH)/uc_pv.o $(DSI_COMMON_OBJS)
TASK = fbd 

MF_ROOT_DIR=../..
include $(MF_ROOT_DIR)/make.inc
BUILD_DIR = $(MF_ROOT_DIR)/build/drivers/$(MF)

CFLAGS += -I$(DSI_COMMON_DIR)
CXXFLAGS += -I$(DSI_COMMON_DIR)

all: $(TASK)

$(TASK): $(TASK_OBJS)
	mkdir -p $(BUILD_DIR)
	$(LD) -Ttext=100 $(TASK_OBJS) -o $(BUILD_DIR)/$(TASK) $(LDFLAGS) -lfbd $(EWOK_LIB_GRAPH) $(BSP_LFLAGS) -ltinyjson $(EWOK_LIBC)

$(ARCH)/dsi/%.o: $(DSI_COMMON_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TASK_OBJS)
