/*
 * Orin Nano microSD probe: initialise SDHCI, report capacity, dump sector 0
 * (MBR) and list any MBR partitions.  Bring-up test for the SDMMC1 driver.
 * Output goes to klog (serial console) so it is visible when autostarted from
 * init.ramfs.rd before the rescue console owns stdout.
 *   sdprobe
 */
#include <bsp/bsp_sdmmc.h>
#include <stdint.h>
#include <string.h>
#include <ewoksys/klog.h>

int main(void) {
    klog("sdprobe: start\n");
    if (!bsp_sdmmc_present()) {
        klog("sdprobe: no card detected (GPIO G7)\n");
        return 1;
    }
    if (bsp_sdmmc_init() != 0) {
        klog("sdprobe: card init failed\n");
        return 1;
    }
    uint64_t sectors = bsp_sdmmc_capacity();
    klog("sdprobe: card ready, %llu sectors (%llu MB)\n",
            (unsigned long long)sectors,
            (unsigned long long)(sectors / 2048u));

    uint8_t mbr[512];
    if (bsp_sdmmc_read_blocks(0, mbr, 1) != 0) {
        klog("sdprobe: sector 0 read failed\n");
        return 1;
    }
    klog("sector 0 [0..63]:\n");
    for (int i = 0; i < 64; i += 16) {
        klog("  %02x: %02x %02x %02x %02x %02x %02x %02x %02x"
                " %02x %02x %02x %02x %02x %02x %02x %02x\n", i,
                mbr[i+0], mbr[i+1], mbr[i+2], mbr[i+3],
                mbr[i+4], mbr[i+5], mbr[i+6], mbr[i+7],
                mbr[i+8], mbr[i+9], mbr[i+10], mbr[i+11],
                mbr[i+12], mbr[i+13], mbr[i+14], mbr[i+15]);
    }
    if (mbr[510] == 0x55 && mbr[511] == 0xaa) {
        klog("MBR signature OK; partitions:\n");
        uint32_t p0_lba = 0;
        for (int p = 0; p < 4; p++) {
            const uint8_t* e = mbr + 446 + p * 16;
            uint32_t lba = (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
                    ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
            uint32_t cnt = (uint32_t)e[12] | ((uint32_t)e[13] << 8) |
                    ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
            if (e[4] == 0)
                continue;
            if (p0_lba == 0)
                p0_lba = lba;
            klog("  %d: type=%02x lba=%u sectors=%u (%u MB)\n",
                    p, e[4], lba, cnt, cnt / 2048u);
        }
        /* Multi-block read path: 4 consecutive sectors from LBA 0. */
        static uint8_t multi[4 * 512];
        if (bsp_sdmmc_read_blocks(0, multi, 4) == 0 &&
                multi[510] == 0x55 && multi[511] == 0xaa)
            klog("multi-block read (4 sectors) OK\n");
        else
            klog("multi-block read FAILED\n");
        /* Read the first partition's boot sector (read-only). */
        if (p0_lba != 0) {
            static uint8_t bs[512];
            if (bsp_sdmmc_read_blocks(p0_lba, bs, 1) == 0) {
                klog("partition0 @lba=%u boot [0..15]:", p0_lba);
                for (int i = 0; i < 16; i++)
                    klog(" %02x", bs[i]);
                klog("\n");
                klog("  boot sig=%02x%02x oem='%.8s'\n",
                        bs[511], bs[510], bs + 3);
            } else {
                klog("partition0 boot read FAILED\n");
            }
        }
    } else {
        klog("no MBR signature (raw filesystem or GPT?)\n");
    }
    return 0;
}