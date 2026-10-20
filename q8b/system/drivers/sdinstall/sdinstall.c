/*
 * Small bootstrap updater for the physical Q8B rootfs.
 *
 * It intentionally updates only the existing /sbin/sdfsd inode.  The normal
 * sdupdate utility handles arbitrary ext3 files; this helper is kept small so
 * it can cross the board's current SSH single-transfer limit while that
 * utility is being installed.
 */
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>

#include <bsp/bsp_sd.h>
#include <sd/sd.h>

#define FS_BLOCK_SIZE 4096U
#define FS_BLOCK_SECTORS (FS_BLOCK_SIZE / SECTOR_SIZE)
#define PARTITION_LBA 1050624U
#define MAX_SOURCE_SIZE (256U * 1024U)

static uint8_t _block[FS_BLOCK_SIZE];
static uint8_t _source[MAX_SOURCE_SIZE];

static int sd_read_block(uint32_t block, void* data) {
    return sd_read_sectors((int32_t)(PARTITION_LBA +
                block * FS_BLOCK_SECTORS), data, FS_BLOCK_SECTORS) ==
            (int32_t)FS_BLOCK_SIZE ? 0 : -1;
}

static int sd_write_block(uint32_t block, const void* data) {
    return sd_write_sectors((int32_t)(PARTITION_LBA +
                block * FS_BLOCK_SECTORS), data, FS_BLOCK_SECTORS) ==
            (int32_t)FS_BLOCK_SIZE ? 0 : -1;
}

static uint16_t get16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
            ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put32(uint8_t* p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint32_t inode_block(uint32_t inode, uint32_t inodes_per_group,
        uint32_t inode_size, uint32_t inode_table, uint32_t* offset) {
    uint32_t index = (inode - 1U) % inodes_per_group;
    *offset = index * inode_size;
    return inode_table + *offset / FS_BLOCK_SIZE;
}

static int find_dir_entry(const uint8_t* dir, uint32_t size, const char* name,
        uint32_t* inode_out) {
    uint32_t off = 0;
    uint32_t want = 0;
    while (name[want] != 0)
        want++;
    while (off + 8U <= size) {
        uint32_t inode = get32(dir + off);
        uint16_t rec_len = get16(dir + off + 4U);
        uint8_t name_len = dir[off + 6U];
        if (rec_len < 8U || off + rec_len > size)
            return -1;
        if (inode != 0 && name_len == want) {
            uint32_t i;
            for (i = 0; i < want; i++)
                if (dir[off + 8U + i] != (uint8_t)name[i])
                    break;
            if (i == want) {
                *inode_out = inode;
                return 0;
            }
        }
        off += rec_len;
    }
    return -1;
}

static int read_inode(uint32_t inode, uint32_t inodes_per_group,
        uint32_t inode_size, uint32_t inode_table, uint8_t* out) {
    uint32_t off;
    uint32_t block = inode_block(inode, inodes_per_group, inode_size,
            inode_table, &off);
    if (sd_read_block(block, _block) != 0)
        return -1;
    for (uint32_t i = 0; i < inode_size; i++)
        out[i] = _block[(off % FS_BLOCK_SIZE) + i];
    return 0;
}

static int read_inode_dir(uint32_t inode, const uint8_t* node,
        uint8_t* dir, uint32_t size) {
    uint32_t blocks = (size + FS_BLOCK_SIZE - 1U) / FS_BLOCK_SIZE;
    if (blocks > 12U)
        return -1;
    for (uint32_t i = 0; i < blocks; i++) {
        uint32_t block = get32(node + 40U + i * 4U);
        if (block == 0 || sd_read_block(block, dir + i * FS_BLOCK_SIZE) != 0)
            return -1;
    }
    return 0;
}

static int read_source_file(const char* path, uint32_t* size_out) {
    int fd = open(path, O_RDONLY);
    uint32_t size = 0;
    if (fd < 0)
        return -1;
    while (size < MAX_SOURCE_SIZE) {
        int n = read(fd, _source + size, MAX_SOURCE_SIZE - size);
        if (n < 0) {
            close(fd);
            return -1;
        }
        if (n == 0)
            break;
        size += (uint32_t)n;
    }
    close(fd);
    *size_out = size;
    return size != 0 && size < MAX_SOURCE_SIZE ? 0 : -1;
}

int main(void) {
    uint8_t super[FS_BLOCK_SIZE];    uint8_t gd[FS_BLOCK_SIZE];
    uint8_t root[128];
    uint8_t sbin[128];
    uint8_t file[128];
    uint8_t dir[12U * FS_BLOCK_SIZE];
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t inode_table;
    uint32_t root_ino;
    uint32_t sbin_ino;
    uint32_t file_ino;
    uint32_t source_size;
    uint32_t old_size;
    uint32_t blocks;
    uint32_t indirect;

    if (bsp_sd_init_physical() != 0)
        return 1;
    if (read_source_file("/tmp/sdfsd.chunk.aa", &source_size) != 0)
        return 1;
    /* The second chunk is appended in RAM after the first one. */
    {
        int fd = open("/tmp/sdfsd.chunk.ab", O_RDONLY);
        if (fd < 0)
            return 1;
        while (source_size < MAX_SOURCE_SIZE) {
            int n = read(fd, _source + source_size,
                    MAX_SOURCE_SIZE - source_size);
            if (n < 0) {
                close(fd);
                return 1;
            }
            if (n == 0)
                break;
            source_size += (uint32_t)n;
        }
        close(fd);
    }

    if (sd_read_block(1U, super) != 0 || get16(super + 56U) != 0xef53U)
        return 1;
    if (get32(super + 24U) != 2U || get16(super + 88U) != 128U)
        return 1;
    inodes_per_group = get32(super + 40U);
    inode_size = get16(super + 88U);
    if (inodes_per_group == 0 || inode_size != 128U)
        return 1;
    if (sd_read_block(2U, gd) != 0)
        return 1;
    inode_table = get32(gd + 8U);
    if (read_inode(2U, inodes_per_group, inode_size, inode_table, root) != 0)
        return 1;
    root_ino = 2U;
    (void)root_ino;
    if (read_inode_dir(root_ino, root, dir, get32(root + 4U)) != 0 ||
            find_dir_entry(dir, get32(root + 4U), "sbin", &sbin_ino) != 0)
        return 1;
    if (read_inode(sbin_ino, inodes_per_group, inode_size, inode_table, sbin) != 0 ||
            read_inode_dir(sbin_ino, sbin, dir, get32(sbin + 4U)) != 0 ||
            find_dir_entry(dir, get32(sbin + 4U), "sdfsd", &file_ino) != 0)
        return 1;
    if (read_inode(file_ino, inodes_per_group, inode_size, inode_table, file) != 0)
        return 1;
    old_size = get32(file + 4U);
    blocks = (source_size + FS_BLOCK_SIZE - 1U) / FS_BLOCK_SIZE;
    if (source_size == 0 || source_size > old_size || blocks > 268U)
        return 1;

    indirect = get32(file + 40U + 12U * 4U);
    for (uint32_t i = 0; i < blocks; i++) {
        uint32_t data_block;
        uint32_t off = i * FS_BLOCK_SIZE;
        uint32_t count = source_size - off;
        if (count > FS_BLOCK_SIZE)
            count = FS_BLOCK_SIZE;
        if (i < 12U) {
            data_block = get32(file + 40U + i * 4U);
        } else {
            if (indirect == 0 || sd_read_block(indirect, _block) != 0)
                return 1;
            data_block = get32(_block + (i - 12U) * 4U);
        }
        if (data_block == 0)
            return 1;
        if (count != FS_BLOCK_SIZE) {
            if (sd_read_block(data_block, _block) != 0)
                return 1;
            for (uint32_t j = 0; j < count; j++)
                _block[j] = _source[off + j];
            for (uint32_t j = count; j < FS_BLOCK_SIZE; j++)
                _block[j] = 0;
            if (sd_write_block(data_block, _block) != 0)
                return 1;
        } else if (sd_write_block(data_block, _source + off) != 0) {
            return 1;
        }
    }
    put32(file + 4U, source_size);
    {
        uint32_t off;
        uint32_t block = inode_block(file_ino, inodes_per_group, inode_size,
                inode_table, &off);
        if (sd_read_block(block, _block) != 0)
            return 1;
        for (uint32_t i = 0; i < inode_size; i++)
            _block[(off % FS_BLOCK_SIZE) + i] = file[i];
        if (sd_write_block(block, _block) != 0)
            return 1;
    }
    return 0;
}
