/* SPDX-License-Identifier: GPL-2.0-only */
/* Bounded QRTR v1/v2 framing and the ath11k WLFW control handshake.
 * Wire reference: Linux net/qrtr and ath11k/qmi.{c,h}. */
#include "wcn_mhi.h"
#include "wcn_qmi.h"
#include "wcn_board.h"
#include "wcn_ce.h"
#include <ewoksys/kernel_tic.h>
#include <ewoksys/klog.h>
#include <ewoksys/dma.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define CTRL 0xfffffffeu
#define LOCAL_NODE 1u
#define LOCAL_PORT 0x4000u
#define MAX_SEGMENTS 16u
static struct { void *va; uint64_t pa; uint32_t size, type; } memory[MAX_SEGMENTS];
static unsigned memory_count;
static struct { uint8_t *data; unsigned size, offset, sequence, chunk, type; } upload;
static void *m3_va;
static uint64_t m3_pa;
static unsigned m3_size;
static struct {
    uint32_t node, port, packets, malformed;
    uint16_t transaction, pending, pending_id, last_id, result, error;
    unsigned hello, registered, host_cap, memory_requests;
    unsigned memory_ready, chip, board, soc, fw;
    unsigned target_cap, bdf_ready, m3_ready, firmware_ready;
    unsigned ce_config, mode_ready, failed;
    uint64_t deadline;
} q;

static uint16_t get16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | ((uint32_t)get16(p + 2) << 16); }
static void set16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void set32(uint8_t *p, uint32_t v) { set16(p, v); set16(p + 2, v >> 16); }

static int send_packet(unsigned type, uint32_t node, uint32_t port,
                       uint32_t source, const void *payload, unsigned bytes) {
    uint8_t packet[8192];
    if (bytes > sizeof(packet) - 32) return -1;
    unsigned padded = (bytes + 3) & ~3u;
    memset(packet, 0, 32 + padded);
    set32(packet, 1); set32(packet + 4, type);
    set32(packet + 8, LOCAL_NODE); set32(packet + 12, source);
    set32(packet + 20, bytes);
    set32(packet + 24, node); set32(packet + 28, port);
    memcpy(packet + 32, payload, bytes);
    return wcn_ipcr_send(packet, 32 + padded);
}

static int send_control(unsigned type, uint32_t node, uint32_t arg1, uint32_t arg2) {
    uint8_t payload[20] = {0};
    set32(payload, type); set32(payload + 4, arg1); set32(payload + 8, arg2);
    return send_packet(type, node, CTRL, CTRL, payload, sizeof(payload));
}

static unsigned tlv(uint8_t *p, unsigned type, unsigned value, unsigned size) {
    p[0] = type; set16(p + 1, size);
    for (unsigned i = 0; i < size; i++) p[3 + i] = value >> (8 * i);
    return size + 3;
}

static int request(unsigned id, const void *payload, unsigned bytes) {
    uint8_t packet[7168];
    if (!q.port || q.pending || bytes > sizeof(packet) - 7) return -1;
    uint16_t txn = ++q.transaction;
    if (!txn) txn = ++q.transaction;
    packet[0] = 0; set16(packet + 1, txn); set16(packet + 3, id);
    set16(packet + 5, bytes); memcpy(packet + 7, payload, bytes);
    if (send_packet(1, q.node, q.port, LOCAL_PORT, packet, bytes + 7)) return -1;
    q.pending = txn; q.pending_id = id;
    q.deadline = kernel_tic_ms(0) + 20000;
    klog("wcn: QMI request id=%x txn=%u bytes=%u\n", id, txn, bytes);
    return 0;
}

static int send_m3(void) {
    uint8_t *file = wcn_read_file("/etc/firmware/ath11k/WCN6855/hw2.1/m3.bin", 1024 * 1024, &m3_size);
    if (!file || m3_va) { free(file); return -1; }
    unsigned allocation = (m3_size + 0x3fff) & ~0x3fffu;
    m3_va = (void *)dma_alloc(0, allocation);
    if (!m3_va) { free(file); return -1; }
    m3_pa = dma_phy_addr(0, (ewokos_addr_t)m3_va);
    if (!m3_pa || m3_pa + allocation > 0x100000000ULL) { free(file); return -1; }
    memcpy(m3_va, file, m3_size); free(file);
    __asm__ volatile("dsb sy" ::: "memory");
    uint8_t p[18]; p[0] = 1; set16(p + 1, 8);
    set32(p + 3, m3_pa); set32(p + 7, m3_pa >> 32);
    p[11] = 2; set16(p + 12, 4); set32(p + 14, m3_size);
    klog("wcn: M3 bytes=%u pa=%llx\n", m3_size, (unsigned long long)m3_pa);
    return request(0x3c, p, sizeof(p));
}

static int upload_segment(void) {
    if (!upload.data || upload.offset >= upload.size) return -1;
    uint8_t p[6400]; unsigned n = 0;
    unsigned remaining = upload.size - upload.offset;
    upload.chunk = remaining > 6144 ? 6144 : remaining;
    n += tlv(p + n, 1, 1, 1);
    n += tlv(p + n, 0x10, q.board, 4);
    n += tlv(p + n, 0x11, remaining, 4);
    n += tlv(p + n, 0x12, upload.sequence, 4);
    p[n] = 0x13; set16(p + n + 1, upload.chunk + 2);
    set16(p + n + 3, upload.chunk);
    memcpy(p + n + 5, upload.data + upload.offset, upload.chunk); n += upload.chunk + 5;
    n += tlv(p + n, 0x14, upload.chunk == remaining, 1);
    n += tlv(p + n, 0x15, upload.type, 1);
    return request(0x25, p, n);
}

static int begin_board_upload(int regdb) {
    if (upload.data) return -1;
    upload.data = wcn_board_data(q.chip, q.board, regdb, &upload.size);
    if (!upload.data) return -1;
    upload.offset = upload.sequence = 0;
    upload.type = regdb ? 4 : (upload.size >= 4 && !memcmp(upload.data, "\177ELF", 4) ? 1 : 0);
    klog("wcn: uploading %s type=%u bytes=%u\n", regdb ? "regdb" : "BDF", upload.type, upload.size);
    return upload_segment();
}

static void register_indications(void) {
    uint8_t p[64]; unsigned n = 0;
    const unsigned enables[] = {0x10, 0x16, 0x17, 0x18, 0x1b};
    for (unsigned i = 0; i < sizeof(enables) / sizeof(enables[0]); i++)
        n += tlv(p + n, enables[i], 1, 1);
    n += tlv(p + n, 0x15, 0x4b4e454c, 4);
    (void)request(0x20, p, n);
}

static void host_capabilities(void) {
    uint8_t p[64]; unsigned n = 0;
    n += tlv(p + n, 0x10, 1, 4); /* One host client. */
    /* WCN6855 uses the global reset sequence in wcn_mhi.c. Match ath11k's
     * sleep-clock, host C-state, global-reset and PME capability bits. */
    n += tlv(p + n, 0x13, 0x1e, 1);
    n += tlv(p + n, 0x14, 1, 1); /* Board file support. */
    n += tlv(p + n, 0x16, 1, 1); /* M3 supported. */
    n += tlv(p + n, 0x17, 1, 1); /* Host retains M3. */
    n += tlv(p + n, 0x1a, 0, 1); /* Calibration not completed yet. */
    n += tlv(p + n, 0x1c, 0, 1); /* Default target memory mode. */
    (void)request(0x34, p, n);
}

static int respond_memory(const uint8_t *p, unsigned bytes) {
    uint32_t sizes[MAX_SEGMENTS], types[MAX_SEGMENTS];
    if (!bytes || !p[0] || p[0] > MAX_SEGMENTS || memory_count) return -1;
    unsigned count = p[0], pos = 1, total = 0;
    /* Validate the complete request before allocating or publishing addresses. */
    for (unsigned i = 0; i < count; i++) {
        if (bytes - pos < 9) return -1;
        sizes[i] = get32(p + pos); types[i] = get32(p + pos + 4);
        unsigned cfgs = p[pos + 8]; pos += 9;
        klog("wcn: QMI requested segment=%u type=%u bytes=%u cfgs=%u\n", i, types[i], sizes[i], cfgs);
        /* The type is an opaque firmware tag (new releases also use 12/13).
         * Like ath11k, echo it while allocating ordinary host DDR. */
        if (!sizes[i] || sizes[i] > 32u * 1024 * 1024 || types[i] > 0x7fffffff || cfgs > 2)
            return -1;
        total += sizes[i];
        if (total > 64u * 1024 * 1024 || bytes - pos < cfgs * 13) return -1;
        for (unsigned c = 0; c < cfgs; c++) {
            /* Secure memory requires SCM ownership handling, not host DMA. */
            if (p[pos + 12]) return -1;
            pos += 13;
        }
    }
    if (pos != bytes) return -1;
    uint8_t reply[4 + MAX_SEGMENTS * 17];
    reply[0] = 1; set16(reply + 1, 1 + count * 17); reply[3] = count;
    for (unsigned i = 0; i < count; i++) {
        unsigned allocation = (sizes[i] + 0x3fff) & ~0x3fffu;
        /* Match the power-of-two alignment guarantee of Linux coherent DMA.
         * Page-only addresses made this firmware assert during DDR setup.
         * Keep the original allocation pointer for dma_free(). */
        unsigned alignment = 0x4000;
        while (alignment < allocation) alignment <<= 1;
        memory[i].va = dma_alloc(0, allocation + alignment - 0x4000);
        memory_count = i + 1;
        if (!memory[i].va) return -1;
        memory[i].pa = dma_phy_addr(0, (ewokos_addr_t)memory[i].va);
        uint64_t aligned = (memory[i].pa + alignment - 1) & ~((uint64_t)alignment - 1);
        unsigned padding = (unsigned)(aligned - memory[i].pa);
        memory[i].pa = aligned;
        if (!memory[i].pa || memory[i].pa + allocation > 0x100000000ULL) return -1;
        memory[i].size = sizes[i]; memory[i].type = types[i];
        memset((uint8_t *)memory[i].va + padding, 0, allocation);
        uint8_t *seg = reply + 4 + i * 17;
        set32(seg, memory[i].pa); set32(seg + 4, memory[i].pa >> 32);
        set32(seg + 8, sizes[i]); set32(seg + 12, types[i]); seg[16] = 0;
        klog("wcn: QMI memory type=%u bytes=%u pa=%llx\n", types[i], sizes[i],
             (unsigned long long)memory[i].pa);
    }
    __asm__ volatile("dsb sy" ::: "memory");
    return request(0x36, reply, 4 + count * 17);
}

static void receive_qmi(const uint8_t *p, unsigned bytes) {
    if (bytes < 7 || get16(p + 5) != bytes - 7) { q.malformed++; return; }
    unsigned id = get16(p + 3), txn = get16(p + 1);
    q.last_id = id;
    klog("wcn: QMI receive type=%u id=%x txn=%u bytes=%u\n", p[0], id, txn, bytes);
    if (p[0] == 4) {
        if (id == 0x35) {
            q.memory_requests++;
            for (unsigned i = 7; i + 3 <= bytes;) {
                unsigned len = get16(p + i + 1);
                if (len > bytes - i - 3) { q.malformed++; return; }
                if (p[i] == 1 && respond_memory(p + i + 3, len))
                    klog("wcn: QMI memory request rejected or allocation failed\n");
                i += 3 + len;
            }
        }
        if (id == 0x37) {
            q.memory_ready = 1;
            if (!q.pending) (void)request(0x24, "", 0);
        }
        if (id == 0x38) { q.firmware_ready = 1; klog("wcn: firmware initialization done; CE/HTC setup pending\n"); }
        return;
    }
    if (p[0] != 2 || !q.pending || txn != q.pending || id != q.pending_id) return;
    int found = 0;
    for (unsigned i = 7; i < bytes;) {
        if (bytes - i < 3) { q.malformed++; return; }
        unsigned len = get16(p + i + 1);
        if (len > bytes - i - 3) { q.malformed++; return; }
        if (p[i] == 2 && len == 4 && !found) {
            q.result = get16(p + i + 3); q.error = get16(p + i + 5); found = 1;
        }
        if (id == 0x24) {
            if (p[i] == 0x10 && len == 8) q.chip = get32(p + i + 3);
            if (p[i] == 0x11 && len == 4) q.board = get32(p + i + 3);
            if (p[i] == 0x12 && len == 4) q.soc = get32(p + i + 3);
            if (p[i] == 0x13 && len >= 4) q.fw = get32(p + i + 3);
        }
        i += 3 + len;
    }
    if (!found) { q.malformed++; return; }
    q.pending = 0;
    klog("wcn: QMI response id=%x result=%u error=%u\n", id, q.result, q.error);
    if (q.result) { q.failed = 1; return; }
    if (id == 0x20) { q.registered = 1; host_capabilities(); }
    if (id == 0x34) q.host_cap = 1;
    if (id == 0x36 && q.memory_ready) (void)request(0x24, "", 0);
    if (id == 0x24) {
        q.target_cap = 1;
        klog("wcn: QMI target chip=%x board=%x soc=%x firmware=%08x\n", q.chip, q.board, q.soc, q.fw);
        /* PCI devices already have their BAR; DEVICE_INFO is hybrid-bus only. */
        if (begin_board_upload(1) && begin_board_upload(0))
            klog("wcn: board upload could not start\n");
    }
    if (id == 0x25 && upload.data) {
        upload.offset += upload.chunk; upload.sequence++;
        if (upload.offset < upload.size) {
            (void)upload_segment();
        } else {
            unsigned type = upload.type;
            klog("wcn: board upload complete type=%u bytes=%u\n", type, upload.size);
            free(upload.data); memset(&upload, 0, sizeof(upload));
            if (type == 4) {
                if (begin_board_upload(0)) klog("wcn: BDF upload could not start\n");
            } else {
                q.bdf_ready = 1;
                if (send_m3()) klog("wcn: M3 setup failed\n");
            }
        }
    }
    if (id == 0x3c) q.m3_ready = 1;
    if (id == 0x23) q.ce_config = 2;
    if (id == 0x22) q.mode_ready = 2;
}

void wcn_qmi_receive(const void *packet, unsigned bytes) {
    const uint8_t *p = packet;
    unsigned hdr, type, confirm, size;
    uint32_t node, port, dest_port;
    if (!bytes) { q.malformed++; return; }
    if (p[0] == 1 && bytes >= 32 && get32(p) == 1) {
        hdr = 32; type = get32(p + 4); node = get32(p + 8); port = get32(p + 12);
        confirm = get32(p + 16); size = get32(p + 20); dest_port = get32(p + 28);
    } else if (p[0] == 3 && bytes >= 16) {
        hdr = 16 + p[3]; type = p[1]; confirm = p[2] & 1; size = get32(p + 4);
        node = get16(p + 8); port = get16(p + 10); dest_port = get16(p + 14);
        if (port == 0xffff) port = CTRL;
        if (dest_port == 0xffff) dest_port = CTRL;
    } else { q.malformed++; return; }
    if (hdr > bytes || size > bytes - hdr || ((size + 3u) & ~3u) != bytes - hdr) {
        q.malformed++; return;
    }
    q.packets++;
    const uint8_t *body = p + hdr;
    if (type == 2 && dest_port == CTRL && size >= 4 && get32(body) == 2) {
        if (!q.hello) {
            q.hello = 1;
            (void)send_control(2, node, 0, 0);
            klog("wcn: QRTR hello from node=%u\n", node);
        }
    } else if (type == 4 && dest_port == CTRL && size >= 20 && get32(body) == 4) {
        unsigned service = get32(body + 4), instance = get32(body + 8);
        klog("wcn: QRTR service=%x instance=%x node=%u port=%u\n",
             service, instance, get32(body + 12), get32(body + 16));
        if (service == 0x45 && (instance & 255) == 1 && !q.port) {
            q.node = get32(body + 12); q.port = get32(body + 16);
            register_indications();
        }
    } else if (type == 1 && dest_port == LOCAL_PORT && node == q.node && port == q.port) {
        receive_qmi(body, size);
    }
    if (confirm) (void)send_control(7, node, LOCAL_NODE, dest_port);
}

void wcn_qmi_reset(void) { memset(&q, 0, sizeof(q)); }
void wcn_qmi_release(void) {
    for (unsigned i = 0; i < memory_count; i++)
        if (memory[i].va) dma_free(0, (ewokos_addr_t)memory[i].va);
    memset(memory, 0, sizeof(memory)); memory_count = 0;
    free(upload.data); memset(&upload, 0, sizeof(upload));
    if (m3_va) dma_free(0, (ewokos_addr_t)m3_va);
    m3_va = NULL; m3_pa = m3_size = 0;
}
void wcn_qmi_begin(void) {
    wcn_qmi_reset(); q.board = 255;
    (void)send_control(2, 0xffffffffu, 0, 0);
}

static int configure_ce(void) {
    static const uint32_t targets[9][5] = {
        {0,2,32,2048,0}, {1,1,32,2048,0}, {2,1,32,2048,0},
        {3,2,32,2048,0}, {4,2,256,256,8}, {5,1,32,2048,0},
        {6,3,32,16384,0}, {7,4,0,0,8}, {8,3,32,16384,0}
    };
    uint8_t p[512]; unsigned n = 0;
    p[n++] = 0x11; set16(p+n, 1 + sizeof(targets)); n += 2;
    p[n++] = 9;
    for (unsigned i = 0; i < 9; i++)
        for (unsigned j = 0; j < 5; j++, n += 4) set32(p+n, targets[i][j]);
    /* WMI data/control, endpoint zero and HTT service mappings. */
    static const unsigned services[] = {0x104,0x102,0x101,0x103,0x100,1,0x300};
    p[n++] = 0x12; set16(p+n, 1 + 14 * 12); n += 2; p[n++] = 14;
    for (unsigned i = 0; i < 7; i++) {
        for (unsigned direction = 2; direction; direction--) {
            unsigned pipe = i < 5 ? (direction == 2 ? 3 : 2) :
                i == 5 ? (direction == 2 ? 0 : 2) : (direction == 2 ? 4 : 1);
            set32(p+n, services[i]); set32(p+n+4, direction); set32(p+n+8, pipe); n += 12;
        }
    }
    return request(0x23, p, n);
}

void wcn_qmi_poll(void) {
    if (q.failed) return;
    if (q.pending) {
        if (kernel_tic_ms(0) >= q.deadline) {
            klog("wcn: QMI timeout id=%x txn=%u\n", q.pending_id, q.pending);
            q.failed = 1;
        }
        return;
    }
    if (!q.firmware_ready || !q.m3_ready) return;
    if (!q.ce_config) {
        if (wcn_ce_init() || configure_ce()) { q.failed = 1; return; }
        q.ce_config = 1;
    } else if (q.ce_config == 2 && !q.mode_ready) {
        uint8_t p[11]; unsigned n = tlv(p, 1, 0, 4);
        n += tlv(p+n, 0x10, 0, 1);
        if (request(0x22, p, n)) { q.failed = 1; return; }
        q.mode_ready = 1;
    }
}
void wcn_qmi_status(char *out, size_t size) {
    snprintf(out, size, "qmi=%u:%u registered=%u host_cap=%u mem_ready=%u chip=%x board=%x bdf=%u m3=%u fw_ready=%u cfg=%u mode=%u pending=%x malformed=%u qmi_failed=%u",
             q.node, q.port, q.registered, q.host_cap, q.memory_ready, q.chip, q.board,
             q.bdf_ready, q.m3_ready, q.firmware_ready, q.ce_config, q.mode_ready,
             q.pending ? q.pending_id : 0, q.malformed, q.failed);
}
