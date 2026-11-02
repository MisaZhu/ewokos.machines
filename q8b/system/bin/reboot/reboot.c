/* Q8B board reset entry point used by the MCP serial console. */
#include <stdint.h>
#include <stdio.h>

#include <ewoksys/syscall.h>
#include <q8b_npu.h>

int main(int argc, char **argv) {
	(void)argc;
	(void)argv;
	printf("q8b reboot: requesting PSCI SYSTEM_RESET\n");
	/* The platform handler does not return on a functioning PSCI path. */
	uint64_t ret = syscall3(SYS_PLATFORM_CTL, Q8B_NPU_SYS_RESET, 0, 0);
	printf("q8b reboot: reset returned %llu\n", (unsigned long long)ret);
	return 1;
}
