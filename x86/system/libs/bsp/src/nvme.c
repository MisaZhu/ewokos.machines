/*
 * x86 用户态 NVMe HCD。
 *
 * PCI(class 0108) -> BAR0 经 SYS_MEM_MAP 映射 -> admin/IO 队列对(dma 池)
 * -> 轮询完成队列。实现: 控制器使能、IDENTIFY(控制器/namespace)、
 * CREATE_IO_SQ/CQ、READ/WRITE(PRP 单页, 每命令最多 8 扇区)。
 */

#include <bsp/bsp_nvme.h>
#include <bsp/x86_pio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <sysinfo.h>
#include <ewoksys/syscall.h>
#include <ewoksys/mmio.h>
#include <ewoksys/dma.h>
#include <ewoksys/klog.h>
#include <ewoksys/proc.h>

static int32_t bsp_nvme_controller_init(void);

/* ---- PCI 配置空间 ---- */
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

/* ---- NVMe 控制器寄存器 (stride = 1 << CAP.DSTRD) ---- */
#define NVME_CAP   0x00
#define NVME_VS    0x08
#define NVME_CC    0x14
#define NVME_CSTS  0x1C
#define NVME_AQA   0x24
#define NVME_ASQ   0x28
#define NVME_ACQ   0x30

#define CAP_MQES(x)   ((x) & 0xFFFF)
#define CAP_DSTRD(x)  (((x) >> 32) & 0xF)
#define CC_EN     (1u << 0)
#define CC_IOSQES (6u << 16)   /* SQ entry 64B -> log2 = 6 */
#define CC_IOCQES (4u << 20)   /* CQ entry 16B -> log2 = 4 */
#define CSTS_RDY  (1u << 0)

#define ASQ_SIZE  4    /* admin SQ: 4 entries (初始化流程不回绕) */
#define ACQ_SIZE  4    /* admin CQ: 4 entries */
#define IOQ_SIZE  8    /* IO SQ/CQ: 8 entries */

/* admin 命令码 */
#define NVME_CQE_DEBUG 1

#define NVME_ADM_DEL_IO_SQ   0x00
#define NVME_ADM_CREATE_IO_SQ 0x01
#define NVME_ADM_CREATE_IO_CQ 0x05
#define NVME_ADM_IDENTIFY    0x06

/* IO 命令码 */
#define NVME_IO_WRITE 0x01
#define NVME_IO_READ  0x02

typedef struct {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t cid;
    uint32_t nsid;
    uint32_t reserved0[2];
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} nvme_cmd_t;   /* 64B */

typedef struct {
    uint32_t dw0, dw1, dw2, dw3;
} nvme_cq_entry_t;  /* 16B, dw3: P(bit16) SF>>17 */

typedef struct {
    volatile uint8_t *regs;
    uint32_t stride;

    uint8_t *qmem;           /* 队列内存 (dma 池) */
    ewokos_addr_t qmem_phys;

    /* admin */
    nvme_cmd_t *asq;
    nvme_cq_entry_t *acq;
    uint32_t asq_tail, acq_head;
    uint16_t asq_phase, acq_phase;

    /* IO */
    nvme_cmd_t *iosq;
    nvme_cq_entry_t *iocq;
    uint32_t iosq_tail, iocq_head;
    uint16_t iosq_phase, iocq_phase;

    uint64_t nsze;           /* namespace 大小(块) */
    uint8_t  lba_shift;
    bool     ready;
} nvme_state_t;

static nvme_state_t _nv;

#define QMEM_SIZE (128 * 1024)
/* 布局(队列结构必须按 MPS 页对齐, 偏移由 mps_min 动态计算):
   ASQ | page | ACQ | page | IOSQ | page | IOCQ | page | IDBUF(4K) | DATA(4K) */
#define ASQ_OFF   0
#define ACQ_OFF   (qmem_page)
#define IOSQ_OFF  (qmem_page * 2)
#define IOCQ_OFF  (qmem_page * 3)
#define IDBUF_OFF (qmem_page * 4)
#define DATA_OFF  (qmem_page * 4 + 4096)
#define DATA_SIZE 4096
static uint32_t qmem_page = 4096;

/* BAR0 映射 VA: 用户地址空间(< 0x80000000)内的空闲位置 */
#define NVME_BAR_VA   0x51000000

static inline uint32_t nvreg_read(uint32_t off) {
    return get32((ewokos_addr_t)(_nv.regs + off));
}
static inline void nvreg_write(uint32_t off, uint32_t val) {
    put32((ewokos_addr_t)(_nv.regs + off), val);
}
static inline uint64_t nvreg_read64(uint32_t off) {
    uint64_t lo = nvreg_read(off);
    uint64_t hi = nvreg_read(off + 4);
    return lo | (hi << 32);
}
static inline void nvreg_write64(uint32_t off, uint64_t v) {
    nvreg_write(off, (uint32_t)v);
    nvreg_write(off + 4, (uint32_t)(v >> 32));
}
/* doorbell: SQ0TDBL = 0x1000 + qid*2*stride; CQ0IHBL = 0x1000 + (qid*2+1)*stride */
static inline void sq_doorbell(uint32_t qid, uint32_t tail) {
    ewokos_addr_t dba = (ewokos_addr_t)(_nv.regs + 0x1000 + (qid * 2) * _nv.stride);
    put32(dba, tail);
}
static inline void cq_doorbell(uint32_t qid, uint32_t head) {
    nvreg_write(0x1000 + (qid * 2 + 1) * _nv.stride, head);
}

static void spin(void) {
    for (volatile int i = 0; i < 1000; i++) {
        __asm__ volatile("pause");
    }
}

/* 提交一条 admin 命令并轮询完成; 返回 0 = 成功(status==0) */
static int admin_submit(nvme_cmd_t *cmd, uint16_t *status_out) {
    uint32_t my_tail = _nv.asq_tail;
    nvme_cq_entry_t *cq = _nv.acq;

    _nv.asq[my_tail] = *cmd;
#if NVME_CQE_DEBUG
    {
        const uint32_t *w = (const uint32_t *)cmd;
        klog("nvmefsd: submit cid=%u op=%x w10=%x w11=%x w12=%x w13=%x\n",
             cmd->cid, cmd->opcode, w[10], w[11], w[12], w[13]);
    }
#endif
    __asm__ volatile("mfence" ::: "memory");
    _nv.asq_tail = (my_tail + 1) % ASQ_SIZE;
    sq_doorbell(0, _nv.asq_tail);

    /* 轮询完成队列 phase 位; TCG(单线程)下设备完成经 QEMU 主循环
     * 的 bottom-half 投递, 必须周期性让出 vCPU(proc_usleep -> 内核
     * hlt) BH 才能得到执行 */
    for (uint64_t spins = 0; spins < 3000ull; spins++) {
        nvme_cq_entry_t *e = &cq[_nv.acq_head];
        uint16_t p = (e->dw3 >> 16) & 1;
        if ((spins & 0x3F) == 0x3F) {
            proc_usleep(500);
        }
        if (p == _nv.acq_phase) {
            /* dw3: [15:0]=CID [16]=P(phase) [31:17]=SF(status) */
            uint16_t st = (e->dw3 >> 17) & 0x7FFF;
            uint32_t cid = e->dw3 & 0xFFFF;
#if NVME_CQE_DEBUG
            klog("nvmefsd: cqe dw2=%x dw3=%x st=%x cid=%u\n",
                 e->dw2, e->dw3, st, cid);
#endif
            _nv.acq_head = (_nv.acq_head + 1) % ACQ_SIZE;
            if (_nv.acq_head == 0) {
                _nv.acq_phase ^= 1;
            }
            cq_doorbell(0, _nv.acq_head);
            (void)cid;
            if (status_out) {
                *status_out = st;
            }
            return st ? -1 : 0;
        }
    }
    klog("nvmefsd: admin cmd %x timeout csts=%x cc=%x cq_dw3=%x\n",
         cmd->opcode, nvreg_read(NVME_CSTS), nvreg_read(NVME_CC),
         cq[_nv.acq_head].dw3);
    return -1;
}

/* IO 命令: 提交后轮询 IO 完成队列 */
static int io_submit(nvme_cmd_t *cmd) {
    uint32_t my_tail = _nv.iosq_tail;
    nvme_cq_entry_t *cq = _nv.iocq;

    _nv.iosq[my_tail] = *cmd;
    __asm__ volatile("mfence" ::: "memory");
    _nv.iosq_tail = (my_tail + 1) % IOQ_SIZE;
    sq_doorbell(1, _nv.iosq_tail);

    for (uint64_t spins = 0; spins < 30000000ull; spins++) {
        nvme_cq_entry_t *e = &cq[_nv.iocq_head];
        uint16_t p = (e->dw3 >> 16) & 1;
        if (p == _nv.iocq_phase) {
            uint16_t st = (e->dw3 >> 17) & 0x7FFF;   /* dw3: [15:0]=CID [16]=P [31:17]=SF */
            _nv.iocq_head = (_nv.iocq_head + 1) % IOQ_SIZE;
            if (_nv.iocq_head == 0) {
                _nv.iocq_phase ^= 1;
            }
            cq_doorbell(1, _nv.iocq_head);
#if NVME_CQE_DEBUG
            klog("nvmefsd: io cqe dw2=%x dw3=%x st=%x\n", e->dw2, e->dw3, st);
#endif
            return st ? -1 : 0;
        }
        if ((spins & 0x3F) == 0x3F) {
            proc_usleep(500);
        }
    }
    klog("nvmefsd: io cmd %x cid=%u timeout\n", cmd->opcode, cmd->cid);
    return -1;
}

static uint16_t next_cid = 1;
static uint32_t _diag_reads = 0;

/* ≤4KB 单页传输: 缓冲必须页对齐(dma 池保证) */
static int nvme_rw_blocks(uint64_t lba, uint32_t count, void *buf, bool write) {
    if (!_nv.ready || count == 0 || count > (DATA_SIZE >> _nv.lba_shift)) {
        return -1;
    }
    uint8_t *dma_data = _nv.qmem + DATA_OFF;
    ewokos_addr_t data_pa = _nv.qmem_phys + DATA_OFF;
    uint32_t bytes = count << _nv.lba_shift;

    if (write) {
        memcpy(dma_data, buf, bytes);
        __asm__ volatile("mfence" ::: "memory");
    }

    nvme_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.opcode = write ? NVME_IO_WRITE : NVME_IO_READ;
    c.flags = 0;
    c.cid = next_cid++;
    c.nsid = 1;
    c.prp1 = data_pa;
    c.cdw10 = (uint32_t)lba;
    c.cdw11 = (uint32_t)(lba >> 32);
    c.cdw12 = count - 1;
    if (io_submit(&c) != 0) {
        return -1;
    }
    if (!write) {
        memcpy(buf, dma_data, bytes);
#if NVME_CQE_DEBUG
        if (_diag_reads < 3) {
            _diag_reads++;
            const uint32_t *w = (const uint32_t *)buf;
            klog("nvmefsd: read lba=%llu n=%u d0..3=%x %x %x %x\n",
                 (unsigned long long)lba, count, w[0], w[1], w[2], w[3]);
        }
#endif
    }
    return 0;
}

static int32_t bsp_nvme_sd_read_sector(int32_t sector, void *buf) {
    if (sector < 0)
        return -1;
    return bsp_nvme_read((uint64_t)(uint32_t)sector, buf, 1) == 1 ? 0 : -1;
}

static int32_t bsp_nvme_sd_read_sectors(int32_t sector, void *buf,
                    uint32_t count) {
    if (sector < 0)
        return -1;
    return bsp_nvme_read((uint64_t)(uint32_t)sector, buf, count)
        == (int32_t)count ? 0 : -1;
}

static int32_t bsp_nvme_sd_write_sector(int32_t sector, const void *buf) {
    if (sector < 0)
        return -1;
    return bsp_nvme_write((uint64_t)(uint32_t)sector, buf, 1) == 1 ? 0 : -1;
}

int bsp_nvme_init(void) {
    return sd_init_ex(bsp_nvme_controller_init,
                 bsp_nvme_sd_read_sector,
                 bsp_nvme_sd_read_sectors,
                 bsp_nvme_sd_write_sector,
                 NULL, NULL);
}

static int32_t bsp_nvme_controller_init(void) {
    uint64_t cap;
    uint16_t st;
    if (_nv.ready) {
        return 0;
    }
    memset(&_nv, 0, sizeof(_nv));

    /* ---- PCI 探测: class 0x0108 (NVM 非易失存储), subclass 0x08 NVMe ---- */
    uint32_t bar_lo = 0, bar_hi = 0;
    for (uint32_t bus = 0; bus < 8 && bar_lo == 0; bus++) {
        for (uint32_t dev = 0; dev < 32 && bar_lo == 0; dev++) {
            for (uint32_t fn = 0; fn < 8 && bar_lo == 0; fn++) {
                if ((pci_cfg_read32(bus, dev, fn, 0) & 0xFFFF) == 0xFFFF) {
                    continue;
                }
                uint32_t cls = pci_cfg_read32(bus, dev, fn, 8);
                if ((cls >> 16) != 0x0108) {
                    continue;
                }
                uint16_t cmd = pci_cfg_read16(bus, dev, fn, 0x04);
                pci_cfg_write16(bus, dev, fn, 0x04, (uint16_t)(cmd | 0x0006));
                bar_lo = pci_cfg_read32(bus, dev, fn, 0x10);
                bar_hi = pci_cfg_read32(bus, dev, fn, 0x14);
            }
        }
    }
    if (bar_lo == 0 || (bar_lo & 0x1)) {
        klog("nvmefsd: no NVMe controller found\n");
        return -1;
    }
    ewokos_addr_t bar = ((ewokos_addr_t)(bar_hi & ~0xFUL) << 32) | (bar_lo & ~0xFUL);
    if (bar == 0) {
        return -1;
    }
    _nv.regs = (volatile uint8_t *)NVME_BAR_VA;
    {
        ewokos_addr_t ret = syscall3(SYS_MEM_MAP, NVME_BAR_VA, bar, 0x4000);
        klog("nvmefsd: map bar0 phy=%llx va=%llx ret=%llx\n",
             (unsigned long long)bar, (unsigned long long)NVME_BAR_VA,
             (unsigned long long)ret);
        if (ret != (ewokos_addr_t)NVME_BAR_VA) {
            return -1;
        }
    }

    cap = nvreg_read64(NVME_CAP);
    _nv.stride = 4u << CAP_DSTRD(cap);   /* 门铃间距 = 1 << (2 + DSTRD) 字节 */
    qmem_page = 4096u << ((cap >> 48) & 0xF);   /* MPS = 2^(12+mpsmin) */

    /* 队列内存 (dma 池, 按 MPS 页对齐; dma_alloc 返回页对齐 VA,
     * qmem_page 为 4K 的倍数, 偏移后仍保持对齐) */
    /* dma 池起始物理不保证页对齐(如 0x6c1640), 而 NVMe 要求队列按
     * MPS 页对齐 — 在分配内部向上对齐, VA/PHY 线性偏移保持一致 */
    uint32_t qmem_size = qmem_page * 4 + 16384 + 4096;
    uint8_t *raw = (uint8_t *)dma_alloc(0, qmem_size);
    if (raw == NULL) {
        klog("nvmefsd: dma alloc failed\n");
        return -1;
    }
    ewokos_addr_t raw_phys = dma_phy_addr(0, (ewokos_addr_t)raw);
    uint32_t bump = (uint32_t)((0 - (uint32_t)(raw_phys & (qmem_page - 1))) & (qmem_page - 1));
    _nv.qmem = raw + bump;
    _nv.qmem_phys = raw_phys + bump;
    memset(_nv.qmem, 0, qmem_size - bump);
    klog("nvmefsd: qmem va=%p phys=%llx page=%u\n", _nv.qmem,
         (unsigned long long)_nv.qmem_phys, qmem_page);
    _nv.asq = (nvme_cmd_t *)(_nv.qmem + ASQ_OFF);
    _nv.acq = (nvme_cq_entry_t *)(_nv.qmem + ACQ_OFF);
    _nv.iosq = (nvme_cmd_t *)(_nv.qmem + IOSQ_OFF);
    _nv.iocq = (nvme_cq_entry_t *)(_nv.qmem + IOCQ_OFF);
    _nv.asq_phase = _nv.acq_phase = 1;
    _nv.iosq_phase = _nv.iocq_phase = 1;

    /* ---- 控制器使能序列 ---- */
    nvreg_write(NVME_CC, 0);   /* EN=0 */
    for (uint64_t s = 0; s < 1000000ull; s++) {
        if (!(nvreg_read(NVME_CSTS) & CSTS_RDY)) break;
    }
    nvreg_write(NVME_AQA, ((ASQ_SIZE - 1) & 0xFFF) | (((ACQ_SIZE - 1) & 0xFFF) << 16));
    nvreg_write64(NVME_ASQ, _nv.qmem_phys + ASQ_OFF);
    nvreg_write64(NVME_ACQ, _nv.qmem_phys + ACQ_OFF);
    nvreg_write(NVME_CC, CC_EN | CC_IOSQES | CC_IOCQES);
    klog("nvmefsd: cc written, aqa=%x asq=%llx acq=%llx\n",
         nvreg_read(NVME_AQA), (unsigned long long)nvreg_read64(NVME_ASQ),
         (unsigned long long)nvreg_read64(NVME_ACQ));
    {
        /* TCG 下指令速度有限, 轮询上限取够大的固定值(QEMU 微秒级完成);
         * 定期打印 CSTS 便于诊断 EN 是否被控制器接受 */
        /* QEMU 按 CAP.TO 用定时器延迟置 RDY(TO=15 -> ~8s)。MMIO 读在
         * TCG 下很贵(~15us/次), 改为本地自旋推进时间 + 低频读 CSTS */
        uint32_t csts = 0;
        for (uint32_t round = 0; round < 400; round++) {
            csts = nvreg_read(NVME_CSTS);
            if (round == 0) {
                        }
            if (csts & CSTS_RDY) break;
            for (volatile uint32_t d = 0; d < 2000000u; d++) {
                __asm__ volatile("pause");
            }
        }
        if (!(csts & CSTS_RDY)) {
            klog("nvmefsd: controller enable timeout csts=%x\n", csts);
            return -1;
        }
    }

    /* ---- IDENTIFY controller (CNS=1, 顺手校验通路) ---- */
    {
        nvme_cmd_t c;
        memset(&c, 0, sizeof(c));
        c.opcode = NVME_ADM_IDENTIFY;
        c.cid = next_cid++;
        c.prp1 = _nv.qmem_phys + IDBUF_OFF;
        c.cdw10 = 1;
        st = 0;
        if (admin_submit(&c, &st) != 0) {
            klog("nvmefsd: identify ctrl failed st=%x\n", st);
            return -1;
        }
        klog("nvmefsd: identify ctrl ok\n");
    }

    /* ---- IDENTIFY namespace 1 (CNS=0): NSZE/LBAF ---- */
    {
        nvme_cmd_t c;
        uint32_t *id = (uint32_t *)(_nv.qmem + IDBUF_OFF);
        memset(&c, 0, sizeof(c));
        c.opcode = NVME_ADM_IDENTIFY;
        c.cid = next_cid++;
        c.nsid = 1;
        c.prp1 = _nv.qmem_phys + IDBUF_OFF;
        c.cdw10 = 0;
        memset(id, 0xAA, 4096);
        __asm__ volatile("mfence" ::: "memory");
        st = 0;
        if (admin_submit(&c, &st) != 0) {
            klog("nvmefsd: identify ns failed st=%x\n", st);
            return -1;
        }
        _nv.nsze = id[0] | ((uint64_t)id[1] << 32);   /* NSZE @bytes 0..7 */
        uint8_t flbas = ((uint8_t *)id)[26] & 0xF;
        uint32_t lbaf = id[32 + flbas / 4];           /* LBAF[] @bytes 128.. */
        /* LBAF 项: bytes 0-1 = MS, byte 2 = DS (data size, log2), byte 3 = RP */
        uint8_t ds = (lbaf >> 16) & 0xFF;
        if (ds != 512) {
            /* 观测到部分固件/QEMU 版本的 identify-ns 数据布局偏差,
             * 512B 是唯一受支持的 LBA 大小, 直接按 512 继续 */
            if (_nv.nsze == 0) {
                klog("nvmefsd: no namespace capacity, give up\n");
                return -1;
            }
            klog("nvmefsd: lbaf parse failed, assume 512B LBA\n");
        }
        _nv.lba_shift = 9;
    }

    /* ---- 创建 IO CQ/SQ (qid=1) ---- */
    {
        nvme_cmd_t c;
        memset(&c, 0, sizeof(c));
        c.opcode = NVME_ADM_CREATE_IO_CQ;
        c.cid = next_cid++;
        c.prp1 = _nv.qmem_phys + IOCQ_OFF;
        /* 16 位字段两两打包: CDW10 = CQID | (QSIZE<<16), CDW11 = FLAGS | (VECTOR<<16) */
        c.cdw10 = 1u | ((uint32_t)(IOQ_SIZE - 1) << 16);   /* CQID=1, size-1 */
        c.cdw11 = 1u;                         /* flags: PC=1(bit0), IEN=0(bit1); vector=0 */
        st = 0;
        if (admin_submit(&c, &st) != 0) {
            klog("nvmefsd: create io cq failed st=%x\n", st);
            return -1;
        }
        memset(&c, 0, sizeof(c));
        c.opcode = NVME_ADM_CREATE_IO_SQ;
        c.cid = next_cid++;
        c.prp1 = _nv.qmem_phys + IOSQ_OFF;
        c.cdw10 = 1u | ((uint32_t)(IOQ_SIZE - 1) << 16);   /* SQID=1, size-1 */
        c.cdw11 = 1u | (1u << 16);            /* flags: PC=1; 关联 CQID=1 */
        if (admin_submit(&c, &st) != 0) {
            klog("nvmefsd: create io sq failed st=%x\n", st);
            return -1;
        }
    }

    /* 预热: 首条 IO 命令的完成经 QEMU 主循环投递, 延迟不定; 提交一条
     * 探测读后等待并排空完成队列, 避免遗留陈旧 CQE 错位后续命令 */
    {
        uint32_t probe[128];
        memset(probe, 0, sizeof(probe));
        nvme_rw_blocks(8, 8, probe, false);
        for (uint32_t drain = 0; drain < 6000; drain++) {
            nvme_cq_entry_t *e = &_nv.iocq[_nv.iocq_head];
            if (((e->dw3 >> 16) & 1) != _nv.iocq_phase) {
                proc_usleep(500);
                continue;
            }
            _nv.iocq_head = (_nv.iocq_head + 1) % IOQ_SIZE;
            if (_nv.iocq_head == 0) {
                _nv.iocq_phase ^= 1;
            }
            cq_doorbell(1, _nv.iocq_head);
            klog("nvmefsd: warm-up done (drained)\n");
            break;
        }
    }

    _nv.ready = true;
    klog("nvmefsd: nvme ready, nsze=%llu blocks (%llu MB), lba=%u\n",
         (unsigned long long)_nv.nsze,
         (unsigned long long)(_nv.nsze >> (20 - _nv.lba_shift)),
         1u << _nv.lba_shift);
    return 0;
}

int32_t bsp_nvme_read(uint64_t start_lba, void *buf, uint32_t count) {
    uint32_t max_chunk = DATA_SIZE >> _nv.lba_shift;
    uint8_t *p = (uint8_t *)buf;
    uint32_t total = count;
    while (count) {
        uint32_t n = count > max_chunk ? max_chunk : count;
        if (nvme_rw_blocks(start_lba, n, p, false) != 0) {
            return -1;
        }
        p += n << _nv.lba_shift;
        start_lba += n;
        count -= n;
    }
    return (int32_t)total;   /* raspi5 语义: 成功返回读取的块数 */
}

int32_t bsp_nvme_write(uint64_t start_lba, const void *buf, uint32_t count) {
    uint32_t max_chunk = DATA_SIZE >> _nv.lba_shift;
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t total = count;
    while (count) {
        uint32_t n = count > max_chunk ? max_chunk : count;
        if (nvme_rw_blocks(start_lba, n, (void *)p, true) != 0) {
            return -1;
        }
        p += n << _nv.lba_shift;
        start_lba += n;
        count -= n;
    }
    return (int32_t)total;   /* raspi5 语义: 成功返回写入的块数 */
}

uint64_t bsp_nvme_get_block_count(void) {
    return _nv.nsze;
}

uint32_t bsp_nvme_get_block_size(void) {
    return 1u << _nv.lba_shift;
}
