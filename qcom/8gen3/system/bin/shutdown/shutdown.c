/* QCOM8GEN3 board power-off entry point used by the MCP serial console. */
#include <stdio.h>
#include <stdlib.h>

#include <ewoksys/vdevice.h>

int main(int argc, char **argv) {
	(void)argc;
	(void)argv;
	printf("qcom8gen3 shutdown: requesting SYSTEM_OFF via /dev/power0\n");
	char* ret = dev_cmd("/dev/power0", "shutdown");
	if (ret == NULL) {
		printf("qcom8gen3 shutdown: no reply from powerd\n");
		return 1;
	}
	printf("%s", ret);
	free(ret);
	return 1;
}
