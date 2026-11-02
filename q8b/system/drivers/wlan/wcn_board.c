/* SPDX-License-Identifier: GPL-2.0-only */
/* Select the board-2 container entry using the PCI and QMI identity. */
#include "wcn_board.h"
#include "wcn_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define FW_DIR "/etc/firmware/ath11k/WCN6855/hw2.1/"
static uint16_t pci_subsystem_vendor = 0x17cb;
static uint16_t pci_subsystem_device = 0x0108;

void wcn_board_set_subsystem(uint16_t vendor, uint16_t device) {
    pci_subsystem_vendor = vendor;
    pci_subsystem_device = device;
}
static uint32_t word(const uint8_t *p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint8_t *wcn_read_file(const char *path, unsigned limit, unsigned *size) {
    *size = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    off_t end = lseek(fd, 0, SEEK_END);
    if (end <= 0 || end > limit || lseek(fd, 0, SEEK_SET) < 0) { close(fd); return NULL; }
    uint8_t *data = malloc(end);
    if (!data) { close(fd); return NULL; }
    unsigned pos = 0;
    while (pos < (unsigned)end) {
        int n = read(fd, data + pos, end - pos);
        if (n <= 0) { close(fd); free(data); return NULL; }
        pos += n;
    }
    close(fd); *size = pos; return data;
}

static const uint8_t *select_entry(const uint8_t *data, unsigned bytes,
                                  const char *name, unsigned outer, unsigned *size) {
    static const char magic[] = "QCA-ATH11K-BOARD";
    unsigned pos = (sizeof(magic) + 3) & ~3u;
    if (bytes < pos || memcmp(data, magic, sizeof(magic))) return NULL;
    while (bytes - pos >= 8) {
        unsigned id = word(data + pos), n = word(data + pos + 4); pos += 8;
        if (n > bytes - pos || n > 0xfffffffcu) return NULL;
        unsigned padded = (n + 3) & ~3u;
        if (padded > bytes - pos) return NULL;
        if (id == outer) {
            unsigned off = 0; int match = 0;
            while (n - off >= 8) {
                unsigned type = word(data + pos + off), len = word(data + pos + off + 4);
                off += 8;
                if (len > n - off || len > 0xfffffffcu || ((len + 3) & ~3u) > n - off) return NULL;
                const uint8_t *value = data + pos + off;
                if (type == 0 && strlen(name) == len && !memcmp(value, name, len)) match = 1;
                if (type == 1 && match && len && len <= 256 * 1024) {
                    *size = len; return value;
                }
                off += (len + 3) & ~3u;
            }
        }
        pos += padded;
    }
    return NULL;
}

/* Some WCN6855 cards use a new subsystem device number before the matching
 * board-2.bin entry is shipped.  The subsystem vendor and QMI identity still
 * select the board calibration family, so permit a deterministic fallback to
 * an entry from that vendor.  Keep this separate from the exact lookup above:
 * an exact match always wins and an unrelated vendor is never selected. */
static const uint8_t *select_vendor_entry(const uint8_t *data, unsigned bytes,
        uint16_t vendor, unsigned chip, unsigned board, unsigned outer,
        unsigned *size) {
    static const char magic[] = "QCA-ATH11K-BOARD";
    char vendor_tag[32], qmi_tag[64], candidate[256];
    unsigned pos = (sizeof(magic) + 3) & ~3u;
    int n = snprintf(vendor_tag, sizeof(vendor_tag),
            "subsystem-vendor=%04x,", vendor);
    int q = snprintf(qmi_tag, sizeof(qmi_tag),
            "qmi-chip-id=%u,qmi-board-id=%u", chip, board);
    if (n <= 0 || (size_t)n >= sizeof(vendor_tag) || q <= 0 ||
            (size_t)q >= sizeof(qmi_tag) || bytes < pos ||
            memcmp(data, magic, sizeof(magic)))
        return NULL;
    while (bytes - pos >= 8) {
        unsigned id = word(data + pos), nbytes = word(data + pos + 4);
        pos += 8;
        if (nbytes > bytes - pos || nbytes > 0xfffffffcu)
            return NULL;
        unsigned padded = (nbytes + 3) & ~3u;
        if (padded > bytes - pos)
            return NULL;
        if (id == outer) {
            unsigned off = 0;
            int match = 0;
            while (nbytes - off >= 8) {
                unsigned type = word(data + pos + off);
                unsigned len = word(data + pos + off + 4);
                off += 8;
                if (len > nbytes - off || len > 0xfffffffcu ||
                        ((len + 3) & ~3u) > nbytes - off)
                    return NULL;
                const uint8_t *value = data + pos + off;
                if (type == 0 && len < sizeof(candidate)) {
                    memcpy(candidate, value, len);
                    candidate[len] = 0;
                    match = !strncmp(candidate,
                            "bus=pci,vendor=17cb,device=1103,", 32) &&
                            strstr(candidate, vendor_tag) != NULL &&
                            strstr(candidate, qmi_tag) != NULL;
                }
                if (type == 1 && match && len && len <= 256 * 1024) {
                    *size = len;
                    return value;
                }
                off += (len + 3) & ~3u;
            }
        }
        pos += padded;
    }
    return NULL;
}

uint8_t *wcn_board_data(unsigned chip, unsigned board, int regdb, unsigned *size) {
    unsigned bytes; uint8_t *file = wcn_read_file(FW_DIR "board-2.bin", 16 * 1024 * 1024, &bytes);
    const uint8_t *entry = NULL;
    char name[192];
    snprintf(name, sizeof(name), "bus=pci,vendor=17cb,device=1103,subsystem-vendor=%04x,subsystem-device=%04x,qmi-chip-id=%u,qmi-board-id=%u",
            pci_subsystem_vendor, pci_subsystem_device, chip, board);
    if (file) entry = select_entry(file, bytes, name, regdb ? 1 : 0, size);
    if (!entry && !regdb && file) {
        /* The new 105b:e130 card is absent from older board-2.bin files.
         * Reuse the first calibration entry for the same subsystem vendor,
         * chip and board identity instead of refusing BDF upload entirely. */
        entry = select_vendor_entry(file, bytes, pci_subsystem_vendor,
                chip, board, 0, size);
        if (entry)
            wcn_log("wcn: BDF exact subsystem %04x:%04x missing; using vendor calibration fallback\n",
                    pci_subsystem_vendor, pci_subsystem_device);
    }
    if (!entry && regdb && file) entry = select_entry(file, bytes, "bus=pci", 1, size);
    uint8_t *result = NULL;
    if (entry) {
        result = malloc(*size);
        if (result) memcpy(result, entry, *size);
    }
    free(file);
    if (!result && regdb) result = wcn_read_file(FW_DIR "regdb.bin", 256 * 1024, size);
    if (!result) { *size = 0; wcn_log("wcn: missing %s for %s\n", regdb ? "regdb" : "BDF", name); }
    return result;
}
