/* SPDX-License-Identifier: GPL-2.0-only */
/* WCN6855 MHI firmware bootstrap for Q8B.
 * Protocol reference: Linux drivers/bus/mhi/{common.h,host/boot.c,host/pm.c}
 * and drivers/net/wireless/ath/ath11k/mhi.c. Uses EwokOS noncached DMA and
 * polls event rings; MSI writes target a reserved noncached DMA word.
 * DMA memory belongs to this long-lived controller process. */
#include "wcn_mhi.h"
#include "wcn_wmi.h"
#include "wcn_qmi.h"
#include "wcn_board.h"
#include "wcn_ce.h"
#include "wcn_dp.h"
#include "wcn_sta.h"
#include <bsp/pcie.h>
#include <ewoksys/dma.h>
#include <ewoksys/klog.h>
#include "wcn_util.h"
#include <ewoksys/kernel_tic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define BAR_VA 0x57000000ULL
#define BAR_SIZE 0x100000u
#define DMA_BYTES (8u * 1024u * 1024u)
#define FW_OFFSET 0x10000u
#define FW_LIMIT (6u * 1024u * 1024u)
#define SBL_BYTES 0x80000u
#define CTX_CHANNEL 0x0000u
#define CTX_EVENT 0x1800u
#define CTX_COMMAND 0x1900u
#define RING_COMMAND 0x2000u
#define RING_EVENT0 0x3000u
#define RING_EVENT1 0x4000u
#define VECTOR_OFFSET 0x5000u
#define RDDM_VECTOR 0x5200u
#define RDDM_BYTES 0x420000u
#define RING_IPCR_TX 0x6000u
#define RING_IPCR_RX 0x6400u
#define IPCR_RX 0x600000u
#define IPCR_TX 0x680000u
#define IPCR_COUNT 64u
#define IPCR_BYTES 8192u

/* MHI contexts have a 44-byte wire stride, including the three u32 words. */
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t config, type, index;
    uint64_t base, length, rp, wp;
} mhi_context_t;
typedef struct { uint64_t ptr; uint32_t data, info; } mhi_tre_t;
typedef struct { uint64_t address, size; } bhi_vector_t;
typedef char context_wire_size[(sizeof(mhi_context_t) == 44) ? 1 : -1];
typedef char tre_wire_size[(sizeof(mhi_tre_t) == 16) ? 1 : -1];

static struct {
    q8b_pcie_device_t pcie;
    uint64_t bar_phys, dma_phys;
    uint8_t *dma;
    uint8_t *rddm;
    uint64_t rddm_phys;
    uint32_t register_window;
    uint32_t hw, subsystem, bhi, bhie, chdb, erdb, fw_size;
    uint32_t er_rp[2], er_wp[2], events, last_event, last_data;
    unsigned cmd_wp, cmd_done, cmd_code;
    unsigned tx_rp, tx_wp, rx_rp, rx_wp, rx_packets, tx_packets;
    int ipcr_ready;
    int attached, running, contexts_ready;
    const char *stage;
} w;

const char *wcn_firmware_path(const char *name) {
    static char path[160];
    /* The QCNFA765 nfa765 payload is byte-identical in the linux-firmware
     * hw2.0 and hw2.1 directories (AMSS/M3/regdb/board-2.bin). The endpoint
     * revision is therefore accepted as hw2.0 or hw2.1 while this image tree
     * keeps one canonical hw2.1 copy. The 17cb:0108 form is what replacement
     * cards report; using the generic image makes firmware fall back to a
     * generated MAC instead of exposing the module's calibrated identity. */
    const uint32_t nfa765_qcom =
            ((uint32_t)Q8B_PCIE_WLAN_SUBSYS_QCOM_DID << 16) |
            Q8B_PCIE_WLAN_SUBSYS_QCOM_VID;
    const uint32_t nfa765_alt =
            ((uint32_t)Q8B_PCIE_WLAN_SUBSYS_ALT_DID << 16) |
            Q8B_PCIE_WLAN_SUBSYS_ALT_VID;
    const char *dir = (w.subsystem == nfa765_qcom ||
            w.subsystem == nfa765_alt) ? "nfa765/" : "";
    snprintf(path, sizeof(path),
            "/etc/firmware/ath11k/WCN6855/hw2.1/%s%s", dir, name);
    return path;
}

static uint32_t read32(uint64_t addr) {
    uint32_t value = *(volatile uint32_t *)(ewokos_addr_t)addr;
    __asm__ volatile("dmb sy" ::: "memory");
    return value;
}
static void write32(uint64_t addr, uint32_t value) {
    *(volatile uint32_t *)(ewokos_addr_t)addr = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
static uint32_t reg(uint32_t off) { return read32(BAR_VA + off); }
static void put(uint32_t off, uint32_t value) {
    /* Direct window writes and SOC reset invalidate the cached selection. */
    if (off == 0x310c || off == 0x3008) w.register_window = 0;
    write32(BAR_VA + off, value);
}
static void put64(uint32_t off, uint64_t value) {
    put(off + 4, (uint32_t)(value >> 32));
    put(off, (uint32_t)value);
}
/* The single driver owner serializes register access. Like ath11k, only
 * flush the PCIe window write when the selected 512-KiB region changes. */
static void select_window(uint32_t off) {
    uint32_t window = 0x40000000 | ((off >> 19) & 63);
    if (window == w.register_window) return;
    put(0x310c, window);
    (void)reg(0x310c);
    w.register_window = window;
}
static uint32_t soc_read(uint32_t off) {
    if (off < 0x80000) return reg(off);
    select_window(off);
    return reg(0x80000 + (off & 0x7ffff));
}
static void soc_write(uint32_t off, uint32_t value) {
    if (off < 0x80000) { put(off, value); return; }
    select_window(off);
    put(0x80000 + (off & 0x7ffff), value);
}
uint32_t wcn_soc_read(uint32_t off) { return soc_read(off); }
void wcn_soc_write(uint32_t off, uint32_t value) { soc_write(off, value); }
static int offset_valid(uint32_t offset, uint32_t bytes) {
    return offset && !(offset & 3) && offset <= BAR_SIZE - bytes;
}
static uint32_t ee(void) { return reg(w.bhi + 0x28); }
static mhi_context_t *context(unsigned off) { return (mhi_context_t *)(w.dma + off); }
static void sync_dma(void) { __asm__ volatile("dsb sy" ::: "memory"); }

static int wait_reg(uint32_t off, uint32_t mask, uint32_t value, unsigned timeout) {
    uint64_t end = kernel_tic_ms(0) + timeout;
    do {
        uint32_t v = reg(off);
        if (v == 0xffffffffu) return -1;
        if ((v & mask) == value) return 0;
        usleep(1000);
    } while (kernel_tic_ms(0) < end);
    wcn_log("wcn: timeout reg=%x value=%08x mask=%08x want=%08x ee=%u\n",
         off, reg(off), mask, value, ee());
    return -1;
}

int wcn_attach(void) {
    w.stage = "attach";
    wcn_log("wcn: attach probing PCIe snapshot\n");
    /* pcied owns power, LTSSM, ECAM and PCI configuration.  Consume only its
     * read-only snapshot; this driver never writes a PCI capability or the
     * command register. */
    int count = q8b_pcie_service_get_count();
    int found = 0;
    if (count < 0)
        return -1;
    if (count > 32)
        count = 32;
    for (int i = 0; i < count; i++) {
        q8b_pcie_device_t device;
        if (q8b_pcie_service_get_device((unsigned)i, &device))
            return -1;
        if (device.vendor_id == Q8B_PCIE_WLAN_VENDOR_ID &&
                device.device_id == Q8B_PCIE_WLAN_DEVICE_ID &&
                q8b_pcie_wlan_subsystem_supported(device.subsystem_vendor,
                        device.subsystem_device)) {
            w.pcie = device;
            found = 1;
            break;
        }
    }
    if (!found ||
            (w.pcie.command & (Q8B_PCIE_ENABLE_MEMORY | Q8B_PCIE_ENABLE_MASTER)) !=
            (Q8B_PCIE_ENABLE_MEMORY | Q8B_PCIE_ENABLE_MASTER) ||
            !w.pcie.bar_phys[0])
        return -1;
    w.subsystem = ((uint32_t)w.pcie.subsystem_device << 16) |
            w.pcie.subsystem_vendor;
    wcn_wmi_set_permanent_mac(w.pcie.vpd_mac_valid ? w.pcie.vpd_mac : NULL);
    /* Keep the fallback identity stable without writing a host file.  The
     * module itself (QCNFA765 replacement cards) ships unprogrammed: VPD,
     * PCIe DSN and QMI OTP/EEPROM are all absent, so identical modules would
     * otherwise derive colliding addresses from the PCI identity alone and
     * kill each other's 4-way handshake on one AP.  Anchor the seed to the
     * SoC serial number (QFPROM, per-device) instead: the address follows
     * the board, not the module slot.  Prefer the PCIe Device Serial Number
     * when a module does expose one, and fold the PCI function identity in
     * as low-order seasoning so two WLAN devices on one board still differ.
     * Do not include BAR0: pcied may allocate a different BAR after a
     * stop/start, and a BAR address is not hardware identity. */
    uint64_t seed = wcn_board_cpu_id();
    if (seed) {
        seed = seed * UINT64_C(0x100000001b3) ^
                (((uint64_t)w.pcie.bus << 24) |
                 ((uint64_t)w.pcie.device << 8) | w.pcie.function);
        wcn_log("wcn: fallback MAC seed source=SoC serial (QFPROM) %08x %08x\n",
                (unsigned)(seed >> 32), (unsigned)seed);
    } else {
        seed = UINT64_C(0x9e3779b97f4a7c15);
        seed ^= ((uint64_t)w.pcie.vendor_id << 48) |
                ((uint64_t)w.pcie.device_id << 32) |
                ((uint64_t)w.pcie.subsystem_vendor << 16) |
                w.pcie.subsystem_device;
        seed = seed * UINT64_C(0x100000001b3) ^
                (((uint64_t)w.pcie.bus << 24) |
                 ((uint64_t)w.pcie.device << 8) | w.pcie.function);
        if (w.pcie.serial_valid) {
            seed ^= w.pcie.serial_number;
            wcn_log("wcn: fallback MAC seed source=PCIe Device Serial Number\n");
        } else {
            wcn_log("wcn: fallback MAC seed source=stable PCI identity\n");
        }
    }
    if (!seed) seed = UINT64_C(1);
    wcn_wmi_set_hardware_seed(seed);
    wcn_board_set_subsystem(w.pcie.subsystem_vendor, w.pcie.subsystem_device);
    wcn_log("wcn: firmware profile=%s\n",
            (w.subsystem == (((uint32_t)Q8B_PCIE_WLAN_SUBSYS_QCOM_DID << 16) |
                    Q8B_PCIE_WLAN_SUBSYS_QCOM_VID) ||
             w.subsystem == (((uint32_t)Q8B_PCIE_WLAN_SUBSYS_ALT_DID << 16) |
                    Q8B_PCIE_WLAN_SUBSYS_ALT_VID)) ? "nfa765" : "common");
    w.bar_phys = w.pcie.bar_phys[0];
    if (q8b_pcie_map_bar(&w.pcie, 0, BAR_VA, BAR_SIZE)) {
        wcn_log("wcn: attach failed: BAR0 map phys=%llx size=%x\n",
                (unsigned long long)w.pcie.bar_phys[0], BAR_SIZE);
        return -1;
    }
    w.hw = reg(0x224);
    /* WCN6855 cards in the Q8B/TW220 population report both 2.0 (0x10)
     * and 2.1 (0x11) silicon revisions. The firmware profile is selected
     * from the PCI subsystem identity, so the low revision byte must not
     * reject the otherwise valid 2.0 endpoint. */
    unsigned hw_major = (w.hw >> 8) & 15;
    unsigned hw_rev = w.hw & 255;
    if (hw_major != 2 || (hw_rev != 0x10 && hw_rev != 0x11)) {
        wcn_log("wcn: attach failed: unexpected HW=%08x (want WCN6855 hw2.0/2.1)\n", w.hw);
        return -1;
    }
    wcn_log("wcn: silicon revision=hw2.%u firmware tree=hw2.1/nfa765\n",
            hw_rev - 0x10);
    w.bhi = reg(0x28);
    if (!offset_valid(w.bhi, 0xc8)) {
        wcn_log("wcn: attach failed: invalid BHI offset=%08x\n", w.bhi);
        return -1;
    }
    w.attached = 1;
    w.stage = "PBL";
    wcn_log("wcn: attach ok vendor=%04x device=%04x sub=%04x:%04x bar=%llx\n",
            w.pcie.vendor_id, w.pcie.device_id, w.pcie.subsystem_vendor,
            w.pcie.subsystem_device, (unsigned long long)w.bar_phys);
    return 0;
}

static int software_reset(void) {
    /* ath11k's power-on reset prevents stale hot-reset interrupts and the
     * warm-boot entry from sending SBL down an obsolete execution path. */
    /* A previous owner/failed attempt may have disabled PCI memory decode.
     * Restore it through pcied before the first BAR access, not only after
     * SOC reset. Otherwise all of the reset writes can be discarded. */
    if (q8b_pcie_service_prepare_wlan_dma()) return -1;
    usleep(100000);
    soc_write(0x1e081b0, 0x111);
    soc_write(0x1e402bc, soc_read(0x1e402bc) | 0x10);
    usleep(5000);
    soc_write(0x1e08228, 0xffffffff);
    soc_write(0x1f8031c, soc_read(0x1f8031c) & ~4u);
    put(0x360, 0); put(0x368, 0); put(0x394, 0); put(0x39c, 0);
    (void)soc_read(0x1f80500); /* Q6 cookie. */
    (void)soc_read(0x1f80504);
    usleep(10000);
    soc_write(0x1f80504, 0);
    usleep(10000);
    (void)soc_read(0x1f80504);
    /* WLAON_SOC_RESET_CAUSE is read-to-clear. This is part of ath11k's
     * reset sequence, even when debug logging is disabled: leaving it set
     * can send Q6 down the old warm-boot path. */
    uint32_t cause = soc_read(0x1f8060c);
    wcn_log("wcn: reset cause=%08x\n", cause);
    uint32_t reset = reg(0x3008);
    put(0x3008, reset | 1);
    usleep(10000);
    put(0x3008, reset & ~1u);
    usleep(10000);
    put(0x310c, 0x40000000);
    /* SOC reset can clear bus mastering. Restore it before asking MHI/PBL
     * to recover from SYSERR, as in the original working reset sequence. */
    if (q8b_pcie_service_prepare_wlan_dma()) return -1;
    put(0x38, 2);
    usleep(10000);
    w.bhi = reg(0x28);
    wcn_log("wcn: reset complete ctrl=%08x status=%08x ee=%u\n",
            reg(0x38), reg(0x48), ee());
    return 0;
}

static int load_firmware(void) {
    const char *path = wcn_firmware_path("amss.bin");
    int fd = open(path, O_RDONLY);
    if (fd < 0) { wcn_log("wcn: missing %s\n", path); return -1; }
    off_t size = lseek(fd, 0, SEEK_END);
    if (size < SBL_BYTES || size > FW_LIMIT || lseek(fd, 0, SEEK_SET) < 0) {
        close(fd); return -1;
    }
    if (!w.dma) {
        w.dma = (uint8_t *)dma_alloc(0, DMA_BYTES);
        if (!w.dma) { close(fd); return -1; }
        w.dma_phys = dma_phy_addr(0, (ewokos_addr_t)w.dma);
        if (!w.dma_phys || w.dma_phys + DMA_BYTES > 0x100000000ULL) {
            dma_free(0, (ewokos_addr_t)w.dma); w.dma = NULL;
            close(fd); return -1;
        }
    }
    w.fw_size = (uint32_t)size;
    unsigned pos = 0;
    while (pos < w.fw_size) {
        int n = read(fd, w.dma + FW_OFFSET + pos, w.fw_size - pos);
        if (n <= 0) { close(fd); return -1; }
        pos += (unsigned)n;
    }
    close(fd);
    if (memcmp(w.dma + FW_OFFSET, "\177ELF", 4)) return -1;
    sync_dma();
    return 0;
}

static int prepare_rddm(void) {
    w.bhie = reg(0x2c);
    if (!offset_valid(w.bhie, 0x80)) return -1;
    if (!w.rddm) {
        w.rddm = (uint8_t *)dma_alloc(0, RDDM_BYTES);
        if (!w.rddm) return -1;
        w.rddm_phys = dma_phy_addr(0, (ewokos_addr_t)w.rddm);
        if (!w.rddm_phys || w.rddm_phys + RDDM_BYTES > 0x100000000ULL) return -1;
    }
    memset(w.rddm, 0, RDDM_BYTES);
    bhi_vector_t *vec = (bhi_vector_t *)(w.dma + RDDM_VECTOR);
    unsigned off = 0, n = 0;
    while (off < RDDM_BYTES) {
        unsigned len = RDDM_BYTES - off;
        if (len > SBL_BYTES) len = SBL_BYTES;
        vec[n].address = w.rddm_phys + off; vec[n].size = len;
        n++; off += len;
    }
    for (unsigned off = 0x60; off <= 0x78; off += 4) put(w.bhie + off, 0);
    sync_dma();
    put64(w.bhie + 0x60, w.dma_phys + RDDM_VECTOR);
    put(w.bhie + 0x68, n * sizeof(*vec));
    uint32_t sequence = (uint32_t)kernel_tic_ms(0) & 0x3fffffff;
    put(w.bhie + 0x70, sequence ? sequence : 1);
    return 0;
}

static void capture_rddm(void) {
    if (!w.rddm) return;
    uint64_t until = kernel_tic_ms(0) + 10000;
    while ((reg(w.bhie + 0x78) >> 30) != 2 && kernel_tic_ms(0) < until) usleep(1000);
    uint32_t status = reg(w.bhie + 0x78);
    if ((status >> 30) != 2) return;
    sync_dma();
    int fd = open("/tmp/wcn-rddm.bin", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    unsigned pos = 0;
    while (pos < RDDM_BYTES) {
        int n = write(fd, w.rddm + pos, RDDM_BYTES - pos);
        if (n <= 0) break;
        pos += n;
    }
    close(fd);
    wcn_log("wcn: RDDM saved bytes=%u /tmp/wcn-rddm.bin\n", pos);
}

static int setup_contexts(void) {
    w.chdb = reg(0x18); w.erdb = reg(0x20);
    if (!offset_valid(w.chdb, 128 * 8) || !offset_valid(w.erdb, 16)) return -1;
    /* Preserve the BHIE receive vector installed before BHI download. */
    memset(w.dma, 0, VECTOR_OFFSET);
    memset(w.dma + 0x6000, 0, FW_OFFSET - 0x6000);
    w.cmd_wp = w.cmd_done = w.cmd_code = 0;
    w.tx_rp = w.tx_wp = w.rx_rp = 0;
    w.rx_wp = IPCR_COUNT - 1;
    w.ipcr_ready = 0;
    /* IPCR outbound/inbound share event ring 1, as in ath11k. */
    mhi_context_t *ch = context(CTX_CHANNEL);
    ch[20].config = ch[21].config = 2u << 8;
    ch[20].type = 1; ch[21].type = 2;
    ch[20].index = ch[21].index = 1;
    mhi_context_t *ev = context(CTX_EVENT);
    const unsigned offsets[] = {RING_EVENT0, RING_EVENT1};
    const unsigned counts[] = {32, 256};
    for (unsigned i = 0; i < 2; i++) {
        ev[i].type = 1;
        ev[i].index = 0; /* Single MSI vector; ring is consumed by polling. */
        ev[i].base = w.dma_phys + offsets[i];
        ev[i].length = counts[i] * sizeof(mhi_tre_t);
        ev[i].rp = ev[i].base;
        ev[i].wp = ev[i].base + ev[i].length - sizeof(mhi_tre_t);
        w.er_rp[i] = 0; w.er_wp[i] = counts[i] - 1;
    }
    mhi_context_t *cmd = context(CTX_COMMAND);
    cmd->base = w.dma_phys + RING_COMMAND;
    cmd->length = 128 * sizeof(mhi_tre_t);
    cmd->rp = cmd->wp = cmd->base;
    sync_dma();
    put64(0x58, w.dma_phys + CTX_CHANNEL);
    put64(0x60, w.dma_phys + CTX_EVENT);
    put64(0x68, w.dma_phys + CTX_COMMAND);
    /* QMI allocates firmware DDR separately from the transport arena.
     * Match the controller's DMA aperture, not the first allocation. */
    put64(0x80, 0);
    put64(0x88, 0xffffffffULL);
    put64(0x98, 0);
    put64(0xa0, 0xffffffffULL);
    put(0x10, (reg(0x10) & 0x0000ffff) | (2u << 16));
    for (unsigned i = 0; i < 2; i++) put64(w.erdb + i * 8, ev[i].wp);
    put64(w.chdb + 127 * 8, 1); /* Keep awake while polling during bring-up. */
    w.contexts_ready = 1;
    put(0x38, (reg(0x38) & ~0xff00u) | 0x200);
    return 0;
}

static void ipcr_event(const mhi_tre_t *event) {
    unsigned type = (event->info >> 16) & 255;
    unsigned code = event->data >> 24;
    if (type == 0x21) {
        uint64_t base = w.dma_phys + RING_COMMAND;
        if (event->ptr < base || event->ptr >= base + 128 * 16 || (event->ptr & 15)) return;
        w.cmd_done = (unsigned)((event->ptr - base) / 16) + 1;
        w.cmd_code = code;
        return;
    }
    if (type != 0x22 || !w.ipcr_ready) return;
    unsigned channel = event->info >> 24;
    if (channel != 20 && channel != 21) return;
    if (code != 2 && code != 4) {
        wcn_log("wcn: IPCR channel=%u completion error=%u\n", channel, code);
        w.ipcr_ready = 0;
        w.stage = "IPCR-completion-error";
        return;
    }
    uint64_t base = w.dma_phys + (channel == 20 ? RING_IPCR_TX : RING_IPCR_RX);
    if (event->ptr < base || event->ptr >= base + IPCR_COUNT * 16 || (event->ptr & 15)) {
        w.ipcr_ready = 0; w.stage = "IPCR-invalid-pointer"; return;
    }
    unsigned index = (unsigned)((event->ptr - base) / 16);
    if (channel == 20) {
        w.tx_rp = (index + 1) % IPCR_COUNT;
        return;
    }
    unsigned bytes = event->data & 0xffff;
    if (index != w.rx_rp || bytes > IPCR_BYTES) {
        w.ipcr_ready = 0; w.stage = "IPCR-invalid-receive"; return;
    }
    w.rx_packets++;
    wcn_qmi_receive(w.dma + IPCR_RX + index * IPCR_BYTES, bytes);
    w.rx_rp = (w.rx_rp + 1) % IPCR_COUNT;
    w.rx_wp = (w.rx_wp + 1) % IPCR_COUNT;
    context(CTX_CHANNEL)[21].wp = base + w.rx_wp * 16;
    sync_dma();
    put64(w.chdb + 21 * 8, base + w.rx_wp * 16);
}

void wcn_poll(void) {
    if (!w.contexts_ready) return;
    if (reg(0x48) & 4) {
        wcn_log("wcn: MHI SYSERR ee=%u status=%08x error=%08x\n", ee(), reg(0x48), reg(w.bhi + 0x30));
        capture_rddm();
        wcn_stop();
        w.stage = "MHI-SYSERR";
        return;
    }
    mhi_context_t *ev = context(CTX_EVENT);
    const unsigned offsets[] = {RING_EVENT0, RING_EVENT1};
    const unsigned counts[] = {32, 256};
    for (unsigned i = 0; i < 2; i++) {
        sync_dma();
        uint64_t device_rp = *(volatile uint64_t *)(void *)&ev[i].rp;
        uint64_t base = w.dma_phys + offsets[i];
        if (device_rp < base || device_rp >= base + counts[i] * 16 || (device_rp & 15)) {
            w.stage = "invalid-event-pointer";
            return;
        }
        unsigned count = 0;
        while (base + w.er_rp[i] * 16 != device_rp && count++ < counts[i]) {
            mhi_tre_t event;
            memcpy(&event, w.dma + offsets[i] + w.er_rp[i] * 16, sizeof(event));
            w.events++; w.last_event = event.info; w.last_data = event.data;
            ipcr_event(&event);
            w.er_rp[i] = (w.er_rp[i] + 1) % counts[i];
            w.er_wp[i] = (w.er_wp[i] + 1) % counts[i];
        }
        if (count) {
            ev[i].wp = base + w.er_wp[i] * 16;
            sync_dma();
            put64(w.erdb + i * 8, ev[i].wp);
        }
    }
    wcn_qmi_poll();
    wcn_ce_poll();
    wcn_dp_poll();
    wcn_sta_poll();
}

int wcn_ipcr_send(const void *packet, unsigned bytes) {
    if (!w.ipcr_ready || !packet || !bytes || bytes > IPCR_BYTES) return -1;
    unsigned next = (w.tx_wp + 1) % IPCR_COUNT;
    if (next == w.tx_rp) return -1;
    unsigned off = IPCR_TX + w.tx_wp * IPCR_BYTES;
    memcpy(w.dma + off, packet, bytes);
    mhi_tre_t *tre = (mhi_tre_t *)(w.dma + RING_IPCR_TX);
    tre[w.tx_wp].ptr = w.dma_phys + off;
    tre[w.tx_wp].data = bytes;
    tre[w.tx_wp].info = (2u << 16) | (1u << 9);
    w.tx_wp = next;
    uint64_t wp = w.dma_phys + RING_IPCR_TX + next * 16;
    context(CTX_CHANNEL)[20].wp = wp;
    sync_dma();
    put64(w.chdb + 20 * 8, wp);
    w.tx_packets++;
    return 0;
}

static int start_channel(unsigned channel, unsigned ring) {
    mhi_context_t *ch = &context(CTX_CHANNEL)[channel];
    ch->config = (2u << 8) | 1;
    ch->base = w.dma_phys + ring;
    ch->length = IPCR_COUNT * 16;
    ch->rp = ch->wp = ch->base;
    mhi_tre_t *cmd = (mhi_tre_t *)(w.dma + RING_COMMAND);
    unsigned index = w.cmd_wp;
    cmd[index].ptr = 0; cmd[index].data = 0;
    cmd[index].info = (channel << 24) | (0x12u << 16);
    w.cmd_wp = (index + 1) % 128;
    uint64_t wp = w.dma_phys + RING_COMMAND + w.cmd_wp * 16;
    context(CTX_COMMAND)->wp = wp;
    w.cmd_done = 0;
    sync_dma(); put64(0x70, wp);
    uint64_t until = kernel_tic_ms(0) + 3000;
    while (w.cmd_done != index + 1 && kernel_tic_ms(0) < until) {
        wcn_poll(); usleep(1000);
    }
    return w.cmd_done == index + 1 && w.cmd_code == 1 ? 0 : -1;
}

static int start_ipcr(void) {
    if (start_channel(20, RING_IPCR_TX) || start_channel(21, RING_IPCR_RX)) return -1;
    mhi_tre_t *rx = (mhi_tre_t *)(w.dma + RING_IPCR_RX);
    for (unsigned i = 0; i < IPCR_COUNT; i++) {
        rx[i].ptr = w.dma_phys + IPCR_RX + i * IPCR_BYTES;
        rx[i].data = IPCR_BYTES;
        rx[i].info = (2u << 16) | (1u << 9);
    }
    uint64_t wp = w.dma_phys + RING_IPCR_RX + w.rx_wp * 16;
    context(CTX_CHANNEL)[21].wp = wp;
    w.ipcr_ready = 1;
    sync_dma(); put64(w.chdb + 21 * 8, wp);
    wcn_qmi_begin();
    return 0;
}

static int download_amss(void) {
    w.bhie = reg(0x2c);
    if (!offset_valid(w.bhie, 0x80)) return -1;
    bhi_vector_t *vec = (bhi_vector_t *)(w.dma + VECTOR_OFFSET);
    uint32_t off = FW_OFFSET, left = w.fw_size, n = 0;
    if (left > SBL_BYTES + 4 && !memcmp(w.dma + off + SBL_BYTES, "\177ELF", 4)) {
        off += SBL_BYTES; left -= SBL_BYTES;
    }
    while (left) {
        uint32_t bytes = left > SBL_BYTES ? SBL_BYTES : left;
        vec[n].address = w.dma_phys + off;
        vec[n].size = bytes;
        n++; off += bytes; left -= bytes;
    }
    sync_dma();
    put64(w.bhie + 0x2c, w.dma_phys + VECTOR_OFFSET);
    put(w.bhie + 0x34, n * sizeof(*vec));
    uint32_t seq = (reg(w.bhie + 0x3c) + 1) & 0x3fffffff;
    if (!seq) seq = 1;
    put(w.bhie + 0x3c, seq);
    uint64_t until = kernel_tic_ms(0) + 20000;
    do {
        wcn_poll();
        uint32_t status = reg(w.bhie + 0x44);
        if ((status >> 30) == 2) return 0;
        if ((status >> 30) == 3 || (reg(0x48) & 4)) return -1;
        usleep(1000);
    } while (kernel_tic_ms(0) < until);
    wcn_log("wcn: BHIE timeout status=%08x ee=%u\n", reg(w.bhie + 0x44), ee());
    return -1;
}

void wcn_stop(void) {
    if (!w.attached) return;
    /* Stop the endpoint engine before memory can be reused.  PCI command and
     * link state remain owned by pcied for the lifetime of the bus. */
    if (w.running) {
        put(0x38, reg(0x38) | 2);
        (void)wait_reg(0x38, 2, 0, 2000);
    }
    w.register_window = 0;
    w.running = w.contexts_ready = w.ipcr_ready = 0;
    wcn_ce_release();
    wcn_dp_release();
    wcn_sta_release();
    wcn_qmi_release();
    wcn_qmi_reset();
    w.stage = "stopped";
}

int wcn_start(void) {
    if (!w.attached || w.running) return -1;
    wcn_log("wcn: start begin\n");
    w.stage = "load-firmware";
    wcn_log("wcn: loading firmware\n");
    if (load_firmware()) { w.stage = "firmware-error"; return -1; }
    wcn_log("wcn: firmware loaded bytes=%u\n", w.fw_size);
    w.running = 1;
    wcn_log("wcn: software reset begin\n");
    if (software_reset()) goto fail;
    /* Allow PBL to finish clearing its boot interface after SYSERR reset. */
    usleep(200000);
    /* SOC reset clears function configuration. Ask the host owner to restore
     * DMA and its retained polling MSI sink; never map/write ECAM here. */
    if (q8b_pcie_service_prepare_wlan_dma()) goto fail;
    if (prepare_rddm()) goto fail;
    wcn_log("wcn: RDDM prepared\n");
    w.stage = "BHI";
    put(w.bhi + 0x20, 0); /* BHI interrupt vector; completion is polled. */
    put(w.bhi + 0x2c, 0);
    put64(w.bhi + 8, w.dma_phys + FW_OFFSET);
    put(w.bhi + 0x10, SBL_BYTES);
    /* PBL can retain its last doorbell across a software reset. */
    uint32_t old_seq = reg(w.bhi + 0x18) & 0x3fffffff;
    uint32_t seq = (uint32_t)kernel_tic_ms(0) & 0x3fffffff;
    if (!seq || seq == old_seq) seq = (old_seq + 1) & 0x3fffffff;
    if (!seq) seq = 1;
    put(w.bhi + 0x18, seq);
    if (wait_reg(w.bhi + 0x2c, 0xc0000000, 0x80000000, 20000)) goto fail;
    wcn_log("wcn: BHI ready\n");
    w.stage = "READY";
    if (wait_reg(0x38, 2, 0, 10000) || wait_reg(0x48, 1, 1, 10000) || setup_contexts()) goto fail;
    wcn_log("wcn: MHI contexts ready\n");
    uint64_t until = kernel_tic_ms(0) + 10000;
    while (ee() == 0 && kernel_tic_ms(0) < until) { wcn_poll(); usleep(1000); }
    if (ee() != 1) goto fail;
    w.stage = "BHIE";
    wcn_log("wcn: downloading AMSS\n");
    if (download_amss()) goto fail;
    until = kernel_tic_ms(0) + 20000;
    while (ee() != 2 && kernel_tic_ms(0) < until) { wcn_poll(); usleep(1000); }
    if (ee() != 2) goto fail;
    w.stage = "AMSS";
    w.stage = "IPCR";
    wcn_log("wcn: starting IPCR\n");
    if (start_ipcr()) goto fail;
    w.stage = "QMI-discovery";
    return 0;
fail:
    klog("wcn: failed stage=%s ee=%u MHI=%08x BHI=%08x err=%08x BHIE=%08x\n",
         w.stage, ee(), reg(0x48), reg(w.bhi + 0x2c), reg(w.bhi + 0x30),
         offset_valid(w.bhie, 0x80) ? reg(w.bhie + 0x44) : 0);
    wcn_log("wcn: failed stage=%s ee=%u MHI=%08x BHI=%08x err=%08x\n",
         w.stage, ee(), reg(0x48), reg(w.bhi + 0x2c), reg(w.bhi + 0x30));
    const char *failed = w.stage;
    wcn_stop();
    w.stage = failed;
    return -1;
}

int wcn_running(void) { return w.running; }

void wcn_status(char *out, size_t size) {
    char qmi[288], ce_status[512], dp[512];
    wcn_qmi_status(qmi, sizeof(qmi));
    wcn_ce_status(ce_status, sizeof(ce_status));
    wcn_dp_status(dp,sizeof(dp));
    snprintf(out, size,
        "wcn6855 stage=%s attached=%d running=%d hw=%08x subsystem=%08x "
        "bar=%llx ee=%u mhi=%08x bhi=%08x error=%08x dma=%llx "
        "events=%u last=%08x/%08x ipcr=%d rx=%u tx=%u %s %s %s network_ready=%u\n",
        w.running && wcn_wmi_ready() ? "WMI-ready" : w.stage ? w.stage : "none", w.attached, w.running, w.hw, w.subsystem,
        (unsigned long long)w.bar_phys, w.attached ? ee() : 0,
        w.attached ? reg(0x48) : 0, w.attached ? reg(w.bhi + 0x2c) : 0,
        w.attached ? reg(w.bhi + 0x30) : 0, (unsigned long long)w.dma_phys,
        w.events, w.last_event, w.last_data, w.ipcr_ready, w.rx_packets, w.tx_packets, qmi, ce_status, dp,wcn_sta_link_ready());
}
