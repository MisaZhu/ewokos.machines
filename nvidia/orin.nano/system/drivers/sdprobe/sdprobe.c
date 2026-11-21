/*
 * Orin Nano microSD probe: initialise SDHCI, report capacity, dump sector 0
 * (MBR) and list any MBR partitions.  Bring-up test for the SDMMC1 driver.
 *   sdprobe
 */
#include <bsp/bsp_sdmmc.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    if (!bsp_sdmmc_present()) {
        printf("sdprobe: no card detected (GPIO G7)\n");
        return 1;
    }
    if (bsp_sdmmc_init() != 0) {
        printf("sdprobe: card init failed\n");
        return 1;
    }
    uint64_t sectors = bsp_sdmmc_capacity();
    printf("sdprobe: card ready, %llu sectors (%llu MB)\n",
            (unsigned long long)sectors,
            (unsigned long long)(sectors / 2048u));

    uint8_t mbr[512];
    if (bsp_sdmmc_read_blocks(0, mbr, 1) != 0) {
        printf("sdprobe: sector 0 read failed\n");
        return 1;
    }
    printf("sector 0 [0..63]:\n");
    for (int i = 0; i < 64; i += 16) {
        printf("  %02x:", i);
        for (int j = 0; j < 16; j++)
            printf(" %02x", mbr[i + j]);
        printf("\n");
    }
    if (mbr[510] == 0x55 && mbr[511] == 0xaa) {
        printf("MBR signature OK; partitions:\n");
        for (int p = 0; p < 4; p++) {
            const uint8_t* e = mbr + 446 + p * 16;
            uint32_t lba = (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
                    ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
            uint32_t cnt = (uint32_t)e[12] | ((uint32_t)e[13] << 8) |
                    ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
            if (e[4] == 0)
                continue;
            printf("  %d: type=%02x lba=%u sectors=%u (%u MB)\n",
                    p, e[4], lba, cnt, cnt / 2048u);
        }
    } else {
        printf("no MBR signature (raw filesystem or GPT?)\n");
    }
    return 0;
}