/*
 * x86 用户态 AHCI (SATA) HCD。
 *
 * PCI(class 0106) -> BAR5(ABAR) 经 SYS_MEM_MAP 映射 -> 轮询 DMA 块 I/O。
 * 移植自 kernel-testsuite 已在 QEMU 验证的内核 AHCI 实现, 保留其怪癖处理:
 *   - device 寄存器必须置 LBA 位, 否则 QEMU 按 CHS 解释;
 *   - QEMU 对 LBA0 的 LBA48 命令可能回 ERR=ABRT, 单扇区 LBA0 走 LBA28;
 *   - 命令完成需等 CI 清零 + DHRS 中断状态, 之后才能发下一条。
 */

#include <bsp/bsp_ahci.h>
#include <bsp/x86_pio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sysinfo.h>
#include <ewoksys/syscall.h>
#include <ewoksys/klog.h>
#include <ewoksys/dma.h>

static int32_t bsp_ahci_controller_init(void);

/* ---- PCI 配置空间 (0xCF8/0xCFC) ---- */
#define PCI_CFG_ADDR_PORT 0xCF8
#define PCI_CFG_DATA_PORT 0xCFC

static uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t cfg_addr = 0x80000000u |
            ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
            ((uint32_t)func << 8) | (offset & 0xFC);
    x86_outl(PCI_CFG_ADDR_PORT, cfg_addr);
    return x86_inl(PCI_CFG_DATA_PORT);
}

static uint16_t pci_cfg_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t v = pci_cfg_read32(bus, dev, func, offset);
    return (uint16_t)((v >> ((offset & 0x2) * 8)) & 0xFFFF);
}

static void pci_cfg_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint16_t value) {
    uint32_t shift = (offset & 0x2) * 8;
    uint32_t reg = pci_cfg_read32(bus, dev, func, offset);
    reg &= ~(0xFFFFu << shift);
    reg |= ((uint32_t)value << shift);
    uint32_t cfg_addr = 0x80000000u |
            ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
            ((uint32_t)func << 8) | (offset & 0xFC);
    x86_outl(PCI_CFG_ADDR_PORT, cfg_addr);
    x86_outl(PCI_CFG_DATA_PORT, reg);
}

/* ---- HBA 寄存器 ---- */
#define HBA_GHC   0x04
#define HBA_PI    0x0C
#define GHC_HR    (1u << 0)
#define GHC_AE    (1u << 31)

#define PORT_BASE 0x100
#define PORT_SZ   0x80
#define P_CLB   0x00
#define P_CLBU  0x04
#define P_FB    0x08
#define P_FBU   0x0C
#define P_IS    0x10
#define P_CMD   0x18
#define P_TFD   0x20
#define P_SSTS  0x28
#define P_CI    0x38

#define CMD_ST   (1u << 0)
#define CMD_FRE  (1u << 4)
#define SSTS_DET 0xF
#define SSTS_DEV_PRESENT 3

#define AHCI_MAX_SECTORS 128      /* 每条命令 ≤128 扇区 (64KB PRDT 单项) */

/* 命令头 32B: DW0=[4:0]CFL [6]W [31:16]PRDTL, DW1=PRDBC(硬件写) */
typedef struct {
    uint32_t opts;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved[4];
} cmd_hdr_t;

typedef struct {
    uint32_t dba, dbau, reserved;
    uint32_t dbc_i;      /* [22:0]=字节数-1 [31]=I */
} prdt_t;

/* DMA 一致性内存(命令结构 + 数据缓冲都从这里出, 保证对齐) */
static uint8_t *_dma_pool = NULL;
static ewokos_addr_t _dma_pool_phys = 0;
#define DMA_POOL_SIZE (64 * 1024)

#define CLB_OFF  0     /* 1024B, 1024 对齐 */
#define FB_OFF   1024  /* 256B,  256 对齐 */
#define CT_OFF   2048  /* 2048B, 128 对齐 (64B CFIS + 0x80 + PRDT) */
#define BUF_OFF  4096  /* 128*512B 数据缓冲 */

static volatile uint8_t *_abar = NULL;
static uint8_t _ahci_port = 0;
static int32_t _ahci_ready = 0;
static uint8_t _ahci_diag = 0;
static uint64_t _block_count = 0;

static volatile uint32_t *preg(uint8_t port, uint32_t off) {
    return (volatile uint32_t *)(_abar + PORT_BASE + (uint32_t)port * PORT_SZ + off);
}

static inline void ahci_delay(void) {
    for (volatile int i = 0; i < 1000; i++) {
        __asm__ volatile("pause");
    }
}

/* 把 [VA, VA+size) 映射到物理 BAR 并返回 VA。失败返回 NULL */
static volatile uint8_t *map_bar(ewokos_addr_t phy, uint32_t size) {
    /* VA 选在用户地址空间(< 0x80000000)内的空闲位置,
     * 避开内核设备窗口(0xA0000000+)/sys_dma(0xA8000000+)/fb(0xB8000000) */
    ewokos_addr_t va = 0x50000000;
    ewokos_addr_t ret = syscall3(SYS_MEM_MAP, va, phy, size);
    klog("ahci: map bar phy=%llx va=%llx ret=%llx\n",
         (unsigned long long)phy, (unsigned long long)va, (unsigned long long)ret);
    if (ret != va) {
        return NULL;
    }
    return (volatile uint8_t *)va;
}

static int32_t ahci_submit(uint8_t cmd, uint64_t lba, uint16_t count,
                           uint32_t data_off, uint32_t nbytes, int write) {
    if (_abar == NULL) {
        return -1;
    }
    uint8_t *clb = _dma_pool + CLB_OFF;
    uint8_t *ct = _dma_pool + CT_OFF;
    cmd_hdr_t *hdr = (cmd_hdr_t *)clb;

    memset(hdr, 0, sizeof(cmd_hdr_t));
    hdr->opts = (5 & 0x1F)                    /* CFL=5 dwords */
              | (write ? (1u << 6) : 0)       /* W */
              | (1u << 16);                   /* PRDTL=1 */
    hdr->ctba = (uint32_t)(_dma_pool_phys + CT_OFF);
    hdr->ctbau = (uint32_t)((_dma_pool_phys + CT_OFF) >> 32);

    memset(ct, 0, 0x80 + sizeof(prdt_t));
    /* Register FIS Host-to-Device: [0]=0x27, [1]=0x80(C=1), [2]=命令 */
    ct[0] = 0x27;
    ct[1] = 0x80;
    ct[2] = cmd;
    ct[3] = 0;
    /* LBA48: 低 3 字节在 4-6, 高 3 字节在 8-10; device bit6 = LBA 位 */
    ct[4]  = (uint8_t)(lba);
    ct[5]  = (uint8_t)(lba >> 8);
    ct[6]  = (uint8_t)(lba >> 16);
    ct[7]  = 0x40;
    ct[8]  = (uint8_t)(lba >> 24);
    ct[9]  = (uint8_t)(lba >> 32);
    ct[10] = (uint8_t)(lba >> 40);
    ct[11] = 0;
    ct[12] = (uint8_t)(count & 0xFF);
    ct[13] = (uint8_t)(count >> 8);
    ct[14] = 0;
    ct[15] = 1;   /* Control: C=1 */

    prdt_t *prdt = (prdt_t *)(ct + 0x80);
    uint64_t buf_pa = _dma_pool_phys + data_off;
    prdt[0].dba = (uint32_t)buf_pa;
    prdt[0].dbau = (uint32_t)(buf_pa >> 32);
    prdt[0].reserved = 0;
    prdt[0].dbc_i = (nbytes - 1) | (1u << 31);

    __asm__ volatile("mfence" ::: "memory");
    *preg(_ahci_port, P_IS) = 0xFFFFFFFF;
    *preg(_ahci_port, P_CI) = 1;

    /* 轮询 CI 清零 (TCG 下指令速度有限, 上限取够大的固定值) */
    for (uint64_t spin = 0; spin < 20000000ull; spin++) {
        if (!(*preg(_ahci_port, P_CI) & 1)) {
            for (uint64_t w = 0; w < 2000000ull; w++) {
                if (*preg(_ahci_port, P_IS) & 1) {
                    break;
                }
            }
            uint32_t is = *preg(_ahci_port, P_IS);
            uint32_t tfd = *preg(_ahci_port, P_TFD);
            cmd_hdr_t *dh = (cmd_hdr_t *)_dma_pool;
            *preg(_ahci_port, P_IS) = 0xFFFFFFFF;
            if (tfd & 0x01) {
                klog("ahci: cmd %x ERR tfd=%x prdbc=%u\n", cmd, tfd, dh->prdbc);
            } else if (_ahci_diag) {
                klog("ahci: cmd %x ok tfd=%x prdbc=%u data0=%02x%02x\n",
                     cmd, tfd, dh->prdbc,
                     _dma_pool[BUF_OFF + 1], _dma_pool[BUF_OFF]);
            }
            return (tfd & 0x01) ? -1 : 0;
        }
    }

    /* 超时: 诊断并复位端口 */
    klog("ahci: cmd=%x lba=%llu timeout CI=%x TFD=%x CMD=%x IS=%x SIG=%x\n",
         cmd, (unsigned long long)lba, *preg(_ahci_port, P_CI),
         *preg(_ahci_port, P_TFD), *preg(_ahci_port, P_CMD),
         *preg(_ahci_port, P_IS), *preg(_ahci_port, 0x24));
    klog("ahci: cfis %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
         ct[0], ct[1], ct[2], ct[3], ct[4], ct[5], ct[6], ct[7],
         ct[8], ct[9], ct[10], ct[11], ct[12], ct[13], ct[14], ct[15]);
    {
        volatile uint32_t *h32 = (volatile uint32_t *)clb;
        klog("ahci: hdr %08x %08x %08x %08x prdt=%08x %08x\n",
             h32[0], h32[1], h32[2], h32[3], prdt[0].dba, prdt[0].dbc_i);
    }
    uint32_t c = *preg(_ahci_port, P_CMD);
    *preg(_ahci_port, P_CMD) = c & ~(CMD_ST | CMD_FRE);
    while (*preg(_ahci_port, P_CMD) & (CMD_ST | (1u << 14))) {
        ahci_delay();
    }
    *preg(_ahci_port, P_IS) = 0xFFFFFFFF;
    *preg(_ahci_port, P_CMD) = c | (CMD_ST | CMD_FRE);
    return -1;
}

static int32_t ahci_read_sectors_impl(uint64_t lba, uint32_t count, void *buf) {
    /* LBA48 Read DMA Ext=0x25; QEMU 对 LBA0 的 LBA48 可能 ABRT → LBA28 0xC8 兜底 */
    uint8_t *p = (uint8_t *)buf;
    uint8_t *dma_buf = _dma_pool + BUF_OFF;
    while (count) {
        uint32_t n = count > AHCI_MAX_SECTORS ? AHCI_MAX_SECTORS : count;
        int32_t r;
        if (lba == 0 && n == 1) {
            r = ahci_submit(0xC8, lba, 1, BUF_OFF, 512, 0);
        } else {
            r = ahci_submit(0x25, lba, (uint16_t)n, BUF_OFF, n * 512u, 0);
        }
        if (r) {
            return r;
        }
        memcpy(p, dma_buf, (size_t)n * 512);
        p += n * 512;
        lba += n;
        count -= n;
    }
    return 0;
}

static int32_t ahci_write_sectors_impl(uint64_t lba, uint32_t count, const void *buf) {
    uint8_t *dma_buf = _dma_pool + BUF_OFF;
    const uint8_t *p = (const uint8_t *)buf;
    while (count) {
        uint32_t n = count > AHCI_MAX_SECTORS ? AHCI_MAX_SECTORS : count;
        memcpy(dma_buf, p, (size_t)n * 512);
        __asm__ volatile("mfence" ::: "memory");
        int32_t r = ahci_submit(0x35, lba, (uint16_t)n, BUF_OFF, n * 512u, 1);
        if (r) {
            return r;
        }
        p += n * 512;
        lba += n;
        count -= n;
    }
    return 0;
}

static int32_t ahci_identify(void) {
    /* IDENTIFY DEVICE (0xEC, PIO-In): 512B 结果经 PRDT 传入 */
    uint16_t *id = (uint16_t *)(_dma_pool + BUF_OFF);
    if (ahci_submit(0xEC, 0, 0, BUF_OFF, 512, 0) != 0) {
        return -1;
    }
    /* words 100-103: LBA48 最大可寻址扇区数 */
    uint64_t sectors = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
                       ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    if (sectors == 0) {
        /* words 60-61: LBA28 扇区数 */
        sectors = (uint64_t)id[60] | ((uint64_t)id[61] << 16);
    }
    if (sectors == 0) {
        return -1;
    }
    _block_count = sectors;
    return 0;
}

static int32_t bsp_ahci_sd_read_sector(int32_t sector, void *buf) {
    if (sector < 0)
        return -1;
    return bsp_ahci_read((uint64_t)(uint32_t)sector, buf, 1) == 0 ? 0 : -1;
}

static int32_t bsp_ahci_sd_read_sectors(int32_t sector, void *buf,
                    uint32_t count) {
    if (sector < 0)
        return -1;
    return bsp_ahci_read((uint64_t)(uint32_t)sector, buf, count) == 0 ? 0 : -1;
}

static int32_t bsp_ahci_sd_write_sector(int32_t sector, const void *buf) {
    if (sector < 0)
        return -1;
    return bsp_ahci_write((uint64_t)(uint32_t)sector, buf, 1) == 0 ? 0 : -1;
}

/* 供 bsp_sd 在无 legacy IDE 的平台(q35/现代 x86)回退使用 */
int32_t bsp_ahci_register_sd(void) {
    return sd_init_ex(bsp_ahci_controller_init,
                 bsp_ahci_sd_read_sector,
                 bsp_ahci_sd_read_sectors,
                 bsp_ahci_sd_write_sector,
                 bsp_ahci_flush, NULL);
}

int32_t bsp_ahci_init(void) {
    return sd_init_ex(bsp_ahci_controller_init,
                 bsp_ahci_sd_read_sector,
                 bsp_ahci_sd_read_sectors,
                 bsp_ahci_sd_write_sector,
                 bsp_ahci_flush, NULL);
}

static int32_t bsp_ahci_controller_init(void) {
    if (_ahci_ready) {
        return 0;
    }

    /* PCI 探测 class 0106 (SATA/AHCI) */
    uint8_t found_bus = 0, found_dev = 0, found_fn = 0;
    int32_t found = 0;
    for (uint32_t bus = 0; bus < 8 && !found; bus++) {
        for (uint32_t dev = 0; dev < 32 && !found; dev++) {
            for (uint32_t fn = 0; fn < 8 && !found; fn++) {
                if ((pci_cfg_read32(bus, dev, fn, 0) & 0xFFFF) == 0xFFFF) {
                    continue;
                }
                uint32_t cls = pci_cfg_read32(bus, dev, fn, 8);
                if ((cls >> 16) != 0x0106) {
                    continue;
                }
                uint32_t bar5 = pci_cfg_read32(bus, dev, fn, 0x24);
                if ((bar5 & 0x1) != 0 || (bar5 & ~0xFu) == 0) {
                    continue;
                }
                uint16_t cmd = pci_cfg_read16(bus, dev, fn, 0x04);
                pci_cfg_write16(bus, dev, fn, 0x04, (uint16_t)(cmd | 0x0006));
                found_bus = (uint8_t)bus;
                found_dev = (uint8_t)dev;
                found_fn = (uint8_t)fn;
                _abar = map_bar(bar5 & ~0xFu, 0x1000);
                found = (_abar != NULL);
            }
        }
    }
    if (!found) {
        return -1;
    }
    klog("ahci: ahci found at %x:%x.%x abar mapped\n",
         found_bus, found_dev, found_fn);

    /* HBA 复位 + AHCI 使能。
     * 若 HBA 已被前置守护进程初始化(AE=1 且端口在跑), 跳过复位——
     * 复位会打断并发访问, 且 TCG 下 100000 次延时轮询代价高昂,
     * 这是每个 init 守护进程启动耗时 ~90s 的主要原因。 */
    volatile uint32_t *ghc = (volatile uint32_t *)(_abar + HBA_GHC);
    uint32_t pi_early = *(volatile uint32_t *)(_abar + HBA_PI);
    int32_t port_running = 0;
    if ((*ghc & GHC_AE) != 0) {
        for (uint8_t p = 0; p < 32 && !port_running; ++p) {
            if (!(pi_early & (1u << p))) continue;
            if ((*preg(p, P_CMD) & (CMD_ST | CMD_FRE)) == (CMD_ST | CMD_FRE)) {
                port_running = 1;
            }
        }
    }
    if (!port_running) {
        *ghc = GHC_HR;
        for (int i = 0; i < 100000 && (*ghc & GHC_HR); i++) {
            ahci_delay();
        }
        *ghc = GHC_AE;
    }

    /* DMA 一致性池: 按 4KB 向上对齐 — AHCI 要求 CLB 1024B 对齐、
     * CTBA 128B 对齐, 统一页对齐最稳(dma 池起始物理不保证对齐) */
    uint8_t *raw = (uint8_t *)dma_alloc(0, DMA_POOL_SIZE + 4096);
    if (raw == NULL) {
        return -1;
    }
    ewokos_addr_t raw_phys = dma_phy_addr(0, (ewokos_addr_t)raw);
    uint32_t bump = (uint32_t)((0 - (uint32_t)(raw_phys & 0xFFF)) & 0xFFF);
    _dma_pool = raw + bump;
    _dma_pool_phys = raw_phys + bump;
    memset(_dma_pool, 0, DMA_POOL_SIZE);
    klog("ahci: dma pool phys=%llx\n", (unsigned long long)_dma_pool_phys);

    /* 找已连接设备的端口 */
    uint32_t pi = *(volatile uint32_t *)(_abar + HBA_PI);
    klog("ahci: pi=%x ghc=%x\n", pi, *(volatile uint32_t *)(_abar + HBA_GHC));
    for (uint8_t p = 0; p < 32; p++) {
        if (!(pi & (1u << p))) {
            continue;
        }
        klog("ahci: port %u ssts=%x\n", p, *preg(p, P_SSTS));
        if ((*preg(p, P_SSTS) & SSTS_DET) != SSTS_DEV_PRESENT) {
            continue;
        }
        /* 停端口 -> 装命令结构 -> FRE+ST */
        uint32_t cmd = *preg(p, P_CMD);
        *preg(p, P_CMD) = cmd & ~(CMD_ST | CMD_FRE);
        for (int i = 0; i < 100000 && (*preg(p, P_CMD) & (CMD_ST | CMD_FRE)); i++) {
            ahci_delay();
        }
        uint64_t clb_pa = _dma_pool_phys + CLB_OFF;
        uint64_t fb_pa = _dma_pool_phys + FB_OFF;
        *preg(p, P_CLB) = (uint32_t)clb_pa;
        *preg(p, P_CLBU) = (uint32_t)(clb_pa >> 32);
        *preg(p, P_FB) = (uint32_t)fb_pa;
        *preg(p, P_FBU) = (uint32_t)(fb_pa >> 32);
        cmd = *preg(p, P_CMD);
        *preg(p, P_CMD) = cmd | CMD_FRE | CMD_ST;
        for (int i = 0; i < 100000 && !(*preg(p, P_CMD) & CMD_ST); i++) {
            ahci_delay();
        }
        _ahci_port = p;
        int32_t idrc = ahci_identify();
        klog("ahci: port %u identify rc=%d sectors=%llu\n", p, idrc,
             (unsigned long long)_block_count);
        if (idrc == 0) {
            _ahci_ready = 1;
            klog("ahci: port %u ready, %llu sectors (%llu MB)\n", p,
                 (unsigned long long)_block_count,
                 (unsigned long long)(_block_count * 512 / (1024 * 1024)));
            return 0;
        }
        /* 该端口不是可用数据盘, 还原后试下一个 */
        uint32_t c = *preg(p, P_CMD);
        *preg(p, P_CMD) = c & ~(CMD_ST | CMD_FRE);
    }
    return -1;
}

int32_t bsp_ahci_ready(void) {
    return _ahci_ready;
}

int32_t bsp_ahci_read(uint64_t start_lba, void *buf, uint32_t count) {
    if (!_ahci_ready || buf == NULL || count == 0) {
        return -1;
    }
    return ahci_read_sectors_impl(start_lba, count, buf);
}

int32_t bsp_ahci_write(uint64_t start_lba, const void *buf, uint32_t count) {
    if (!_ahci_ready || buf == NULL || count == 0) {
        return -1;
    }
    return ahci_write_sectors_impl(start_lba, count, buf);
}

int32_t bsp_ahci_flush(void) {
    /* CACHE FLUSH EXT (0xEA) */
    if (!_ahci_ready) {
        return -1;
    }
    return ahci_submit(0xEA, 0, 0, BUF_OFF, 512, 0);
}

uint64_t bsp_ahci_get_block_count(void) {
    return _block_count;
}

uint32_t bsp_ahci_get_block_size(void) {
    return 512;
}
