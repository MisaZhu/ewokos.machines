#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <ewoksys/vfs.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/klog.h>
#include <sd/sd.h>
#include <ext2/ext2fs.h>
#include <bsp/bsp_nvme.h>

#define NVME_FS_BUFFER_SIZE (64 * 1024 * 1024)

static int32_t nvme_read_blocks(int32_t block, void *buf, uint32_t count) {
    return sd_read_blocks(block, buf, count);
}

static uint32_t dir_block_count(ext2_t *ext2, const INODE *inode) {
    uint32_t block_size = ext2_block_size(ext2);
    if(inode->i_size == 0)
        return 0;
    return (inode->i_size + block_size - 1) / block_size;
}

static int dirent_name_equals(const DIR_T *entry, const char *name) {
    size_t len = strlen(name);
    return entry->name_len == len && memcmp(entry->name, name, len) == 0;
}

static int dirent_type(const DIR_T *entry, const INODE *inode) {
    if(entry->file_type == EXT2_FT_DIR ||
            (inode->i_mode & 0xf000) == EXT2_S_IFDIR)
        return FS_TYPE_DIR;
    if(entry->file_type == EXT2_FT_FILE ||
            (inode->i_mode & 0xf000) == EXT2_S_IFREG)
        return FS_TYPE_FILE;
    return FS_TYPE_UNKNOWN;
}

static void inode_to_stat(node_stat_t *stat, const INODE *inode) {
    stat->atime = inode->i_atime;
    stat->ctime = inode->i_ctime;
    stat->mtime = inode->i_mtime;
    stat->gid = inode->i_gid;
    stat->uid = inode->i_uid;
    stat->links_count = inode->i_links_count;
    stat->mode = inode->i_mode;
    stat->size = inode->i_size;
}

static void stat_to_inode(const node_stat_t *stat, INODE *inode) {
    inode->i_atime = stat->atime;
    inode->i_ctime = stat->ctime;
    inode->i_mtime = stat->mtime;
    inode->i_gid = stat->gid;
    inode->i_uid = stat->uid;
    inode->i_links_count = stat->links_count;
    /* chmod must preserve the on-disk directory/regular-file type bits. */
    inode->i_mode = (inode->i_mode & 0xf000) | (stat->mode & 0x0fff);
    inode->i_size = stat->size;
}

static int append_dirent(ext2_t *ext2, const DIR_T *entry,
        fsinfo_t **infos, uint32_t *count, uint32_t *capacity) {
    INODE inode;
    int type;
    uint32_t name_len;

    if(entry->inode == 0 || dirent_name_equals(entry, ".") ||
            dirent_name_equals(entry, ".."))
        return 0;
    if(ext2_node_by_ino(ext2, entry->inode, &inode) != 0)
        return 0;
    type = dirent_type(entry, &inode);
    if(type != FS_TYPE_DIR && type != FS_TYPE_FILE)
        return 0;

    if(*count == *capacity) {
        uint32_t new_capacity = *capacity == 0 ? 16 : *capacity * 2;
        fsinfo_t *new_infos = realloc(*infos,
                sizeof(fsinfo_t) * new_capacity);
        if(new_infos == NULL)
            return -1;
        *infos = new_infos;
        *capacity = new_capacity;
    }

    fsinfo_t *info = &(*infos)[*count];
    memset(info, 0, sizeof(*info));
    name_len = entry->name_len;
    if(name_len >= FS_NODE_NAME_MAX)
        name_len = FS_NODE_NAME_MAX - 1;
    memcpy(info->name, entry->name, name_len);
    info->name[name_len] = 0;
    info->type = type;
    info->data = entry->inode;
    inode_to_stat(&info->stat, &inode);
    (*count)++;
    return 0;
}

static fsinfo_t *list_directory(ext2_t *ext2, const INODE *dir,
        uint32_t *count) {
    uint32_t block_size = ext2_block_size(ext2);
    uint32_t blocks = dir_block_count(ext2, dir);
    uint32_t capacity = 0;
    fsinfo_t *infos = NULL;
    char buf[EXT2_MAX_BLOCK_SIZE];

    *count = 0;
    for(uint32_t lbk = 0; lbk < blocks; lbk++) {
        int32_t read_size;
        uint32_t offset = 0;

        memset(buf, 0, sizeof(buf));
        read_size = ext2_read_block(ext2, (INODE *)dir, buf,
                (int32_t)block_size, (int32_t)(lbk * block_size));
        if(read_size <= 0)
            continue;

        while(offset + 8 <= block_size) {
            DIR_T *entry = (DIR_T *)(buf + offset);
            uint32_t min_rec_len;

            if(entry->name_len == 0 || entry->rec_len < 12 ||
                    offset + entry->rec_len > block_size)
                break;
            min_rec_len = 4 * ((8 + entry->name_len + 3) / 4);
            if(entry->rec_len < min_rec_len)
                break;
            if(append_dirent(ext2, entry, &infos, count, &capacity) != 0) {
                free(infos);
                *count = 0;
                return NULL;
            }
            offset += entry->rec_len;
        }
    }
    return infos;
}

static int nvmeext2_mount(vdevice_t *dev, fsinfo_t *info, void *p) {
    (void)dev;
    ext2_t *ext2 = (ext2_t *)p;
    INODE root;
    uint32_t count;
    fsinfo_t *kids;

    if(ext2_node_by_fname(ext2, "/", &root) != 0)
        return -1;
    info->data = 2;
    inode_to_stat(&info->stat, &root);
    kids = list_directory(ext2, &root, &count);
    if(kids == NULL && count != 0)
        return -1;

    for(uint32_t offset = 0; offset < count; offset += 64) {
        uint32_t n = count - offset;
        if(n > 64)
            n = 64;
        if(vfs_new_nodes(&kids[offset], n, info->node) != 0) {
            for(uint32_t i = 0; i < n; i++)
                vfs_new_node(&kids[offset + i], info->node, false, false);
        }
    }
    free(kids);
    info->state |= FS_STATE_KIDS_LOADED;
    return 0;
}

static int nvmeext2_create(vdevice_t *dev, int pid, fsinfo_t *parent,
        fsinfo_t *info, void *p) {
    (void)dev;
    (void)pid;
    ext2_t *ext2 = (ext2_t *)p;
    int32_t parent_ino = parent->data == 0 ? 2 : (int32_t)parent->data;
    INODE parent_inode;
    int ino;

    if(ext2_node_by_ino(ext2, parent_ino, &parent_inode) != 0)
        return -1;
    if(FS_IS_TYPE(info->type, FS_TYPE_DIR)) {
        info->stat.size = ext2_block_size(ext2);
        ino = ext2_create_dir(ext2, parent_ino, &parent_inode, info->name,
                info->stat.uid, info->stat.gid, info->stat.mode);
    }
    else {
        info->stat.size = 0;
        ino = ext2_create_file(ext2, parent_ino, &parent_inode, info->name,
                info->stat.uid, info->stat.gid, info->stat.mode);
    }
    if(ino < 0)
        return -1;
    info->data = (uint32_t)ino;
    if(FS_IS_TYPE(info->type, FS_TYPE_DIR))
        info->state |= FS_STATE_KIDS_LOADED;
    return 0;
}

static int nvmeext2_open(vdevice_t *dev, int fd, int from_pid,
        fsinfo_t *info, int oflag, void *p) {
    (void)dev;
    (void)fd;
    (void)from_pid;
    if((oflag & O_TRUNC) == 0)
        return 0;

    ext2_t *ext2 = (ext2_t *)p;
    INODE inode;
    if(info->data == 0 || ext2_node_by_ino(ext2, info->data, &inode) != 0)
        return -1;
    if(ext2_truncate(ext2, info->data, &inode) != 0)
        return -1;
    inode_to_stat(&info->stat, &inode);
    return 0;
}

static int nvmeext2_stat(vdevice_t *dev, int from_pid, fsinfo_t *info,
        node_stat_t *stat, void *p) {
    (void)dev;
    (void)from_pid;
    ext2_t *ext2 = (ext2_t *)p;
    uint32_t ino = info->data == 0 ? 2 : info->data;
    INODE inode;

    if(ext2_node_by_ino(ext2, ino, &inode) != 0)
        return -1;
    inode_to_stat(stat, &inode);
    return 0;
}

static int nvmeext2_set(vdevice_t *dev, int from_pid, fsinfo_t *info, void *p) {
    (void)dev;
    (void)from_pid;
    ext2_t *ext2 = (ext2_t *)p;
    INODE inode;

    if(info->data == 0 || ext2_node_by_ino(ext2, info->data, &inode) != 0)
        return -1;
    stat_to_inode(&info->stat, &inode);
    return put_node(ext2, info->data, &inode);
}

static int nvmeext2_get(vdevice_t *dev, int from_pid, const char *fname,
        fsinfo_t *info, void *p) {
    (void)dev;
    (void)from_pid;
    ext2_t *ext2 = (ext2_t *)p;
    DIR_T entry;
    INODE inode;
    uint32_t ino = ext2_ino_by_fname(ext2, fname, &entry);
    uint32_t name_len;

    if(ino == 0 || ext2_node_by_ino(ext2, ino, &inode) != 0)
        return -1;
    memset(info, 0, sizeof(*info));
    name_len = entry.name_len;
    if(name_len >= FS_NODE_NAME_MAX)
        name_len = FS_NODE_NAME_MAX - 1;
    memcpy(info->name, entry.name, name_len);
    info->name[name_len] = 0;
    info->type = dirent_type(&entry, &inode);
    info->data = ino;
    inode_to_stat(&info->stat, &inode);
    return 0;
}

static fsinfo_t *nvmeext2_kids(vdevice_t *dev, fsinfo_t *info,
        uint32_t *count, void *p) {
    (void)dev;
    *count = 0;
    if(!FS_IS_TYPE(info->type, FS_TYPE_DIR))
        return NULL;

    ext2_t *ext2 = (ext2_t *)p;
    uint32_t ino = info->data == 0 ? 2 : info->data;
    INODE inode;
    if(ext2_node_by_ino(ext2, ino, &inode) != 0)
        return NULL;
    return list_directory(ext2, &inode, count);
}

static int nvmeext2_read(vdevice_t *dev, int fd, int from_pid,
        fsinfo_t *info, void *buf, int size, int offset, void *p) {
    (void)dev;
    (void)fd;
    (void)from_pid;
    ext2_t *ext2 = (ext2_t *)p;
    uint32_t ino = info->data == 0 ? 2 : info->data;
    INODE inode;

    if(size < 0 || offset < 0 || ext2_node_by_ino(ext2, ino, &inode) != 0)
        return -1;
    if((uint32_t)offset >= inode.i_size)
        return 0;
    if((uint32_t)size > inode.i_size - (uint32_t)offset)
        size = (int)(inode.i_size - (uint32_t)offset);
    return size == 0 ? 0 : ext2_read(ext2, &inode, buf, size, offset);
}

static int nvmeext2_write(vdevice_t *dev, int fd, int from_pid,
        fsinfo_t *info, const void *buf, int size, int offset, void *p) {
    (void)dev;
    (void)fd;
    (void)from_pid;
    ext2_t *ext2 = (ext2_t *)p;
    INODE inode;
    int written;

    if(info->data == 0 || size < 0 || offset < 0 ||
            ext2_node_by_ino(ext2, info->data, &inode) != 0)
        return -1;
    written = ext2_write(ext2, &inode, buf, size, offset);
    if(written >= 0) {
        inode_to_stat(&info->stat, &inode);
        if(put_node(ext2, info->data, &inode) != 0)
            return -1;
    }
    return written;
}

static int nvmeext2_unlink(vdevice_t *dev, fsinfo_t *info,
        const char *fname, void *p) {
    (void)dev;
    ext2_t *ext2 = (ext2_t *)p;
    int ret = FS_IS_TYPE(info->type, FS_TYPE_DIR) ?
        ext2_rmdir(ext2, fname) : ext2_unlink(ext2, fname);
    if(ret != 0)
        return -1;
    return vfs_del_node(info->node);
}

static void nvmefsd_log_fs_info(const ext2_t *ext2) {
    char volume[sizeof(ext2->super.s_volume_name) + 1];
    uint32_t block_size = ext2_block_size(ext2);
    uint64_t nvme_blocks = bsp_nvme_get_block_count();
    uint32_t nvme_block_size = bsp_nvme_get_block_size();
    uint64_t total_mib =
        ((uint64_t)ext2->super.s_blocks_count * block_size) >> 20;
    uint64_t free_mib =
        ((uint64_t)ext2->super.s_free_blocks_count * block_size) >> 20;

    memcpy(volume, ext2->super.s_volume_name,
            sizeof(ext2->super.s_volume_name));
    volume[sizeof(ext2->super.s_volume_name)] = 0;
    for(int i = (int)sizeof(ext2->super.s_volume_name) - 1;
            i >= 0 && (volume[i] == 0 || volume[i] == ' '); i--)
        volume[i] = 0;

    klog("nvmefsd: NVMe blocks=%llu block_size=%u capacity=%llu MiB\n",
        (unsigned long long)nvme_blocks, nvme_block_size,
        (unsigned long long)((nvme_blocks * nvme_block_size) >> 20));
    klog("nvmefsd: ext2 volume='%s' block_size=%u blocks=%u free=%u "
        "capacity=%llu MiB free=%llu MiB\n",
        volume[0] != 0 ? volume : "<none>", block_size,
        ext2->super.s_blocks_count, ext2->super.s_free_blocks_count,
        (unsigned long long)total_mib, (unsigned long long)free_mib);
    klog("nvmefsd: ext2 inodes=%u free_inodes=%u groups=%d "
        "state=0x%04x revision=%u\n",
        ext2->super.s_inodes_count, ext2->super.s_free_inodes_count,
        ext2->group_num, ext2->super.s_state, ext2->super.s_rev_level);
}

int main(int argc, char **argv) {
    const char *mount_point = argc > 1 ? argv[1] : "/mnt";
    ext2_t ext2;
    vdevice_t dev;
    int ret;

    ret = bsp_nvme_init();
    if(ret != 0) {
        klog("nvmefsd: NVMe init failed (err=%d)\n", ret);
        return ret;
    }
    ret = ext2_init_ex(&ext2, sd_read, nvme_read_blocks, sd_write,
            NVME_FS_BUFFER_SIZE);
    if(ret != 0) {
        klog("nvmefsd: no supported ext2 filesystem (err=%d)\n", ret);
        sd_quit();
        return ret;
    }
    nvmefsd_log_fs_info(&ext2);

    memset(&dev, 0, sizeof(dev));
    strcpy(dev.desc, "nvmefs(ext2)");
    dev.mount = nvmeext2_mount;
    dev.read = nvmeext2_read;
    dev.write = nvmeext2_write;
    dev.create = nvmeext2_create;
    dev.open = nvmeext2_open;
    dev.stat = nvmeext2_stat;
    dev.set = nvmeext2_set;
    dev.get = nvmeext2_get;
    dev.kids = nvmeext2_kids;
    dev.unlink = nvmeext2_unlink;
    dev.extra_data = &ext2;

    ret = device_run(&dev, mount_point, FS_TYPE_DIR, 0777);
    if(ret != 0)
        klog("nvmefsd: mount at %s failed (err=%d)\n", mount_point, ret);
    ext2_quit(&ext2);
    sd_quit();
    return ret;
}
