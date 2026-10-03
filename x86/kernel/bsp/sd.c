#include <dev/sd.h>
#include <kstring.h>
#include <stdint.h>
#include <stddef.h>
#include <mm/mmu.h>
#include "arch.h"
#include "x86_bootinfo.h"

/* ------------------------------------------------------------------
 * x86 内核存储 BSP: 内存盘 rootfs (UEFI 引导 ROOTFS.IMG, bootinfo v3)
 * 最优先, 其后 legacy ATA PIO (0x1F0, pc 机型), 探测失败时回退
 * AHCI DMA (class 0106, q35/现代 x86)。
 *
 * AHCI 的 ABAR 由 hw_info_arch.c 在启动最早阶段探测并映射:
 *   boot_pml4 与内核页表均在 VA 0xC0000000 (+ ABAR 页内偏移) 可见,
 *   因此根文件系统读取在"重映射内核 VM"前后都能工作。
 * 全轮询实现, 与 ATA 路径一致, 不依赖中断。
 * ------------------------------------------------------------------ */

/* legacy IDE: primary(0x1F0)/secondary(0x170) × master/slave 四个位置,
 * sd_init 按此顺序探测, 首个存在数据盘的位置作为根盘 */
#define ATA_MAX_POS      4

/* ---- 内存盘 rootfs (RAM 中的一份 ext3 镜像, 只读为实/写落 RAM) ---- */
static int32_t _rd_active = 0;

/* 区间内偏移对应内核 VA; 越界返回 NULL。
 * 映射窗口自 2MB 对齐的 pstart (rd_base 向下圆整) 起, 故 rd_base 的
 * 2MB 内偏移须计入 (stub 已保证 2MB 对齐, 这里兜底非对齐 bootinfo) */
static void *_rd_ptr(int64_t sector, uint32_t count) {
    uint64_t off = (uint64_t)sector * 512;
    uint64_t end = off + (uint64_t)count * 512;
    if (x86_rd_size == 0 || end > x86_rd_size) {
        return NULL;
    }
    return (void *)(uintptr_t)X86_RAMDISK_VA((x86_rd_base & 0x1FFFFF) + off);
}
static const uint16_t _ata_bases[ATA_MAX_POS / 2] = { 0x1F0, 0x170 };
static uint16_t _ata_base = 0x1F0;      /* 选中位置的任务文件基址 */
static uint16_t _ata_ctl = 0x3F6;       /* 对应控制寄存器 */
static uint8_t _ata_drvsel = 0xA0;      /* 0xA0=master 0xB0=slave */
static int32_t _ata_active = 0;

#define ATA_DATA        0x00
#define ATA_SECCOUNT0   0x02
#define ATA_LBA0        0x03
#define ATA_LBA1        0x04
#define ATA_LBA2        0x05
#define ATA_HDDEVSEL    0x06
#define ATA_COMMAND     0x07
#define ATA_STATUS      0x07

#define ATA_CMD_READ_PIO_EXT  0x24
#define ATA_CMD_WRITE_PIO_EXT 0x34
#define ATA_CMD_CACHE_FLUSH   0xE7
#define ATA_CMD_IDENTIFY      0xEC

#define ATA_SR_BSY 0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DRQ 0x08
#define ATA_SR_DF  0x20
#define ATA_SR_ERR 0x01

/* ---- AHCI ---- */
#define X86_PCI_MMIO_VA   0xC0000000ULL

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

/* DMA 结构区: 链接脚本在内核镜像内保留 4KB(_ahci_dma_base, 页对齐),
 * 重映射后的内核页表可直访其 VA; 寄存器用 V2P 后的物理地址(32-bit,
 * 内核镜像 < 4GB, AHCI 32-bit DMA 可达)。CLB 1024B/CTBA 128B 对齐
 * 由页对齐起点保证。 */
extern uint8_t _ahci_dma_base[];
#define AHCI_CLB_VA   ((uintptr_t)_ahci_dma_base)
#define AHCI_FB_VA    (AHCI_CLB_VA + 0x400)
#define AHCI_CT_VA    (AHCI_CLB_VA + 0x800)
#define AHCI_BUF_VA   (AHCI_CLB_VA + 0x900)

#define AHCI_MAX_SECTORS 8

typedef struct {
    uint32_t opts;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved[4];
} ahci_cmd_hdr_t;

typedef struct {
    uint32_t dba, dbau, reserved;
    uint32_t dbc_i;
} ahci_prdt_t;

extern uint64_t x86_ahci_abar_phys;

static volatile uint8_t *_hba = NULL;
static uint8_t _ahci_port = 0;
static int32_t _ahci_active = 0;

static volatile uint32_t *ahci_preg(uint8_t port, uint32_t off) {
    return (volatile uint32_t *)(_hba + PORT_BASE + (uint32_t)port * PORT_SZ + off);
}

static void ahci_delay(void) {
    for (volatile int i = 0; i < 50; i++) {
        __asm__ volatile("pause");
    }
}

static void ahci_memset_va(uintptr_t va, uint32_t size) {
    volatile uint8_t *p = (volatile uint8_t *)va;
    while (size--) {
        *p++ = 0;
    }
}

static int32_t ahci_init(void) {
    volatile uint32_t *ghc;
    uint32_t pi;

    if (_ahci_active) {
        return 0;
    }
    if (x86_ahci_abar_phys == 0) {
        return -1;
    }
    _hba = (volatile uint8_t *)(uintptr_t)(X86_PCI_MMIO_VA +
            (x86_ahci_abar_phys & 0x3FFFFFULL));

    ghc = (volatile uint32_t *)(_hba + HBA_GHC);
    *ghc = GHC_HR;
    for (int i = 0; i < 100000 && (*ghc & GHC_HR); i++) {
        ahci_delay();
    }
    *ghc = GHC_AE;
    ahci_memset_va(AHCI_CLB_VA, AHCI_BUF_VA + 512 - AHCI_CLB_VA);
    pi = *(volatile uint32_t *)(_hba + HBA_PI);
    for (uint8_t p = 0; p < 32; p++) {
        if (!(pi & (1u << p))) {
            continue;
        }
        if ((*ahci_preg(p, P_SSTS) & SSTS_DET) != SSTS_DEV_PRESENT) {
            continue;
        }
        uint32_t cmd = *ahci_preg(p, P_CMD);
        *ahci_preg(p, P_CMD) = cmd & ~(CMD_ST | CMD_FRE);
        for (int i = 0; i < 100000 && (*ahci_preg(p, P_CMD) & (CMD_ST | CMD_FRE)); i++) {
            ahci_delay();
        }
        *ahci_preg(p, P_CLB) = (uint32_t)V2P(AHCI_CLB_VA);
        *ahci_preg(p, P_CLBU) = 0;
        *ahci_preg(p, P_FB) = (uint32_t)V2P(AHCI_FB_VA);
        *ahci_preg(p, P_FBU) = 0;
        cmd = *ahci_preg(p, P_CMD);
        *ahci_preg(p, P_CMD) = cmd | CMD_FRE | CMD_ST;
        for (int i = 0; i < 100000 && !(*ahci_preg(p, P_CMD) & CMD_ST); i++) {
            ahci_delay();
        }
        _ahci_port = p;
        _ahci_active = 1;
        return 0;
    }
    _hba = NULL;
    return -1;
}

static int32_t ahci_submit(uint8_t cmd, uint64_t lba, uint16_t count,
                           uintptr_t buf_va, uint32_t nbytes, int write) {
    volatile ahci_cmd_hdr_t *hdr = (volatile ahci_cmd_hdr_t *)AHCI_CLB_VA;
    volatile uint8_t *ct = (volatile uint8_t *)AHCI_CT_VA;
    volatile ahci_prdt_t *prdt = (volatile ahci_prdt_t *)(AHCI_CT_VA + 0x80);
    uint32_t buf_pa = (uint32_t)V2P(buf_va);

    hdr->opts = (5 & 0x1F) | (write ? (1u << 6) : 0) | (1u << 16);
    hdr->prdbc = 0;
    hdr->ctba = (uint32_t)V2P(AHCI_CT_VA);
    hdr->ctbau = 0;

    for (uint32_t i = 0; i < 0x80; i++) {
        ct[i] = 0;
    }
    ct[0] = 0x27;
    ct[1] = 0x80;
    ct[2] = cmd;
    ct[4]  = (uint8_t)(lba);
    ct[5]  = (uint8_t)(lba >> 8);
    ct[6]  = (uint8_t)(lba >> 16);
    ct[7]  = 0x40;
    ct[8]  = (uint8_t)(lba >> 24);
    ct[9]  = (uint8_t)(lba >> 32);
    ct[10] = (uint8_t)(lba >> 40);
    ct[12] = (uint8_t)(count & 0xFF);
    ct[13] = (uint8_t)(count >> 8);
    ct[15] = 1;

    prdt->dba = buf_pa;
    prdt->dbau = 0;
    prdt->reserved = 0;
    prdt->dbc_i = (nbytes - 1) | (1u << 31);

    __asm__ volatile("mfence" ::: "memory");
    *ahci_preg(_ahci_port, P_IS) = 0xFFFFFFFF;
    *ahci_preg(_ahci_port, P_CI) = 1;

    for (uint64_t spin = 0; spin < 20000000ull; spin++) {
        if (!(*ahci_preg(_ahci_port, P_CI) & 1)) {
            for (uint64_t w = 0; w < 200000ull; w++) {
                if (*ahci_preg(_ahci_port, P_IS) & 1) {
                    break;
                }
            }
            *ahci_preg(_ahci_port, P_IS) = 0xFFFFFFFF;
            return 0;
        }
    }

    {
        uint32_t c = *ahci_preg(_ahci_port, P_CMD);
        *ahci_preg(_ahci_port, P_CMD) = c & ~(CMD_ST | CMD_FRE);
        while (*ahci_preg(_ahci_port, P_CMD) & (CMD_ST | (1u << 14))) {
            ahci_delay();
        }
        *ahci_preg(_ahci_port, P_IS) = 0xFFFFFFFF;
        *ahci_preg(_ahci_port, P_CMD) = c | (CMD_ST | CMD_FRE);
    }
    return -1;
}

static int32_t ahci_read_sectors(int64_t lba, uint32_t count, void *buf) {
    uint8_t *p = (uint8_t *)buf;
    while (count) {
        uint32_t n = count > AHCI_MAX_SECTORS ? AHCI_MAX_SECTORS : count;
        int32_t r = (lba == 0 && n == 1)
                ? ahci_submit(0xC8, (uint64_t)lba, 1, AHCI_BUF_VA, 512, 0)
                : ahci_submit(0x25, (uint64_t)lba, (uint16_t)n, AHCI_BUF_VA, n * 512u, 0);
        if (r != 0) {
            return -1;
        }
        memcpy(p, (const void *)AHCI_BUF_VA, (size_t)n * 512);
        p += n * 512;
        lba += n;
        count -= n;
    }
    return 0;
}

static int32_t ahci_write_sectors(int64_t lba, uint32_t count, const void *buf) {
    const uint8_t *p = (const uint8_t *)buf;
    while (count) {
        uint32_t n = count > AHCI_MAX_SECTORS ? AHCI_MAX_SECTORS : count;
        memcpy((void *)AHCI_BUF_VA, p, (size_t)n * 512);
        __asm__ volatile("mfence" ::: "memory");
        if (ahci_submit(0x35, (uint64_t)lba, (uint16_t)n, AHCI_BUF_VA, n * 512u, 1) != 0) {
            return -1;
        }
        p += n * 512;
        lba += n;
        count -= n;
    }
    return 0;
}

/* ---- NVMe (第三级回退: IDE -> AHCI -> NVMe) ----
 * 供 NVMe-only 机器(无 IDE/AHCI)引导。寄存器窗口 VA 0xE0000000 由
 * hw_info_arch 早期建立(boot_pml4 备用 PD entry 256 + kernel_vm arch_vm);
 * 队列/缓冲位于内核镜像保留区 _nvme_dma_base(页对齐, V2P 后 <4GB 可 DMA)。
 * 队列深度 4, 轮询完成队列; CQE dw3 = CID[15:0] | P[16] | SF[31:17]。 */
extern uint64_t x86_nvme_bar_phys;
extern uint8_t _nvme_dma_base[];

#define NVME_REG_CAP   0x00
#define NVME_REG_VS    0x08
#define NVME_REG_CC    0x14
#define NVME_REG_CSTS  0x1C
#define NVME_REG_AQA   0x24
#define NVME_REG_ASQ   0x28
#define NVME_REG_ACQ   0x30

#define NVME_CC_EN     (1u << 0)
#define NVME_CC_IOSQES (6u << 16)
#define NVME_CC_IOCQES (4u << 20)
#define NVME_CSTS_RDY  (1u << 0)
#define NVME_CSTS_CFS  (1u << 1)

#define NVME_IO_READ          0x02
#define NVME_ADM_CREATE_IO_SQ 0x01
#define NVME_ADM_CREATE_IO_CQ 0x05
#define NVME_ADM_IDENTIFY     0x06

#define NVME_AQ  4
#define NVME_IQ  4

#define NQ_ASQ    0
#define NQ_ACQ    0x1000
#define NQ_IOSQ   0x2000
#define NQ_IOCQ   0x3000
#define NQ_IDBUF  0x4000
#define NQ_DATA   0x5000

typedef struct {
    uint8_t opcode;
    uint8_t flags;
    uint16_t cid;
    uint32_t nsid;
    uint32_t res0[2];
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} nvme_sqe_t;

typedef struct {
    uint32_t result;
    uint32_t res1;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;      /* [15:3]=SF [2:1]=SC? — QEMU: SF=(status>>1)&0x7fff,
                             P=status bit0 — 实测按 userspace 修正: dw3 =
                             cid | (SF<<17) | (P<<16) — 此处 status 16 位含 P/SF */
} nvme_cqe_t;

static volatile uint8_t *_nvregs = NULL;
static int32_t _nvme_active = 0;
static uint16_t _nvme_cid = 1;
static uint64_t _nvme_nsze = 0;
static uint16_t _asq_tail, _acq_head, _acq_phase = 1;
static uint16_t _iosq_tail, _iocq_head, _iocq_phase = 1;

static inline uint32_t nv_read(uint32_t off) {
    return *(volatile uint32_t *)(_nvregs + off);
}
static inline void nv_write(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(_nvregs + off) = v;
}
static inline void nv_write64(uint32_t off, uint64_t v) {
    nv_write(off, (uint32_t)v);
    nv_write(off + 4, (uint32_t)(v >> 32));
}

/* 完成队列消费: 返回 0 成功, status 非零失败, -1 超时 */
static int32_t nv_poll_cq(volatile nvme_cqe_t *cq, uint16_t *head,
        uint16_t *phase, uint32_t dbl, uint16_t *sf_out) {
    for (uint64_t spin = 0; spin < 40000000ull; ++spin) {
        uint32_t dw3 = *(volatile uint32_t *)&cq[*head].status;
        /* dw3 = cid[15:0] | P[16] | SF[31:17] */
        uint16_t p = (uint16_t)((dw3 >> 16) & 1);
        if (p == *phase) {
            uint16_t sf = (uint16_t)((dw3 >> 17) & 0x7FFF);
            *head = (uint16_t)((*head + 1) & 3);
            if (*head == 0) *phase ^= 1;
            nv_write(dbl, *head);
            if (sf_out) *sf_out = sf;
            return sf ? (int32_t)sf : 0;
        }
    }
    return -1;
}

/* VA→物理 (内核镜像内地址; V2P 对 <16MB 保留区语义正确) */
static uint64_t nv_va_phys(uintptr_t va) {
    return (uint64_t)V2P((ewokos_addr_t)va);
}

static int32_t nv_admin_submit(nvme_sqe_t *cmd, uint16_t *sf_out) {
    volatile nvme_sqe_t *sq = (volatile nvme_sqe_t *)(_nvme_dma_base + NQ_ASQ);
    volatile nvme_cqe_t *cq = (volatile nvme_cqe_t *)(_nvme_dma_base + NQ_ACQ);
    uint16_t sf = 0;
    int32_t r;

    sq[_asq_tail] = *cmd;
    __asm__ volatile("mfence" ::: "memory");
    nv_write(0x1000, _asq_tail);                 /* ASQ doorbell */
    _asq_tail = (uint16_t)((_asq_tail + 1) & (NVME_AQ - 1));
    r = nv_poll_cq(cq, &_acq_head, &_acq_phase, 0x1004, &sf);
    if (sf_out) *sf_out = sf;
    return r;
}

static int32_t nv_io_submit(nvme_sqe_t *cmd) {
    volatile nvme_sqe_t *sq = (volatile nvme_sqe_t *)(_nvme_dma_base + NQ_IOSQ);
    volatile nvme_cqe_t *cq = (volatile nvme_cqe_t *)(_nvme_dma_base + NQ_IOCQ);
    uint16_t sf = 0;
    int32_t r;

    sq[_iosq_tail] = *cmd;
    __asm__ volatile("mfence" ::: "memory");
    nv_write(0x1008, _iosq_tail);                /* IOSQ doorbell (qid=1) */
    _iosq_tail = (uint16_t)((_iosq_tail + 1) & (NVME_IQ - 1));
    r = nv_poll_cq(cq, &_iocq_head, &_iocq_phase, 0x100C, &sf);
    return r ? -1 : 0;
}

static int32_t nvme_init(void) {
    uint64_t cap;
    uint32_t to_spins;
    nvme_sqe_t c;
    uint16_t sf = 0;
    uint64_t qphys;

    if (_nvme_active) return 0;
    if (x86_nvme_bar_phys == 0) {
        printf("sd: no nvme bar (kernel fallback abort)\n");
        return -1;
    }

    _nvregs = (volatile uint8_t *)0xE0000000UL;

    cap = *(volatile uint64_t *)(_nvregs + NVME_REG_CAP);
    if ((cap & 0xFF) == 0) {                     /* MQES=0 异常 */
        printf("sd: nvme cap=0\n");
        return -1;
    }

    qphys = nv_va_phys((uintptr_t)_nvme_dma_base);

    /* 控制器复位 → 使能 */
    nv_write(NVME_REG_CC, 0);
    for (uint64_t s = 0; s < 5000000ull; ++s) {
        if (!(nv_read(NVME_REG_CSTS) & NVME_CSTS_RDY)) break;
    }
    if (nv_read(NVME_REG_CSTS) & NVME_CSTS_CFS) {
        printf("sd: nvme cfs after disable\n");
        return -1;
    }
    nv_write(NVME_REG_AQA, ((NVME_AQ - 1) & 0xFFF) |
            (((NVME_AQ - 1) & 0xFFF) << 16));
    nv_write64(NVME_REG_ASQ, qphys + NQ_ASQ);
    nv_write64(NVME_REG_ACQ, qphys + NQ_ACQ);
    nv_write(NVME_REG_CC, NVME_CC_EN | NVME_CC_IOSQES | NVME_CC_IOCQES);
    to_spins = 50000000ull;
    for (uint64_t s = 0; s < to_spins; ++s) {
        if (nv_read(NVME_REG_CSTS) & NVME_CSTS_RDY) break;
    }
    if (!(nv_read(NVME_REG_CSTS) & NVME_CSTS_RDY)) {
        printf("sd: nvme enable timeout\n");
        return -1;
    }

    /* IDENTIFY namespace 1 → NSZE */
    memset((void *)_nvme_dma_base + NQ_IDBUF, 0, 4096);
    memset(&c, 0, sizeof(c));
    c.opcode = NVME_ADM_IDENTIFY;
    c.cid = _nvme_cid++;
    c.nsid = 1;
    c.prp1 = qphys + NQ_IDBUF;
    c.cdw10 = 0;                                  /* CNS=0: identify ns */
    if (nv_admin_submit(&c, &sf) != 0) {
        printf("sd: nvme identify failed sf=%x\n", sf);
        return -1;
    }
    {
        const uint32_t *id = (const uint32_t *)(_nvme_dma_base + NQ_IDBUF);
        _nvme_nsze = (uint64_t)id[8] | ((uint64_t)id[9] << 32);
    }
    if (_nvme_nsze == 0) {
        printf("sd: nvme nsze=0\n");
        return -1;
    }

    /* 创建 IO CQ(qid=1) 与 IO SQ(qid=1, 关联 CQ1) */
    memset(&c, 0, sizeof(c));
    c.opcode = NVME_ADM_CREATE_IO_CQ;
    c.cid = _nvme_cid++;
    c.prp1 = qphys + NQ_IOCQ;
    c.cdw10 = 1;                                  /* CQID */
    c.cdw11 = (NVME_IQ - 1);                      /* 大小-1 */
    if (nv_admin_submit(&c, &sf) != 0) {
        printf("sd: nvme create cq failed sf=%x\n", sf);
        return -1;
    }
    memset(&c, 0, sizeof(c));
    c.opcode = NVME_ADM_CREATE_IO_SQ;
    c.cid = _nvme_cid++;
    c.prp1 = qphys + NQ_IOSQ;
    c.cdw10 = 1;                                  /* SQID */
    c.cdw11 = (NVME_IQ - 1) | (1u << 16);         /* 大小-1 | 关联 CQID=1 */
    if (nv_admin_submit(&c, &sf) != 0) {
        printf("sd: nvme create sq failed sf=%x\n", sf);
        return -1;
    }

    _nvme_active = 1;
    printf("sd: nvme ready nsze=%llu\n", (unsigned long long)_nvme_nsze);
    (void)cap;
    return 0;
}

static int32_t nvme_rw(uint64_t lba, uint32_t count, void *buf, int32_t write) {
    nvme_sqe_t c;
    uint8_t *dma = _nvme_dma_base + NQ_DATA;
    uint64_t dphys = nv_va_phys((uintptr_t)dma);
    uint8_t *p = (uint8_t *)buf;

    if (!_nvme_active || count == 0 || count > 8) {
        return -1;
    }
    while (count) {
        uint32_t n = count;
        memset(&c, 0, sizeof(c));
        c.opcode = write ? 0x01 : NVME_IO_READ;
        c.cid = _nvme_cid++;
        c.nsid = 1;
        c.prp1 = dphys;
        c.cdw10 = (uint32_t)lba;
        c.cdw11 = (uint32_t)(lba >> 32);
        c.cdw12 = n - 1;
        if (write) {
            memcpy(dma, p, n * 512);
            __asm__ volatile("mfence" ::: "memory");
        }
        if (nv_io_submit(&c) != 0) {
            return -1;
        }
        if (!write) {
            memcpy(p, dma, n * 512);
        }
        p += n * 512;
        lba += n;
        count -= n;
    }
    return 0;
}

static int32_t nvme_read_sectors(int64_t lba, uint32_t count, void *buf) {
    return nvme_rw((uint64_t)lba, count, buf, 0);
}

static int32_t nvme_write_sectors(int64_t lba, uint32_t count, const void *buf) {
    return nvme_rw((uint64_t)lba, count, (void *)buf, 1);
}

/* ---- legacy ATA PIO 路径 (IDE: 全位置探测 + LBA48) ---- */
static int32_t _pending_sector = -1;
static uint8_t _write_buf[512];

static uint16_t ata_ctl_of(uint16_t base) {
    return (uint16_t)(base + 0x206);   /* 0x1F0->0x3F6, 0x170->0x376 */
}

static void ata_400ns_wait(uint16_t ctl) {
    for (int i = 0; i < 4; ++i) {
        (void)inb(ctl);
    }
}

static int ata_wait_ready(uint16_t base) {
    for (int i = 0; i < 1000000; ++i) {
        uint8_t status = inb(base + ATA_STATUS);
        if (status == 0) {
            continue;
        }
        if (status == 0xFF) {
            return -1;  /* 浮空总线: 无 legacy IDE (q35) */
        }
        if ((status & ATA_SR_BSY) == 0 &&
                (status & (ATA_SR_ERR | ATA_SR_DF)) == 0) {
            return 0;
        }
        if ((status & (ATA_SR_ERR | ATA_SR_DF)) != 0) {
            return -1;
        }
    }
    return -1;
}

static int ata_wait_drq(uint16_t base) {
    for (int i = 0; i < 1000000; ++i) {
        uint8_t status = inb(base + ATA_STATUS);
        if (status == 0) {
            continue;
        }
        if ((status & ATA_SR_BSY) == 0 && (status & ATA_SR_DRQ) != 0) {
            return 0;
        }
        if (status & (ATA_SR_ERR | ATA_SR_DF)) {
            return -1;
        }
    }
    return -1;
}

/* 通道软复位 (SRST) */
static void ata_channel_reset(uint16_t base) {
    uint16_t ctl = ata_ctl_of(base);
    outb(ctl, 0x04);
    ata_400ns_wait(ctl);
    outb(ctl, 0x00);
    ata_400ns_wait(ctl);
    for (int i = 0; i < 200000; ++i) {
        uint8_t st = inb(base + ATA_STATUS);
        if (st == 0xFF || (st & ATA_SR_BSY) == 0) {
            break;
        }
    }
}

/* IDENTIFY DEVICE: 判定该位置是否存在 ATA 盘 (ATAPI/浮空 → 不存在) */
static int ata_identify(uint16_t base, uint8_t drvsel) {
    uint16_t id[256];
    outb(base + ATA_HDDEVSEL, drvsel);
    ata_400ns_wait(ata_ctl_of(base));
    outb(base + ATA_SECCOUNT0, 0);
    outb(base + ATA_LBA0, 0);
    outb(base + ATA_LBA1, 0);
    outb(base + ATA_LBA2, 0);
    outb(base + ATA_COMMAND, ATA_CMD_IDENTIFY);
    for (int i = 0; i < 200000; ++i) {
        uint8_t st = inb(base + ATA_STATUS);
        if (st == 0xFF) {
            return -1;
        }
        if ((st & ATA_SR_BSY) == 0) {
            if (st & ATA_SR_ERR) {
                return -1;              /* ATAPI 等 */
            }
            if (st & ATA_SR_DRQ) {
                for (int w = 0; w < 256; ++w) {
                    id[w] = inw(base + ATA_DATA);
                }
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
    outb(b + ATA_SECCOUNT0, 0);
    outb(b + ATA_SECCOUNT0, (uint8_t)nsect);
    outb(b + ATA_LBA0, (uint8_t)(((uint32_t)sector >> 24) & 0xFF));
    outb(b + ATA_LBA1, 0);                       /* LBA[39:32] */
    outb(b + ATA_LBA2, 0);                       /* LBA[47:40] */
    outb(b + ATA_LBA0, (uint8_t)((uint32_t)sector & 0xFF));
    outb(b + ATA_LBA1, (uint8_t)(((uint32_t)sector >> 8) & 0xFF));
    outb(b + ATA_LBA2, (uint8_t)(((uint32_t)sector >> 16) & 0xFF));
    outb(b + ATA_HDDEVSEL, (uint8_t)(0x40 | _ata_drvsel));
    ata_400ns_wait(_ata_ctl);
}

int32_t sd_init(void) {
    /* 内存盘 rootfs 最优先: bootinfo v3 携带 ROOTFS.IMG 即视为权威根设备
     * (ISO/Ventoy 引导时无 ATA/AHCI/NVMe 根盘) */
    if (x86_rd_size != 0) {
        _rd_active = 1;
        printf("sd: ramdisk %dMB@%llx active\n",
                (int32_t)(x86_rd_size / MB), (unsigned long long)x86_rd_base);
        return 0;
    }
    /* legacy IDE 四位置探测: pri-mas, pri-sla, sec-mas, sec-sla;
     * 首个存在数据盘的位置作为根盘。失败回退 AHCI (q35/现代 x86)。 */
    for (uint32_t pos = 0; pos < ATA_MAX_POS; ++pos) {
        uint16_t base = _ata_bases[pos / 2];
        uint8_t drv = (pos % 2) ? 0xB0 : 0xA0;
        if ((pos % 2) == 0) {
            ata_channel_reset(base);
        }
        if (ata_identify(base, drv) == 0) {
            _ata_base = base;
            _ata_ctl = ata_ctl_of(base);
            _ata_drvsel = drv;
            _ata_active = 1;
            printf("sd: ide pos=%u base=%x drv=%x\n", pos, base, drv);
            return 0;
        }
    }
    if (ahci_init() == 0) {
        return 0;
    }
    if (nvme_init() == 0) {
        printf("sd: nvme active (kernel)\n");
        return 0;
    }
    return -1;
}

int32_t sd_dev_read(int32_t sector) {
    _pending_sector = sector;
    return 0;
}

int32_t sd_dev_read_done(void* buf) {
    if (_pending_sector < 0) {
        return -1;
    }
    if (_rd_active) {
        void *src = _rd_ptr(_pending_sector, 1);
        _pending_sector = -1;
        if (src == NULL) {
            return -1;
        }
        memcpy(buf, src, 512);
        return 0;
    }
    if (_ahci_active) {
        int32_t r = ahci_read_sectors(_pending_sector, 1, buf);
        _pending_sector = -1;
        return r;
    }
    if (_nvme_active) {
        int32_t r = nvme_read_sectors(_pending_sector, 1, buf);
        _pending_sector = -1;
        return r;
    }
    ata_tf_lba48(_pending_sector, 1);
    outb(_ata_base + ATA_COMMAND, ATA_CMD_READ_PIO_EXT);
    ata_400ns_wait(_ata_ctl);
    ata_400ns_wait(_ata_ctl);
    if (ata_wait_drq(_ata_base) != 0) {
        return -1;
    }
    for (int i = 0; i < 256; ++i) {
        ((uint16_t*)buf)[i] = inw(_ata_base + ATA_DATA);
    }
    ata_400ns_wait(_ata_ctl);
    _pending_sector = -1;
    return 0;
}

int32_t sd_dev_write(int32_t sector, const void* buf) {
    _pending_sector = sector;
    memcpy(_write_buf, buf, sizeof(_write_buf));
    return 0;
}

int32_t sd_dev_write_done(void) {
    if (_pending_sector < 0) {
        return -1;
    }
    if (_rd_active) {
        void *dst = _rd_ptr(_pending_sector, 1);
        _pending_sector = -1;
        if (dst == NULL) {
            return -1;
        }
        memcpy(dst, _write_buf, sizeof(_write_buf));
        return 0;   /* 写落 RAM, 重启即失 */
    }
    if (_ahci_active) {
        int32_t r = ahci_write_sectors(_pending_sector, 1, _write_buf);
        _pending_sector = -1;
        return r;
    }
    if (_nvme_active) {
        int32_t r = nvme_write_sectors(_pending_sector, 1, _write_buf);
        _pending_sector = -1;
        return r;
    }
    if (ata_wait_ready(_ata_base) != 0) {
        return -1;
    }
    ata_tf_lba48(_pending_sector, 1);
    outb(_ata_base + ATA_COMMAND, ATA_CMD_WRITE_PIO_EXT);
    ata_400ns_wait(_ata_ctl);
    if (ata_wait_drq(_ata_base) != 0) {
        return -1;
    }
    for (int i = 0; i < 256; ++i) {
        outw(_ata_base + ATA_DATA, ((uint16_t*)_write_buf)[i]);
    }
    ata_400ns_wait(_ata_ctl);
    outb(_ata_base + ATA_COMMAND, ATA_CMD_CACHE_FLUSH);
    if (ata_wait_ready(_ata_base) != 0) {
        return -1;
    }
    _pending_sector = -1;
    return 0;
}

/* 多扇区读 (ext3read 的可选优化入口; ramdisk 即 memcpy) */
int32_t sd_dev_read_blocks(int32_t sector, void* buf, uint32_t count) {
    if (count == 0) {
        return 0;
    }
    if (_rd_active) {
        void *src = _rd_ptr(sector, count);
        if (src == NULL) {
            return -1;
        }
        memcpy(buf, src, (size_t)count * 512);
        return 0;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (sd_dev_read(sector + (int32_t)i) != 0) {
            return -1;
        }
        if (sd_dev_read_done((uint8_t*)buf + (size_t)i * 512) != 0) {
            return -1;
        }
    }
    return 0;
}

void sd_dev_handle(void) {
}
