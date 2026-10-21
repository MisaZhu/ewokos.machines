/*
 * espwrite - update files on the Q8B SD card EFI System Partition (FAT32).
 *
 * The board boots BOOTAA64.EFI / KERNEL.BIN from the FAT32 ESP; the ext3
 * rootfs updater (sdupdate) cannot touch it.  This tool talks directly to
 * the user-space SDCC2 block backend at raw LBA level, locates the EFI
 * System Partition in the GPT, and uses the shared FAT32 library (the same
 * one behind usbfat32fsd) to read/write files there.
 *
 *   espwrite list
 *   espwrite get <name> <local-file>
 *   espwrite put <local-file> <name>
 *   espwrite rm <name>
 *   espwrite boot <kernel.bin> <BOOTAA64.EFI>
 *
 * Every put is read back and compared before it reports success.  The tool
 * must run to completion before the rootfs continues to use the card: the
 * SDCC2 probe re-initialises the bus, so reboot afterwards.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <bsp/bsp_sd.h>
#include <sd/sd.h>
#include <fat32/fat32fs.h>

#define GPT_SIGNATURE 0x5452415020494645ULL

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
    uint8_t  disk_guid[16];
    uint64_t entry_lba;
    uint32_t entry_count;
    uint32_t entry_size;
    uint32_t entry_crc32;
} __attribute__((packed)) gpt_header_t;

typedef struct {
    uint8_t  type_guid[16];
    uint8_t  part_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attrs;
    uint8_t  name[72];
} __attribute__((packed)) gpt_entry_t;

/* EFI System Partition: C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
static const uint8_t _esp_type[16] = {
    0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
    0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b
};

static uint32_t _esp_start = 0;
static uint32_t _esp_sectors = 0;

static int raw_read(int32_t sector, void* buf, uint32_t count) {
    return sd_read_sectors(sector, buf, count) ==
            (int32_t)(count * SECTOR_SIZE) ? 0 : -1;
}

static int raw_write(int32_t sector, const void* buf, uint32_t count) {
    return sd_write_sectors(sector, buf, count) ==
            (int32_t)(count * SECTOR_SIZE) ? 0 : -1;
}

static int fat_read_sector(int32_t sector, void* buf) {
    if (sector < 0)
        return -1;
    return raw_read((int32_t)_esp_start + sector, buf, 1);
}

static int fat_read_sectors(int32_t sector, void* buf, uint32_t count) {
    if (sector < 0)
        return -1;
    return raw_read((int32_t)_esp_start + sector, buf, count);
}

static int fat_write_sector(int32_t sector, const void* buf) {
    if (sector < 0)
        return -1;
    return raw_write((int32_t)_esp_start + sector, buf, 1);
}

static int find_esp(void) {
    uint8_t* sector = (uint8_t*)malloc(SECTOR_SIZE);
    gpt_header_t* hdr;
    uint8_t* entries = NULL;
    uint32_t table_bytes;
    int found = -1;

    if (sector == NULL)
        return -1;
    if (raw_read(1, sector, 1) != 0) {
        printf("espwrite: GPT header read failed\n");
        free(sector);
        return -1;
    }
    hdr = (gpt_header_t*)sector;
    if (hdr->signature != GPT_SIGNATURE || hdr->entry_size < sizeof(gpt_entry_t) ||
            hdr->entry_count == 0 || hdr->entry_lba > 0x7fffffffULL) {
        printf("espwrite: no valid GPT header\n");
        free(sector);
        return -1;
    }
    table_bytes = hdr->entry_count * hdr->entry_size;
    if (table_bytes > 512U * 1024U) {
        printf("espwrite: GPT table too large\n");
        free(sector);
        return -1;
    }
    entries = (uint8_t*)malloc(table_bytes);
    if (entries == NULL) {
        free(sector);
        return -1;
    }
    if (raw_read((int32_t)hdr->entry_lba, entries,
                (table_bytes + SECTOR_SIZE - 1) / SECTOR_SIZE) != 0) {
        printf("espwrite: GPT table read failed\n");
        free(entries);
        free(sector);
        return -1;
    }

    for (uint32_t i = 0; i < hdr->entry_count; i++) {
        gpt_entry_t* e = (gpt_entry_t*)(entries + i * hdr->entry_size);
        if (memcmp(e->type_guid, _esp_type, sizeof(_esp_type)) == 0) {
            _esp_start = (uint32_t)e->first_lba;
            _esp_sectors = (uint32_t)(e->last_lba - e->first_lba + 1);
            printf("espwrite: ESP partition %u lba=%u..%llu (%u MiB)\n",
                    i + 1, _esp_start,
                    (unsigned long long)e->last_lba, _esp_sectors / 2048U);
            found = 0;
            break;
        }
    }
    if (found != 0)
        printf("espwrite: EFI System Partition not found\n");
    free(entries);
    free(sector);
    return found;
}

static int fat_open(fat32_t* fat) {
    if (fat32_init_ex(fat, fat_read_sector, fat_read_sectors,
                fat_write_sector) != 0) {
        printf("espwrite: FAT32 init failed\n");
        return -1;
    }
    return 0;
}

/* Split "EFI/BOOT/X" into its parent directory path and leaf name. */
static int split_path(const char* path, char* dir, char* leaf) {
    const char* slash = strrchr(path, '/');
    if (slash == NULL) {
        dir[0] = 0;
        strncpy(leaf, path, FAT32_NAME_MAX - 1);
        leaf[FAT32_NAME_MAX - 1] = 0;
        return 0;
    }
    size_t n = (size_t)(slash - path);
    if (n >= 200)
        return -1;
    memcpy(dir, path, n);
    dir[n] = 0;
    if (dir[0] == 0)
        strcpy(dir, "/");
    strncpy(leaf, slash + 1, FAT32_NAME_MAX - 1);
    leaf[FAT32_NAME_MAX - 1] = 0;
    return leaf[0] ? 0 : -1;
}

static int parent_dir(fat32_t* fat, const char* dir, fat32_node_t* out) {
    if (dir == NULL || dir[0] == 0 || strcmp(dir, "/") == 0) {
        fat32_root_node(fat, out);
        return 0;
    }
    if (fat32_node_by_fname(fat, dir, out) != 0 ||
            (out->attr & FAT32_ATTR_DIRECTORY) == 0) {
        printf("espwrite: directory not found: %s\n", dir);
        return -1;
    }
    return 0;
}

static char* read_local(const char* path, size_t* out_size) {
    struct stat st;
    int fd;
    char* p;
    size_t done = 0;

    if (stat(path, &st) != 0 || st.st_size <= 0) {
        printf("espwrite: cannot stat %s\n", path);
        return NULL;
    }
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("espwrite: cannot open %s\n", path);
        return NULL;
    }
    p = (char*)malloc((size_t)st.st_size);
    if (p == NULL) {
        close(fd);
        return NULL;
    }
    while (done < (size_t)st.st_size) {
        ssize_t n = read(fd, p + done, (size_t)st.st_size - done);
        if (n <= 0) {
            free(p);
            close(fd);
            return NULL;
        }
        done += (size_t)n;
    }
    close(fd);
    *out_size = done;
    return p;
}

static int write_file(const char* path, const char* data, size_t size) {
    fat32_t fat;
    fat32_node_t parent, node;
    char dir[256], leaf[FAT32_NAME_MAX];
    int created = 0;
    int ret = -1;

    if (fat_open(&fat) != 0)
        return -1;
    if (split_path(path, dir, leaf) != 0 ||
            parent_dir(&fat, dir, &parent) != 0)
        goto out;

    if (fat32_node_by_fname(&fat, path, &node) == 0) {
        if (node.attr & FAT32_ATTR_DIRECTORY) {
            printf("espwrite: %s is a directory\n", path);
            goto out;
        }
        if (fat32_truncate(&fat, &node) != 0) {
            printf("espwrite: truncate %s failed\n", path);
            goto out;
        }
    } else {
        if (fat32_create_file(&fat, &parent, leaf, &node) != 0) {
            printf("espwrite: create %s failed\n", path);
            goto out;
        }
        created = 1;
    }

    if (fat32_write(&fat, &node, data, (int32_t)size, 0) != (int32_t)size) {
        printf("espwrite: write %s failed\n", path);
        goto out;
    }
    node.size = (uint32_t)size;
    if (fat32_update_node(&fat, &node) != 0) {
        printf("espwrite: metadata update %s failed\n", path);
        goto out;
    }
    if (fat32_flush(&fat) != 0) {
        printf("espwrite: flush failed\n");
        goto out;
    }
    printf("espwrite: wrote %s (%u bytes%s)\n", path, (unsigned)size,
            created ? ", new" : "");
    ret = 0;

out:
    fat32_quit(&fat);
    return ret;
}

/* Fresh mount + full byte compare. */
static int verify_file(const char* path, const char* data, size_t size) {
    fat32_t fat;
    fat32_node_t node;
    char* buf;
    int ret = -1;

    if (fat_open(&fat) != 0)
        return -1;
    if (fat32_node_by_fname(&fat, path, &node) != 0) {
        printf("espwrite: verify: %s missing\n", path);
        goto out;
    }
    if (node.size != (uint32_t)size) {
        printf("espwrite: verify: %s size %u != %u\n", path, node.size,
                (unsigned)size);
        goto out;
    }
    buf = (char*)malloc(size ? size : 1);
    if (buf == NULL)
        goto out;
    if (fat32_read(&fat, &node, buf, (int32_t)size, 0) != (int32_t)size) {
        printf("espwrite: verify: read %s failed\n", path);
        free(buf);
        goto out;
    }
    if (memcmp(buf, data, size) != 0) {
        printf("espwrite: verify: %s content mismatch\n", path);
        free(buf);
        goto out;
    }
    free(buf);
    printf("espwrite: verified %s\n", path);
    ret = 0;
out:
    fat32_quit(&fat);
    return ret;
}

static int put_file(const char* local, const char* name) {
    size_t size = 0;
    char* data = read_local(local, &size);
    int ret;

    if (data == NULL)
        return -1;
    printf("espwrite: %s -> %s (%u bytes)\n", local, name, (unsigned)size);
    ret = write_file(name, data, size);
    if (ret == 0)
        ret = verify_file(name, data, size);
    free(data);
    return ret;
}

static int get_file(const char* name, const char* local) {
    fat32_t fat;
    fat32_node_t node;
    char* buf;
    int fd;
    int ret = -1;

    if (fat_open(&fat) != 0)
        return -1;
    if (fat32_node_by_fname(&fat, name, &node) != 0 ||
            (node.attr & FAT32_ATTR_DIRECTORY)) {
        printf("espwrite: %s not found\n", name);
        fat32_quit(&fat);
        return -1;
    }
    buf = (char*)malloc(node.size ? node.size : 1);
    if (buf == NULL) {
        fat32_quit(&fat);
        return -1;
    }
    if (fat32_read(&fat, &node, buf, (int32_t)node.size, 0) !=
            (int32_t)node.size) {
        printf("espwrite: read %s failed\n", name);
        free(buf);
        fat32_quit(&fat);
        return -1;
    }
    fd = open(local, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        size_t done = 0;
        while (done < node.size) {
            ssize_t n = write(fd, buf + done, node.size - done);
            if (n <= 0)
                break;
            done += (size_t)n;
        }
        close(fd);
        if (done == node.size) {
            printf("espwrite: extracted %s (%u bytes) -> %s\n", name,
                    node.size, local);
            ret = 0;
        }
    }
    free(buf);
    fat32_quit(&fat);
    return ret;
}

static void list_dir(fat32_t* fat, fat32_node_t* dir, const char* prefix) {
    fat32_dir_t it;
    fat32_node_t kid;
    if (fat32_diropen(fat, dir, &it) != 0)
        return;
    while (fat32_dirnext(fat, &it, &kid) == 1) {
        printf("espwrite:   %s%s%s%u\n", prefix, kid.name,
                (kid.attr & FAT32_ATTR_DIRECTORY) ? "/ " : " ",
                (kid.attr & FAT32_ATTR_DIRECTORY) ? 0 : kid.size);
        if ((kid.attr & FAT32_ATTR_DIRECTORY) && strcmp(kid.name, ".") != 0 &&
                strcmp(kid.name, "..") != 0) {
            char sub[256];
            snprintf(sub, sizeof(sub), "%s%s/", prefix, kid.name);
            list_dir(fat, &kid, sub);
        }
    }
    fat32_dirclose(&it);
}

static int rm_file(const char* name) {
    fat32_t fat;
    int ret;

    if (fat_open(&fat) != 0)
        return -1;
    ret = fat32_unlink(&fat, name) == 0 ? 0 : -1;
    if (ret == 0)
        ret = fat32_flush(&fat) == 0 ? 0 : -1;
    if (ret != 0)
        printf("espwrite: unlink %s failed\n", name);
    else
        printf("espwrite: removed %s\n", name);
    fat32_quit(&fat);
    return ret;
}

int main(int argc, char** argv) {
    fat32_t fat;
    int ret = 1;

    setbuf(stdout, NULL);
    if (argc < 2) {
        printf("usage: espwrite list|get|put|rm|boot ...\n");
        return 1;
    }
    if (bsp_sd_init_physical() != 0) {
        printf("espwrite: user-space SD init failed\n");
        return 1;
    }
    if (find_esp() != 0) {
        sd_quit();
        return 1;
    }

    if (strcmp(argv[1], "list") == 0) {
        if (fat_open(&fat) != 0) {
            sd_quit();
            return 1;
        }
        printf("espwrite: root listing\n");
        fat32_node_t root;
        fat32_root_node(&fat, &root);
        list_dir(&fat, &root, "");
        fat32_quit(&fat);
        ret = 0;
    } else if (strcmp(argv[1], "get") == 0 && argc >= 4) {
        ret = get_file(argv[2], argv[3]);
    } else if (strcmp(argv[1], "put") == 0 && argc >= 4) {
        ret = put_file(argv[2], argv[3]);
    } else if (strcmp(argv[1], "rm") == 0 && argc >= 3) {
        ret = rm_file(argv[2]);
    } else if (strcmp(argv[1], "boot") == 0 && argc >= 4) {
        ret = put_file(argv[2], "KERNEL.BIN");
        if (ret == 0)
            ret = put_file(argv[3], "EFI/BOOT/BOOTAA64.EFI");
    } else {
        printf("usage: espwrite list|get|put|rm|boot ...\n");
    }
    sd_quit();
    return ret ? 1 : 0;
}
