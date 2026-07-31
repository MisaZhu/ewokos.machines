#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <bcm283x/mmc.h>


int32_t bcm283x_sd_init(void) {
        return mmc_init();
}

int32_t bcm283x_sd_read(int32_t sector, void* buf, int count) {
        uint32_t result;

        if (buf == NULL || count <= 0) {
                return -1;
        }

        result = mmc_read_blocks(buf, sector, (uint32_t)count);
        if (result != (uint32_t)count) {
                return -1;
        }
	return 0;
}

int32_t bcm283x_sd_write(int32_t sector, const void* buf, int count) {
	return -1;
}
