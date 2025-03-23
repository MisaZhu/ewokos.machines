EWOKOS_ROOT_DIR = ../../../../..
SYS_ROOT_DIR = $(EWOKOS_ROOT_DIR)/system
SYS_BUILD_DIR = $(SYS_ROOT_DIR)/build

MF_ROOT_DIR=../..
include $(MF_ROOT_DIR)/../make.inc

TASK_OBJS = xgo.o

TASK = xgo
BUILD_DIR = $(MF_ROOT_DIR)/build/apps/$(TASK)

CXXFLAGS += -I $(MF_ROOT_DIR)/build/include
LDFLAGS += -L$(MF_ROOT_DIR)/build/lib

all: $(TASK)

$(TASK): $(TASK_OBJS) 
	mkdir -p $(BUILD_DIR)
	$(LD) -Ttext=100 $(TASK_OBJS) -o $(BUILD_DIR)/$(TASK) $(LDFLAGS) -lxgo $(EWOK_LIB_X) $(EWOK_LIB_GRAPH) $(EWOK_LIBC) -lcxx
	cp -r res $(BUILD_DIR)

clean:
	rm -f $(TASK_OBJS)


