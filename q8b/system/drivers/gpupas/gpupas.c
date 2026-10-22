/* Read-only SCM/PAS capability query. No GPU MMIO, firmware loading or reset. */
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <ewoksys/syscall.h>
#include <q8b_gpu_pas.h>
int main(void) {
	int ret, retry = 0;
	do {
		ret = (int)syscall3(SYS_PLATFORM_CTL, Q8B_GPU_PAS_QUERY, 0, 0);
		if (ret != Q8B_GPU_PAS_BUSY) break;
		usleep(30000);
	} while (++retry <= 20);
	printf("Q8B GPU PAS query: %08x (%d), expected ABI %08x\n",
	       (uint32_t)ret, ret, Q8B_GPU_PAS_ABI);
	if ((uint32_t)ret != Q8B_GPU_PAS_ABI) return 1;
	printf("SCM64 transport available; advertised PAS support is logged by kernel.\n");
	printf("Query success does not establish authentication success or zap optionality.\n");
	return 0;
}
