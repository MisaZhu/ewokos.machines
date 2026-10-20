#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <bsp/sdcc2.h>
#include <ewoksys/sys.h>
#include <ewoksys/syscall.h>
#include <syscalls.h>
#include <bsp/bsp_sd.h>
#include <sd/sd.h>
#include "q8b_platform.h"

#define GCC_PHY        0x00100000ULL
#define GCC_VA         0x58000000ULL
#define GCC_SIZE       0x00020000U
#define SDCC_WIN_PHY   0x08800000ULL
#define SDCC_WIN_VA    0x58100000ULL
#define SDCC_WIN_SIZE  0x00100000U
#define TLMM_PHY       0x0f100000ULL
#define TLMM_VA        0x58200000ULL
#define TLMM_SIZE      0x00300000U
#define RSC_PHY         0x18220000ULL
#define RSC_VA          0x58600000ULL
#define RSC_SIZE        0x00010000U

#define SDCC2_OFF      0x4000
#define GPIO131_CTL    (131 * 0x1000)
#define GPIO131_IO     (GPIO131_CTL + 4)

#define STRESS_BLOCKS          33U
#define GPT_HEADER_SIGNATURE   0x5452415020494645ULL
#define GPT_HEADER_MIN_SIZE    92U
#define GPT_ENTRY_BUFFER_SIZE  (32U * SECTOR_SIZE)

typedef struct {
    uint32_t time_low;
    uint16_t time_mid;
    uint16_t time_hi_and_version;
    uint8_t clock_seq_hi;
    uint8_t clock_seq_low;
    uint8_t node[6];
} __attribute__((packed)) gpt_guid_t;

typedef struct {
    uint64_t signature;
    uint32_t revision;
    uint32_t size;
    uint32_t crc32;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    gpt_guid_t disk_guid;
    uint64_t partition_entry_lba;
    uint32_t partition_entry_count;
    uint32_t partition_entry_size;
    uint32_t partition_entry_crc32;
} __attribute__((packed)) gpt_header_t;

typedef struct {
    gpt_guid_t type;
    gpt_guid_t partition_guid;
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[36];
} __attribute__((packed)) gpt_entry_t;

typedef struct {
    uint32_t inode_count;
    uint32_t block_count;
    uint32_t reserved_block_count;
    uint32_t free_block_count;
    uint32_t free_inode_count;
    uint32_t first_data_block;
    uint32_t log_block_size;
    uint32_t log_cluster_size;
    uint32_t blocks_per_group;
    uint32_t clusters_per_group;
    uint32_t inodes_per_group;
    uint32_t mount_time;
    uint32_t write_time;
    uint16_t mount_count;
    uint16_t max_mount_count;
    uint16_t magic;
    uint16_t state;
    uint16_t errors;
    uint16_t minor_revision;
    uint32_t last_check;
    uint32_t check_interval;
    uint32_t creator_os;
    uint32_t revision;
    uint16_t default_reserved_uid;
    uint16_t default_reserved_gid;
    uint32_t first_inode;
    uint16_t inode_size;
    uint16_t block_group_number;
    uint32_t feature_compat;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
    uint8_t uuid[16];
    char volume_name[16];
} __attribute__((packed)) ext_superblock_prefix_t;

static const gpt_guid_t _linux_filesystem_guid = {
    0x0fc63dafU, 0x8483U, 0x4772U, 0x8eU, 0x79U,
    {0x3dU, 0x69U, 0xd8U, 0x47U, 0x7dU, 0xe4U}
};

static uint8_t _blocks[STRESS_BLOCKS * SECTOR_SIZE];
static uint8_t _sector[SECTOR_SIZE];

static uint32_t rd32(uint64_t base, uint32_t off) {
    return *(volatile uint32_t*)(base + off);
}

static uint32_t rd16(uint64_t base, uint32_t off) {
    uint32_t val = rd32(base, off & ~3U);
    return (val >> ((off & 2U) * 8U)) & 0xffffU;
}

static int map_at(uint64_t va, uint64_t pa, uint32_t size) {
    return syscall3(SYS_MEM_MAP, (ewokos_addr_t)va,
            (ewokos_addr_t)pa, size) == (ewokos_addr_t)va ? 0 : -1;
}

static void dump(const char* name, uint64_t base, uint32_t off) {
    printf("sdprobe: %-12s [0x%08llx] = 0x%08x\n", name,
            (unsigned long long)(base + off), rd32(base, off));
}

static int physical_read(uint32_t lba, void* buf, uint32_t count) {
    return sd_read_sectors((int32_t)lba, buf, count) ==
            (int32_t)(count * 512U) ? 0 : -1;
}

static int guid_is_zero(const gpt_guid_t* guid) {
    static const gpt_guid_t zero;
    return memcmp(guid, &zero, sizeof(*guid)) == 0;
}

static int guid_is_linux_filesystem(const gpt_guid_t* guid) {
    return memcmp(guid, &_linux_filesystem_guid, sizeof(*guid)) == 0;
}

static void print_guid(const gpt_guid_t* guid) {
    printf("%08x-%04x-%04x-%02x%02x-", guid->time_low,
            guid->time_mid, guid->time_hi_and_version,
            guid->clock_seq_hi, guid->clock_seq_low);
    for (uint32_t i = 0; i < sizeof(guid->node); ++i)
        printf("%02x", guid->node[i]);
}

static void gpt_name_ascii(const gpt_entry_t* entry, char* out,
        uint32_t out_size) {
    const uint8_t* utf16 = (const uint8_t*)entry->name;
    uint32_t i;

    if (out_size == 0)
        return;
    for (i = 0; i + 1U < out_size && i < 36U; ++i) {
        uint32_t codepoint = (uint32_t)utf16[i * 2U] |
                ((uint32_t)utf16[i * 2U + 1U] << 8);
        if (codepoint == 0)
            break;
        out[i] = codepoint < 0x80U ? (char)codepoint : '?';
    }
    out[i] = 0;
}

static void inspect_ext_superblock(uint32_t partition, uint64_t start_lba) {
    const ext_superblock_prefix_t* superblock;
    char volume_name[17];

    if (start_lba > 0x7ffffffdULL ||
            physical_read((uint32_t)start_lba + 2U, _sector, 1U) != 0) {
        printf("sdprobe: GPT partition %u ext superblock read failed\n",
                partition);
        return;
    }
    superblock = (const ext_superblock_prefix_t*)_sector;
    if (superblock->magic != 0xef53U || superblock->log_block_size > 2U) {
        printf("sdprobe: GPT partition %u has no ext superblock (magic=0x%04x)\n",
                partition, superblock->magic);
        return;
    }
    memcpy(volume_name, superblock->volume_name, 16U);
    volume_name[16] = 0;
    printf("sdprobe: GPT partition %u ext valid block-size=%u blocks=%u "
            "inode-size=%u label='%s' features=%08x/%08x/%08x\n",
            partition, 1024U << superblock->log_block_size,
            superblock->block_count, superblock->inode_size, volume_name,
            superblock->feature_compat, superblock->feature_incompat,
            superblock->feature_ro_compat);
}

static void inspect_gpt(void) {
    const gpt_header_t* header;
    const uint8_t* entries;
    uint64_t entry_bytes;
    uint32_t entry_sectors;
    uint32_t linux_partitions = 0;

    if (physical_read(1U, _blocks, STRESS_BLOCKS) != 0) {
        printf("sdprobe: 33-sector CMD18 read failed\n");
        return;
    }
    if (physical_read(STRESS_BLOCKS, _sector, 1U) != 0 ||
            memcmp(_sector, _blocks + (STRESS_BLOCKS - 1U) * SECTOR_SIZE,
                SECTOR_SIZE) != 0) {
        printf("sdprobe: 32+1 boundary verification failed\n");
        return;
    }
    printf("sdprobe: 33-sector CMD18 (32+1) verified\n");

    header = (const gpt_header_t*)_blocks;
    if (header->signature != GPT_HEADER_SIGNATURE ||
            header->size < GPT_HEADER_MIN_SIZE ||
            header->size > SECTOR_SIZE || header->my_lba != 1U) {
        printf("sdprobe: invalid GPT header\n");
        return;
    }
    entry_bytes = (uint64_t)header->partition_entry_count *
            header->partition_entry_size;
    if (header->partition_entry_count == 0 ||
            header->partition_entry_size < sizeof(gpt_entry_t) ||
            entry_bytes > GPT_ENTRY_BUFFER_SIZE) {
        printf("sdprobe: unsupported GPT table count=%u entry-size=%u\n",
                header->partition_entry_count, header->partition_entry_size);
        return;
    }
    entry_sectors = (uint32_t)((entry_bytes + SECTOR_SIZE - 1U) /
            SECTOR_SIZE);
    printf("sdprobe: GPT entries=%u size=%u table-lba=%llu usable=%llu..%llu\n",
            header->partition_entry_count, header->partition_entry_size,
            (unsigned long long)header->partition_entry_lba,
            (unsigned long long)header->first_usable_lba,
            (unsigned long long)header->last_usable_lba);

    if (header->partition_entry_lba == 2U) {
        entries = _blocks + SECTOR_SIZE;
    } else {
        if (header->partition_entry_lba > 0x7fffffffULL ||
                physical_read((uint32_t)header->partition_entry_lba,
                    _blocks, entry_sectors) != 0) {
            printf("sdprobe: GPT entry read failed\n");
            return;
        }
        entries = _blocks;
    }

    for (uint32_t i = 0; i < header->partition_entry_count; ++i) {
        const gpt_entry_t* entry = (const gpt_entry_t*)(entries +
                (uint64_t)i * header->partition_entry_size);
        char name[37];

        if (guid_is_zero(&entry->type))
            continue;
        gpt_name_ascii(entry, name, sizeof(name));
        printf("sdprobe: GPT partition %u type=", i + 1U);
        print_guid(&entry->type);
        printf(" lba=%llu..%llu name='%s'%s\n",
                (unsigned long long)entry->first_lba,
                (unsigned long long)entry->last_lba, name,
                guid_is_linux_filesystem(&entry->type) ?
                " [linux-filesystem]" : "");
        if (guid_is_linux_filesystem(&entry->type))
            ++linux_partitions;
        if (guid_is_linux_filesystem(&entry->type))
            inspect_ext_superblock(i + 1U, entry->first_lba);
    }
    printf("sdprobe: Linux filesystem GPT partitions=%u\n",
            linux_partitions);
}

static uint8_t _t_ref[32U * 512U];
static uint8_t _t_out[32U * 512U];
static uint8_t _t_pat[4U * 512U];
static uint8_t _t_orig[4U * 512U];

static void full_test(void) {
    uint64_t sectors = sdcc2_capacity_sectors();
    struct timeval t0;
    struct timeval t1;
    uint64_t us;
    uint32_t ok;
    static const uint32_t counts[5] = { 2U, 4U, 8U, 16U, 32U };

    printf("sdtest: ==== full SD capability test ====\n");
    printf("sdtest: capacity=%u sectors\n", (unsigned)sectors);
    if (sectors == 0)
        return;

    /* 1MB sustained single-block read. */
    printf("sdtest: 1MB single-block read start\n");
    gettimeofday(&t0, NULL);
    ok = 1;
    for (uint32_t i = 0; i < 2048U; i++) {
        if (sdcc2_read_sector(2048 + i, _t_out) != 0) {
            ok = 0;
            break;
        }
    }
    gettimeofday(&t1, NULL);
    us = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000ULL +
            (uint64_t)(t1.tv_usec - t0.tv_usec);
    printf("sdtest: 1MB single read ok=%u us=%llu KB/s=%llu\n",
            (unsigned)ok, (unsigned long long)us,
            (unsigned long long)(us != 0 ? (1024ULL * 1000000ULL) / us : 0));

    /* CMD18 multi-block reads, compared against single-block references. */
    for (uint32_t n = 0; n < 5U; n++) {
        uint32_t cnt = counts[n];
        uint32_t rc1;
        uint32_t rc2;

        printf("sdtest: multi read cnt=%u start\n", (unsigned)cnt);
        rc1 = (uint32_t)sdcc2_read_blocks_test(2048, _t_ref, cnt, 0);
        printf("sdtest: multi cnt=%u single rc=%u\n", (unsigned)cnt,
                (unsigned)rc1);
        if (rc1 != 0)
            continue;
        gettimeofday(&t0, NULL);
        rc2 = (uint32_t)sdcc2_read_blocks_test(2048, _t_out, cnt, 1);
        gettimeofday(&t1, NULL);
        us = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000ULL +
                (uint64_t)(t1.tv_usec - t0.tv_usec);
        printf("sdtest: multi cnt=%u cmd18 rc=%u cmp=%d us=%llu KB/s=%llu\n",
                (unsigned)cnt, (unsigned)rc2,
                rc2 == 0 ? memcmp(_t_ref, _t_out, cnt * 512U) : -2,
                (unsigned long long)us,
                (unsigned long long)(us != 0 ?
                    ((uint64_t)cnt * 512ULL * 1000000ULL) / us / 1024ULL : 0));
    }

    /* Production path: inherited multi flag + ADMA2, no test hook. */
    printf("sdtest: inherited multi read 32 start\n");
    {
        uint32_t rc = (uint32_t)sdcc2_read_blocks(2048, _t_out, 32U);
        printf("sdtest: inherited multi 32 rc=%u cmp=%d\n", (unsigned)rc,
                rc == 0 ? memcmp(_t_ref, _t_out, 32U * 512U) : -2);
    }

    /* Write/readback/restore in the unallocated area after partition 2
     * (partition 2 ends at LBA 5244927; use sectors/2 ~= 30.5M). */
    {
        uint32_t ws = (uint32_t)(sectors / 2ULL);
        uint32_t rc;

        if (ws < 6000000U)
            ws = 6000000U;
        printf("sdtest: write sector=%u start\n", (unsigned)ws);
        rc = (uint32_t)sdcc2_read_blocks_test((int32_t)ws, _t_orig, 4U, 0);
        printf("sdtest: write backup rc=%u\n", (unsigned)rc);
        if (rc != 0)
            return;
        for (uint32_t i = 0; i < 4U * 512U; i++)
            _t_pat[i] = (uint8_t)(0xA5U ^ (uint8_t)(i * 7U + ws));
        rc = (uint32_t)sdcc2_write_blocks_test((int32_t)ws, _t_pat, 4U, 1U);
        printf("sdtest: write adma rc=%u\n", (unsigned)rc);
        if (rc == 0) {
            rc = (uint32_t)sdcc2_read_blocks_test((int32_t)ws, _t_out, 4U, 1U);
            printf("sdtest: write readback adma rc=%u cmp=%d\n", (unsigned)rc,
                    rc == 0 ? memcmp(_t_pat, _t_out, 4U * 512U) : -2);
        }
        printf("sdtest: restore start\n");
        rc = (uint32_t)sdcc2_write_blocks((int32_t)ws, _t_orig, 4U);
        printf("sdtest: restore rc=%u\n", (unsigned)rc);
    }
    printf("sdtest: ==== full test done ====\n");
}

int main(int argc, char** argv) {

    uint64_t sd = SDCC_WIN_VA + SDCC2_OFF;
    sys_info_t sysinfo;
    q8b_platform_data_t* pd;

    setbuf(stdout, NULL);
    if (argc > 1 && argv[1][0] == 't') {
        printf("sdtest: inherit mode (no re-probe)\n");
        if (bsp_sd_init() != 0) {
            printf("sdtest: bsp_sd_init failed\n");
            return 1;
        }
        printf("sdtest: capacity=%u\n",
                (unsigned)sdcc2_capacity_sectors());
        full_test();
        return 0;
    }
    printf("sdprobe: bsp_sd_init_physical start\n");
    if (bsp_sd_init_physical() != 0) {
        printf("sdprobe: user-space SD init failed\n");
        return 1;
    }
    printf("sdprobe: bsp_sd_init_physical ok\n");
    sys_get_sys_info(&sysinfo);
    pd = (q8b_platform_data_t*)(void*)sysinfo.platform_data;
    printf("sdprobe: kernel state=0x%08x detail=0x%08x\n",
            pd->sd.state, pd->sd.detail);
    if (pd->sd.state == 0x53440018U)
        printf("sdprobe: CMD18 ready, capacity=%u sectors (%u MiB)\n",
                pd->sd.detail, pd->sd.detail / 2048U);
    if (physical_read(1U, _blocks, 4U) == 0) {
        printf("sdprobe: userspace CMD18 LBA1 signature='%.8s'\n",
                (const char*)_blocks);
        inspect_gpt();
    } else {
        printf("sdprobe: user-space CMD18 read failed\n");
    }
    if (map_at(GCC_VA, GCC_PHY, GCC_SIZE) != 0 ||
            map_at(SDCC_WIN_VA, SDCC_WIN_PHY, SDCC_WIN_SIZE) != 0 ||
            map_at(TLMM_VA, TLMM_PHY, TLMM_SIZE) != 0 ||
            map_at(RSC_VA, RSC_PHY, RSC_SIZE) != 0) {
        printf("sdprobe: SYS_MEM_MAP failed\n");
        return 1;
    }

    dump("gcc-cmd", GCC_VA, 0x1400c);
    dump("gcc-cfg", GCC_VA, 0x14010);
    dump("gcc-m", GCC_VA, 0x14014);
    dump("gcc-n", GCC_VA, 0x14018);
    dump("gcc-d", GCC_VA, 0x1401c);
    dump("gcc-ahb", GCC_VA, 0x14008);
    dump("gcc-apps", GCC_VA, 0x14004);

    dump("tlmm-sdc2", TLMM_VA, 0x0e8000);
    dump("gpio131-ctl", TLMM_VA, GPIO131_CTL);
    dump("gpio131-io", TLMM_VA, GPIO131_IO);
    printf("sdprobe: card-detect(active-low) = %u\n",
            rd32(TLMM_VA, GPIO131_IO) & 1U);

    printf("sdprobe: host-version [0x%08llx] = 0x%04x\n",
            (unsigned long long)(SDCC_WIN_PHY + SDCC2_OFF + 0xfe),
            rd16(sd, 0xfe));
    dump("caps0", sd, 0x040);
    dump("caps1", sd, 0x044);
    dump("present", sd, 0x024);
    dump("host-ctrl", sd, 0x028);
    dump("clock", sd, 0x02c);
    dump("response", sd, 0x010);
    dump("xfer-command", sd, 0x00c);
    dump("int-status", sd, 0x030);
    dump("mci-version", sd, 0x318);
    dump("vendor-spec", sd, 0x20c);
    dump("vendor-caps0", sd, 0x21c);
    dump("pwr-status", sd, 0x240);
    dump("pwr-mask", sd, 0x244);
    dump("pwr-clear", sd, 0x248);
    dump("pwr-ctl", sd, 0x24c);

    dump("rsc-id", RSC_VA, 0x0000);
    dump("rsc-config", RSC_VA, 0x000c);
    dump("tcs-irq-en", RSC_VA, 0x0d00);
    dump("tcs-irq-st", RSC_VA, 0x0d04);
    dump("tcs0-ctrl-v2", RSC_VA, 0x0d14);
    dump("tcs0-stat-v2", RSC_VA, 0x0d18);
    dump("tcs0-en-v2", RSC_VA, 0x0d1c);
    dump("tcs0-addr-v2", RSC_VA, 0x0d34);
    dump("tcs0-ctrl-v3", RSC_VA, 0x0d24);
    dump("tcs0-stat-v3", RSC_VA, 0x0d28);
    dump("tcs0-en-v3", RSC_VA, 0x0d2c);
    dump("tcs0-addr-v3", RSC_VA, 0x0d38);
    if (argc > 1 && argv[1][0] == 'f')
        full_test();
    return 0;
}
