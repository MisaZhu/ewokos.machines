/* Read-only Q8B regression for concurrent executable/firmware reads.
 * Usage: usb-read-check path md5 path md5 [rounds]
 * Run while WLAN is starting to exercise USB storage alongside PCIe DMA.
 * Expected hashes must come from the host build, not a possibly bad read.
 */
#include <ewoksys/md5.h>
#include <ewoksys/klog.h>
#include <pthread.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    const char *path, *expected;
    int id, rounds, failed;
} worker_t;

static void *check_reads(void *arg) {
    worker_t *w = arg;
    for (int round = 0; round < w->rounds; round++) {
        struct stat st;
        int fd = open(w->path, O_RDONLY);
        unsigned char *buf = NULL;
        size_t size = 0, done = 0;
        if (fd >= 0 && fstat(fd, &st) == 0 && st.st_size > 0) {
            size = (size_t)st.st_size;
            buf = malloc(size);
        }
        while (buf && done < size) {
            size_t n = size - done;
            if (n > 65536) n = 65536;
            int got = read(fd, buf + done, n);
            if (got <= 0) break;
            done += (size_t)got;
        }
        if (fd >= 0) close(fd);
        char hex[33] = "read-error";
        if (buf && done == size) {
            unsigned char digest[16];
            md5_encode(buf, size, digest);
            for (int i = 0; i < 16; i++)
                sprintf(hex + i * 2, "%02x", digest[i]);
        }
        free(buf);
        int bad = strcmp(hex, w->expected) != 0;
        w->failed += bad;
        klog("usb-read-check: worker=%d round=%d %s md5=%s bytes=%u/%u\n",
                w->id, round + 1, bad ? "FAIL" : "PASS", hex,
                (unsigned)done, (unsigned)size);
    }
    return NULL;
}

int main(int argc, char **argv) {
    int wait_root = argc > 1 && strcmp(argv[1], "--wait-root") == 0;
    if (wait_root) { argc--; argv++; }
    if (argc != 5 && argc != 6) {
        printf("usage: usb-read-check [--wait-root] path md5 path md5 [rounds]\n");
        return 2;
    }
    int rounds = argc == 6 ? atoi(argv[5]) : 4;
    if (rounds < 1 || rounds > 100) return 2;
    if (strlen(argv[2]) != 32 || strlen(argv[4]) != 32) return 2;
    if (wait_root) {
        /* Launch from init.ramfs.rd with '&' before switching roots. This
         * also works when an older VFS cannot expose the saved /boot mount. */
        int ready = 0;
        for (int i = 0; i < 180 && !ready; i++) {
            ready = access(argv[1], R_OK) == 0 && access(argv[3], R_OK) == 0;
            if (!ready) usleep(1000000);
        }
        if (!ready) { klog("usb-read-check: FAIL root timeout\n"); return 1; }
        usleep(35000000);
    }
    klog("usb-read-check: begin rounds=%d\n", rounds);
    setbuf(stdout, NULL);
    pthread_t threads[4];
    worker_t workers[4];
    int started = 0, failures = 0;
    for (int i = 0; i < 4; i++) {
        int pair = i & 1;
        workers[i] = (worker_t){argv[1 + pair * 2], argv[2 + pair * 2],
                i, rounds, 0};
        if (pthread_create(&threads[i], NULL, check_reads, &workers[i]) != 0) {
            failures++;
            break;
        }
        started++;
    }
    for (int i = 0; i < started; i++) {
        pthread_join(threads[i], NULL);
        failures += workers[i].failed;
    }
    klog("usb-read-check: %s workers=%d checks=%d failures=%d\n",
            failures ? "FAIL" : "PASS", started, started * rounds, failures);
    return failures ? 1 : 0;
}
