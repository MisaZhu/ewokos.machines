#include <bsp/bsp_sd.h>
#include <bsp/x86_pio.h>
#include <bsp/bsp_ahci.h>
#include <sd/sd.h>
#include <sysinfo.h>
#include <ewoksys/syscall.h>
#include <string.h>
#include <stdint.h>

/* legacy IDE: primary/secondary × master/slave, LBA48 (与内核 bsp/sd.c 一致) */
static const uint16_t _ata_bases[2] = { 0x1F0, 0x170 };
static uint16_t _ata_base = 0x1F0;
static uint16_t _ata_ctl = 0x3F6;
static uint8_t _ata_drvsel = 0xA0;

#define ATA_DATA        0x00
#define ATA_SECCOUNT0   0x02
#define ATA_LBA0        0x03
#define ATA_LBA1        0x04
#define ATA_LBA2        0x05
#define ATA_HDDEVSEL    0x06
#define ATA_COMMAND     0x07
#define ATA_STATUS      0x07

#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_CACHE_FLUSH 0xE7

#define ATA_SR_BSY 0x80
#define ATA_SR_DRQ 0x08
#define ATA_SR_ERR 0x01

static int ata_wait_ready(void) {
    for (int i = 0; i < 100000; ++i) {
        uint8_t status = x86_inb(_ata_base + ATA_STATUS);
        if (status == 0xFF) {
            return -1;  /* 浮空总线: 无 legacy IDE (q35) */
        }
        if ((status & ATA_SR_BSY) == 0) {
            return (status & ATA_SR_ERR) ? -1 : 0;
        }
    }
    return -1;
}

static void ata_400ns_wait(void) {
    for (int i = 0; i < 4; ++i) {
        (void)x86_inb(_ata_ctl);
    }
}

static void ata_channel_reset(uint16_t base) {
    uint16_t ctl = (uint16_t)(base + 0x206);
    x86_outb(ctl, 0x04);
    for (int i = 0; i < 4; ++i) (void)x86_inb(ctl);
    x86_outb(ctl, 0x00);
    for (int i = 0; i < 4; ++i) (void)x86_inb(ctl);
    for (int i = 0; i < 200000; ++i) {
        uint8_t st = x86_inb(base + ATA_STATUS);
        if (st == 0xFF || (st & ATA_SR_BSY) == 0) break;
    }
}

static int ata_identify(uint16_t base, uint8_t drvsel) {
    uint16_t id[256];
    x86_outb(base + ATA_HDDEVSEL, drvsel);
    for (int i = 0; i < 4; ++i) (void)x86_inb((uint16_t)(base + 0x206));
    x86_outb(base + ATA_SECCOUNT0, 0);
    x86_outb(base + ATA_LBA0, 0);
    x86_outb(base + ATA_LBA1, 0);
    x86_outb(base + ATA_LBA2, 0);
    x86_outb(base + ATA_COMMAND, 0xEC);
    for (int i = 0; i < 200000; ++i) {
        uint8_t st = x86_inb(base + ATA_STATUS);
        if (st == 0xFF) return -1;
        if ((st & ATA_SR_BSY) == 0) {
            if (st & ATA_SR_ERR) return -1;
            if (st & ATA_SR_DRQ) {
                for (int w = 0; w < 256; ++w) id[w] = x86_inw(base + ATA_DATA);
                return 0;
            }
            return -1;
        }
    }
    return -1;
}

/* LBA48 任务文件装载 */
static void ata_tf_lba48(int32_t sector, uint16_t nsect) {
    uint16_t b = _ata_base;
    x86_outb(b + ATA_SECCOUNT0, 0);
    x86_outb(b + ATA_SECCOUNT0, (uint8_t)nsect);
    x86_outb(b + ATA_LBA0, (uint8_t)(((uint32_t)sector >> 24) & 0xFF));
    x86_outb(b + ATA_LBA1, 0);
    x86_outb(b + ATA_LBA2, 0);
    x86_outb(b + ATA_LBA0, (uint8_t)((uint32_t)sector & 0xFF));
    x86_outb(b + ATA_LBA1, (uint8_t)(((uint32_t)sector >> 8) & 0xFF));
    x86_outb(b + ATA_LBA2, (uint8_t)(((uint32_t)sector >> 16) & 0xFF));
    x86_outb(b + ATA_HDDEVSEL, (uint8_t)(0x40 | _ata_drvsel));
}

static int ata_wait_drq(void) {
    for (int i = 0; i < 100000; ++i) {
        uint8_t status = x86_inb(_ata_base + ATA_STATUS);
        if ((status & ATA_SR_BSY) == 0 && (status & ATA_SR_DRQ) != 0) {
            return 0;
        }
        if (status & ATA_SR_ERR) {
            return -1;
        }
    }
    return -1;
}

static void ata_select_lba28(uint32_t sector) {
    (void)sector;   /* 已由 ata_tf_lba48 取代 */
}

/* ---- 内存盘 rootfs (UEFI 引导 ROOTFS.IMG; 内核挖洞保留, 这里映射读写) ---- */
static uint8_t *_rd_va = NULL;
static uint32_t _rd_size = 0;

static int32_t rd_init(void) {
    sys_info_t sysinfo;
    ewokos_addr_t ret;
    int32_t src = (int32_t)syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&sysinfo);
    klog("sdfsd: rd probe src=%d machine=%s mem=%llx phy=%llx size=%x\n",
            src, sysinfo.machine, (unsigned long long)sysinfo.total_phy_mem_size,
            (unsigned long long)sysinfo.rd.phy_base, sysinfo.rd.size);
    if (sysinfo.rd.size == 0 || sysinfo.rd.phy_base == 0) {
        return -1;
    }
    /* VA 选在用户地址空间空闲位置 (与 ahci.c map_bar 的 0x50000000 错开) */
    ewokos_addr_t va = 0x60000000;
    ret = syscall3(SYS_MEM_MAP, va, sysinfo.rd.phy_base,
            sysinfo.rd.size);
    klog("sdfsd: rd map va=%llx ret=%llx\n",
            (unsigned long long)va, (unsigned long long)ret);
    if (ret != va) {
        return -1;
    }
    _rd_va = (uint8_t *)va;
    _rd_size = sysinfo.rd.size;
    return 0;
}

static int32_t rd_read_sector(int32_t sector, void* buf) {
    if (_rd_va == NULL || sector < 0 ||
            (uint32_t)sector * 512 + 512 > _rd_size) {
        return -1;
    }
    memcpy(buf, _rd_va + (uint32_t)sector * 512, 512);
    return 0;
}

static int32_t rd_write_sector(int32_t sector, const void* buf) {
    if (_rd_va == NULL || sector < 0 ||
            (uint32_t)sector * 512 + 512 > _rd_size) {
        return -1;
    }
    memcpy(_rd_va + (uint32_t)sector * 512, buf, 512);
    return 0;   /* 写落 RAM, 重启即失 */
}

static int32_t x86_sd_init(void) {
    /* 全位置探测: pri-mas, pri-sla, sec-mas, sec-sla */
    for (uint32_t pos = 0; pos < 4; ++pos) {
        uint16_t base = _ata_bases[pos / 2];
        uint8_t drv = (pos % 2) ? 0xB0 : 0xA0;
        if ((pos % 2) == 0) {
            ata_channel_reset(base);
        }
        if (ata_identify(base, drv) == 0) {
            _ata_base = base;
            _ata_ctl = (uint16_t)(base + 0x206);
            _ata_drvsel = drv;
            return 0;
        }
    }
    return -1;
}

static int32_t x86_sd_read_sector(int32_t sector, void* buf) {
    if (ata_wait_ready() != 0) {
        return -1;
    }
    ata_tf_lba48(sector, 1);
    x86_outb(_ata_base + ATA_COMMAND, 0x24);
    if (ata_wait_drq() != 0) {
        return -1;
    }
    for (int i = 0; i < 256; ++i) {
        ((uint16_t*)buf)[i] = x86_inw(_ata_base + ATA_DATA);
    }
    return 0;
}

static int32_t x86_sd_write_sector(int32_t sector, const void* buf) {
    if (ata_wait_ready() != 0) {
        return -1;
    }
    ata_tf_lba48(sector, 1);
    x86_outb(_ata_base + ATA_COMMAND, 0x34);
    if (ata_wait_drq() != 0) {
        return -1;
    }
    for (int i = 0; i < 256; ++i) {
        x86_outw(_ata_base + ATA_DATA, ((const uint16_t*)buf)[i]);
    }
    x86_outb(_ata_base + ATA_COMMAND, 0xE7);
    return ata_wait_ready();
}

int bsp_sd_init(void) {
    /* 内存盘 rootfs 最优先 (ISO/Ventoy 引导无 ATA/AHCI/NVMe 根盘) */
    if (rd_init() == 0) {
        return sd_init(rd_init, rd_read_sector, rd_write_sector);
    }
    /* legacy ATA PIO 优先; 无 legacy IDE 的平台(q35/现代 x86)回退 AHCI */
    if (x86_sd_init() == 0) {
        return sd_init(x86_sd_init, x86_sd_read_sector, x86_sd_write_sector);
    }
    return bsp_ahci_register_sd();
}
