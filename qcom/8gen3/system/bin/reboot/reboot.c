/* QCOM8GEN3 board reset entry point used by the MCP serial console. */
#include <stdio.h>
#include <stdlib.h>

#include <ewoksys/vdevice.h>

int main(int argc, char **argv) {
	(void)argc;
	(void)argv;
	printf("qcom8gen3 reboot: requesting SYSTEM_RESET via /dev/power0\n");
	char* ret = dev_cmd("/dev/power0", "reboot");
	if (ret == NULL) {
		printf("qcom8gen3 reboot: no reply from powerd\n");
		return 1;
	}
	printf("%s", ret);
	free(ret);
	return 1;
}
