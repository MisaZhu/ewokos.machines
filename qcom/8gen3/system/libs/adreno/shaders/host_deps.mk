# Included after the device Makefile so OBJS is the same source list.
$(OBJS): $(HOST_SHADER_DIR)/host_config.h $(HOST_SHADER_DIR)/Makefile.host $(HOST_SHADER_DIR)/host_deps.mk
-include $(OBJS:.o=.d)
