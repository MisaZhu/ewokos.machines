/* Read-only hardware check: compare single-sector and batched USB reads,
 * or time and hash a file through the root filesystem.
 * usb-read-bench raw /dev/usb0 <absolute-lba> <sectors>
 * usb-read-bench file <path>
 */
#include <ewoksys/vfs.h>
#include <ewoksys/usbmsc.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/md5.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void digest(const void* data, uint32_t size) {
    uint8_t md5[16];
    md5_encode(data, size, md5);
    for (unsigned i = 0; i < sizeof(md5); i++) printf("%02x", md5[i]);
}
static int raw_read(int pid, uint32_t lba, uint32_t count, uint32_t max, uint8_t* p) {
    uint64_t start = kernel_tic_ms(0);
    uint32_t calls = 0, size = count * 512u;
    while (count) {
        uint32_t n = count > max ? max : count;
        proto_t in, out;
        PF->init(&in)->addi(&in, lba)->addi(&in, n);
        PF->init(&out);
        int rc = dev_cntl_by_pid(pid, USBMSC_CMD_READ, &in, &out);
        int32_t len = 0;
        void* data = rc == 0 ? proto_read(&out, &len) : NULL;
        if (!data || len != (int32_t)(n * 512u)) rc = -1;
        else memcpy(p, data, len);
        PF->clear(&in); PF->clear(&out);
        if (rc) { printf("FAIL lba=%u count=%u\n", lba, n); return 1; }
        lba += n; count -= n; p += n * 512u; calls++;
    }
    printf("raw max=%u bytes=%u calls=%u ms=%llu\n", max, size, calls,
            (unsigned long long)(kernel_tic_ms(0) - start));
    return 0;
}
int main(int argc, char** argv) {
    setbuf(stdout, NULL);
    if (argc == 5 && !strcmp(argv[1], "raw")) {
        fsinfo_t info;
        uint32_t lba = strtoul(argv[3], NULL, 0), count = strtoul(argv[4], NULL, 0);
        if (!count || count > 8192 || (uint64_t)lba + count > 0x100000000ULL ||
                vfs_get_by_name(argv[2], &info)) return 2;
        uint32_t size = count * 512u;
        uint8_t *single = malloc(size), *batch = malloc(size);
        if (!single || !batch) return 1;
        int rc = raw_read(info.mount_pid, lba, count, 1, single) ||
                raw_read(info.mount_pid, lba, count, USBMSC_MAX_SECTORS, batch);
        if (!rc) rc = memcmp(single, batch, size) != 0;
        printf("raw compare=%s md5=", rc ? "FAIL" : "PASS");
        if (!rc) digest(batch, size);
        printf("\n"); free(single); free(batch); return rc;
    }
    if (argc == 3 && !strcmp(argv[1], "file")) {
        uint64_t start = kernel_tic_ms(0);
        struct stat st;
        int fd = open(argv[2], O_RDONLY);
        if (fd < 0 || fstat(fd, &st) || st.st_size <= 0 || st.st_size > 64*1024*1024)
            return 1;
        uint32_t size = st.st_size, done = 0;
        uint8_t* data = malloc(size);
        if (!data) return 1;
        while (done < size) {
            uint32_t n = size - done;
            if (n > 65536) n = 65536;
            int got = read(fd, data + done, n);
            if (got <= 0) break;
            done += got;
        }
        close(fd);
        uint64_t elapsed = kernel_tic_ms(0) - start;
        printf("file=%s bytes=%u/%u ms=%llu md5=", argv[2], done, size,
                (unsigned long long)elapsed);
        if (done == size) digest(data, size);
        printf("\n"); free(data); return done == size ? 0 : 1;
    }
    printf("usage: usb-read-bench raw <device> <lba> <sectors> | file <path>\n");
    return 2;
}
