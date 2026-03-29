TASK_OBJS = xgo.o
TASK = xgo

include ../../../make.inc

CXXFLAGS += -I $(SYS_BUILD_DIR)/$(HW)/include
LDFLAGS += -L $(SYS_BUILD_DIR)/$(HW)/lib

MF_ROOT_DIR=../..
BUILD_DIR = $(MF_ROOT_DIR)/build/apps/$(TASK)

all: $(TASK)

$(TASK): $(TASK_OBJS) 
	mkdir -p $(BUILD_DIR)
	$(LD) -Ttext=100 $(TASK_OBJS) -o $(BUILD_DIR)/$(TASK) $(LDFLAGS) -lxgo $(EWOK_LIB_X) $(EWOK_LIB_GRAPH) $(EWOK_LIBC) -lcxx
	cp -r res $(BUILD_DIR)

clean:
	rm -f $(TASK_OBJS)


