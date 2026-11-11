#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <ext3/ext3fs.h>
#include <sd/sd.h>
#include <bsp/bsp_sd.h>
#include <ewoksys/vfs.h>

#define EXT3_IO_BUFFER (64U * 1024U)

static int read_source(const char* path, char** data, size_t* size) {
    struct stat st;
    int fd;
    char* p;
    size_t done = 0;

    if (stat(path, &st) != 0 || st.st_size < 0)
        return -1;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    p = (char*)malloc((size_t)st.st_size);
    if (p == NULL) {
        close(fd);
        return -1;
    }
    while (done < (size_t)st.st_size) {
        ssize_t n = read(fd, p + done, (size_t)st.st_size - done);
        if (n <= 0) {
            free(p);
            close(fd);
            return -1;
        }
        done += (size_t)n;
    }
    close(fd);
    *data = p;
    *size = done;
    return 0;
}

static int update_file(ext3_t* fs, const char* dst, const char* data,
        size_t size) {
    char dir[FS_FULL_NAME_MAX];
    char name[FS_FULL_NAME_MAX];
    EXT3_INODE parent;
    EXT3_INODE node;
    uint32_t parent_ino;
    uint32_t ino;
    int created = 0;

    vfs_dir_name(dst, dir, sizeof(dir));
    vfs_file_name(dst, name, sizeof(name));
    if (name[0] == 0 || dir[0] == 0)
        return -1;

    parent_ino = ext3_ino_by_fname(fs, dir, NULL);
    if (parent_ino == 0 || ext3_node_by_ino(fs, parent_ino, &parent) != 0)
        return -1;
    ino = ext3_ino_by_fname(fs, dst, NULL);
    if (ino != 0) {
        if (ext3_node_by_ino(fs, ino, &node) != 0 ||
                ext3_truncate(fs, ino, &node) != 0)
            return -1;
    } else {
        int32_t created_ino = ext3_create_file(fs, parent_ino, &parent, name,
                0, 0, 0755);
        if (created_ino <= 0)
            return -1;
        ino = (uint32_t)created_ino;
        if (ext3_node_by_ino(fs, ino, &node) != 0)
            return -1;
        created = 1;
    }

    if (size > 0 && ext3_write(fs, &node, data, (int32_t)size, 0) !=
            (int32_t)size)
        return -1;
    if (ext3_put_node(fs, ino, &node) != 0 || ext3_commit(fs) != 0)
        return -1;
    printf("sdupdate: %s %s (%u bytes)\n", created ? "created" : "updated",
            dst, (unsigned int)size);
    return 0;
}

int main(int argc, char** argv) {
    ext3_t fs;
    char* data = NULL;
    size_t size = 0;
    int ret;

    if (argc < 3) {
        printf("usage: sdupdate <source> <destination> [destination ...]\n");
        return 1;
    }
    if (bsp_sd_init_physical() != 0) {
        printf("sdupdate: SD init failed\n");
        return 1;
    }
    if (ext3_init_ex2(&fs, sd_read, sd_read_blocks, sd_write,
            sd_write_blocks, NULL, EXT3_IO_BUFFER) != 0) {
        printf("sdupdate: ext filesystem init failed\n");
        sd_quit();
        return 1;
    }
    if (read_source(argv[1], &data, &size) != 0) {
        printf("sdupdate: source read failed: %s\n", argv[1]);
        ext3_quit(&fs);
        sd_quit();
        return 1;
    }
    ret = 0;
    for (int i = 2; i < argc; i++) {
        if (update_file(&fs, argv[i], data, size) != 0) {
            printf("sdupdate: update failed: %s\n", argv[i]);
            ret = 1;
            break;
        }
    }
    free(data);
    ext3_quit(&fs);
    sd_quit();
    return ret;
}
