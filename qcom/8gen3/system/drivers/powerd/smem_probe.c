/*
 * smem_probe.c - read-only SMEM inspector for the ADSP PMIC GLINK channel
 * bring-up (powerd --smem).
 *
 * Purpose: before implementing any ADSP PAS boot we need one fact from the
 * device - does the boot firmware already leave the ADSP running with a
 * published GLINK edge?  If yes, the kernel only needs a GLINK client and
 * no PAS work at all; if no, the boot path in bsp/batt_glink.c has to load
 * adsp.mdt itself.
 *
 * Layouts are the upstream Linux ones (drivers/soc/qcom/smem.c and
 * drivers/rpmsg/qcom_glink_smem.c):
 *   - smem_header at the region base: proc_comm[4] (32 B), version[32]
 *     (128 B), initialized/free_offset/available/reserved (16 B), then
 *     512 global TOC entries of 16 B (allocated/offset/size/aux_base).
 *   - The partition table ("$TOC") lives in the LAST 4 KB of the region
 *     (qcom_smem_map_toc): magic/version/num_entries/5 reserved words,
 *     then 16-byte entries (offset/size/flags/host0/host1/cacheline).
 *   - Each partition starts with "$PRT" (magic 0x24 0x50 0x52 0x54),
 *     host0/host1, size, offset_free_uncached/cached, 3 reserved words,
 *     followed by 16-byte uncached item headers
 *     (canary 0xa5a5 / item / size / padding_data / padding_hdr / reserved).
 *   - A GLINK edge publishes item 478 (32-byte xprt descriptor: tx tail,
 *     tx head, rx tail, rx head) plus FIFO items 479 (16 KB) and 480.
 *
 * Everything is a plain load from a read-only mapping - no writes, no SPMI
 * access, so this cannot wedge the PMIC bus.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <ewoksys/syscall.h>
#include <syscalls.h>

#include "smem_probe.h"

#define SMEM_PHY    0x80900000ull   /* sc8280xp.dtsi smem@80900000 */
#define SMEM_SIZE   0x00200000u     /* 2 MB reserved region */
#define SMEM_VA     0x5a200000ull   /* free window (SPMI lives at 0x5a08..0x5a14) */

#define SMEM_TOC_OFF        0x00b0u               /* after 32+128+16 bytes */
#define SMEM_TOC_COUNT      512u
#define SMEM_TOC_ENTRY      16u
#define SMEM_PTABLE_SIZE    0x1000u
#define SMEM_GLINK_DESC     478u
#define SMEM_GLINK_FIFO_TX  479u
#define SMEM_GLINK_FIFO_RX  480u

static uint8_t* _smem;
static bool _mapped;

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
            ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const uint8_t* p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

/* Scan the whole region for the strings that name GLINK edges and the
 * battery service.  A hit proves the record was published by some side
 * (boot firmware, ADSP, or a previous Linux boot). */
static void scan_strings(void) {
    static const char* const needles[] = {
        "PMIC_RTR_ADSP_APPS", "fastrpcglink-apps-dsp", "adsp_apps",
        "charger_pd", "tms/servreg", "glink",
    };
    unsigned found = 0;

    printf("smem: string scan (2 MB, read-only):\n");
    for (unsigned n = 0; n < sizeof(needles) / sizeof(needles[0]); n++) {
        const char* s = needles[n];
        size_t len = strlen(s);
        unsigned hits = 0;
        for (uint32_t off = 0; off + len <= SMEM_SIZE; off++) {
            if (_smem[off] != (uint8_t)s[0])
                continue;
            if (memcmp(_smem + off, s, len) != 0)
                continue;
            if (hits < 4)
                printf("  hit \"%s\" at +0x%06x\n", s, off);
            hits++;
        }
        printf("  \"%s\": %u hit(s)\n", s, hits);
        found += hits;
    }
    if (!found)
        printf("  no GLINK/battmgr strings present in SMEM\n");
}

/* Print the global TOC: allocated entries with offset/size. */
static void dump_global_toc(void) {
    unsigned used = 0;

    printf("smem: global TOC (heap items):\n");
    for (uint32_t i = 0; i < SMEM_TOC_COUNT; i++) {
        const uint8_t* e = _smem + SMEM_TOC_OFF + i * SMEM_TOC_ENTRY;
        uint32_t alloc = rd32(e), off = rd32(e + 4), size = rd32(e + 8);
        if (!alloc)
            continue;
        printf("  item %3u offset=0x%06x size=0x%06x\n", i, off, size);
        used++;
    }
    printf("smem: %u allocated heap item(s)\n", used);
}

static void dump_item_list(const uint8_t* phdr, uint32_t part_size) {
    const uint8_t* e = phdr + 32u;  /* sizeof(struct smem_partition_header) */
    const uint8_t* end = phdr + rd32(phdr + 12);  /* offset_free_uncached */
    unsigned count = 0;

    while (e + 16u <= end && e + 16u <= phdr + part_size) {
        uint32_t item;
        if (rd16(e) != 0xa5a5u) {
            printf("    (canary break at +0x%04x)\n",
                    (unsigned)(e - phdr));
            break;
        }
        item = rd16(e + 2);
        uint32_t size = rd32(e + 4);
        printf("    item %3u size=0x%06x%s%s%s\n", item, size,
                item == SMEM_GLINK_DESC ? " [GLINK descriptor]" : "",
                item == SMEM_GLINK_FIFO_TX ? " [GLINK TX fifo]" : "",
                item == SMEM_GLINK_FIFO_RX ? " [GLINK RX fifo]" : "");
        count++;
        if (count >= 64) {
            printf("    ... (more items truncated)\n");
            break;
        }
        e += 16u + rd16(e + 12) + size;  /* padding_hdr + size */
    }
    if (!count)
        printf("    (no items in this partition)\n");
}

static void dump_ptable(void) {
    const uint8_t* p = _smem + SMEM_SIZE - SMEM_PTABLE_SIZE;
    uint32_t version, entries;

    if (memcmp(p, "$TOC", 4) != 0) {
        printf("smem: no \"$TOC\" at region end (offset +0x%x) - layout is "
                "the global-heap variant or the region size differs\n",
                SMEM_SIZE - SMEM_PTABLE_SIZE);
        return;
    }
    version = rd32(p + 4);
    entries = rd32(p + 8);
    printf("smem: partition table \"$TOC\" version=%u entries=%u\n",
            version, entries);
    for (uint32_t i = 0; i < entries && i < 16u; i++) {
        const uint8_t* e = p + 32u + i * 16u;  /* ptable header is 32 B */
        uint32_t off = rd32(e), size = rd32(e + 4);
        uint16_t host0 = rd16(e + 12), host1 = rd16(e + 14);
        if (!off)
            continue;
        printf("  part %u: offset=0x%06x size=0x%06x host0=%u host1=%u\n",
                i, off, size, host0, host1);
        if (off + size > SMEM_SIZE || size < 32u) {
            printf("    (out of range, skipped)\n");
            continue;
        }
        const uint8_t* phdr = _smem + off;
        if (memcmp(phdr, "$PRT", 4) != 0) {
            printf("    bad partition magic\n");
            continue;
        }
        printf("    \"$PRT\" host0=%u host1=%u size=0x%x free_uncached=0x%x\n",
                rd16(phdr + 4), rd16(phdr + 6), rd32(phdr + 8),
                rd32(phdr + 12));
        dump_item_list(phdr, size);
        /* GLINK edge detail: descriptor + FIFO heads/tails. */
        /* (item lookup repeats the walk because items are not indexable) */
    }
}

/* Locate the GLINK descriptor in any partition and print its pointers. */
static void dump_glink_desc(void) {
    for (uint32_t part_off = 0x10000u; part_off < SMEM_SIZE - SMEM_PTABLE_SIZE;
            part_off += 0x1000u) {
        const uint8_t* phdr = _smem + part_off;
        if (memcmp(phdr, "$PRT", 4) != 0)
            continue;
        uint32_t part_size = rd32(phdr + 8);
        const uint8_t* e = phdr + 32u;
        const uint8_t* end = phdr + rd32(phdr + 12);
        while (e + 16u <= end && e + 16u <= phdr + part_size) {
            if (rd16(e) != 0xa5a5u)
                break;
            uint32_t item = rd16(e + 2);
            if (item == SMEM_GLINK_DESC) {
                const uint8_t* d = e + 16u + rd16(e + 12);
                printf("smem: GLINK descriptor @part+0x%x: "
                        "tx_tail=%u tx_head=%u rx_tail=%u rx_head=%u\n",
                        (unsigned)(d - phdr), rd32(d), rd32(d + 4),
                        rd32(d + 8), rd32(d + 12));
                return;
            }
            e += 16u + rd16(e + 12) + rd32(e + 4);
        }
    }
    printf("smem: no GLINK descriptor (item 478) found in any partition\n");
}

int smem_probe_run(void) {
    if (!_mapped) {
        if (syscall3(SYS_MEM_MAP, (ewokos_addr_t)SMEM_VA,
                (ewokos_addr_t)SMEM_PHY, (ewokos_addr_t)SMEM_SIZE) !=
                (ewokos_addr_t)SMEM_VA) {
            printf("smem: map of 0x%llx failed\n",
                    (unsigned long long)SMEM_PHY);
            return -1;
        }
        _smem = (uint8_t*)(uintptr_t)SMEM_VA;
        _mapped = true;
    }
    printf("smem: mapped 0x%llx..0x%llx at 0x%llx\n",
            (unsigned long long)SMEM_PHY,
            (unsigned long long)(SMEM_PHY + SMEM_SIZE),
            (unsigned long long)SMEM_VA);
    printf("smem: header initialized=%u free_offset=0x%x available=0x%x\n",
            rd32(_smem + 0xa0), rd32(_smem + 0xa4), rd32(_smem + 0xa8));
    printf("smem: version[] first non-zero: ");
    for (unsigned i = 0; i < 32; i++) {
        uint32_t v = rd32(_smem + 32 + i * 4);
        if (v) {
            printf("[%u]=0x%x ", i, v);
            if (i > 16)
                break;
        }
    }
    printf("\n");
    dump_global_toc();
    dump_ptable();
    dump_glink_desc();
    scan_strings();
    printf("smem: probe complete (read-only)\n");
    return 0;
}
