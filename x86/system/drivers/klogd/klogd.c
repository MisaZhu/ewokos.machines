#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ewoksys/syscall.h>
#include <ewoksys/klog.h>

/*
 * klogd: 把内核日志环形缓冲 (kprintf.c 的 kout ring) 排空到文件。
 *
 * 无串口真机上这是内核诊断的唯一落盘通道 —— tick 送达率对账、占核指认、
 * USB 枚举/轮询轨迹、panic dump 全部经 kout 进 ring。ring 在内核 BSS
 * (64KB), 本进程持有自己的 last_seq (单调), 落后超过一圈时内核自动跳到
 * 最新数据 (丢最旧的行)。文件超过上限时截断重开, 防止无限膨胀。
 *
 * 用法: klogd [logfile]  (默认 /var/log/kern.log, 打不开则回退 /tmp/kern.log)
 */

#define KLOG_CHUNK       4096

#include <ewoksys/kernel_tic.h>
static uint64_t now_ms(void) {
    return kernel_tic_ms(0);
}
#define KLOG_MAX_FILE    (8*1024*1024)

int main(int argc, char** argv) {
    /*
     * -f (follow): 日志流向 stdout 而不是文件 —— 在控制台前台运行时,
     * 内核日志实时滚动显示在屏幕上。无串口真机冻结排查的标准姿势:
     * 先 `klogd -f` 再复现, 冻结后屏幕上最后的日志直接指认元凶
     * (tick 送达率/占核指认/usbhostd 枚举轨迹), 不需要任何输入。
     */
    bool follow = (argc > 1 && strcmp(argv[1], "-f") == 0);
    const char* path = NULL;
    int fd = -1;
    if (follow) {
        fd = 1; /* stdout */
    }
    else {
        /* init 顺序上可能先于 ramfsd/挂载完成: 两路都打不开时重试 30s */
        path = argc > 1 ? argv[1] : "/var/log/kern.log";
        for (int i = 0; i < 60 && fd < 0; ++i) {
            fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
            if (fd < 0) {
                path = "/tmp/kern.log";
                fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
            }
            if (fd < 0) {
                usleep(500000);
            }
        }
        if (fd < 0) {
            klog("klogd: no writable log file (tried /var/log and /tmp)\n");
            return -1;
        }
    }

    static char buf[KLOG_CHUNK];
    uint64_t seq = 0;   /* 内核侧原位更新: 进参 last_seq, 出参 new_seq */
    off_t written = 0;
    uint64_t last_sync = 0;

    while (1) {
        int32_t n = (int32_t)syscall3(SYS_KLOG_READ,
                (ewokos_addr_t)buf, KLOG_CHUNK, (ewokos_addr_t)&seq);
        if (n > 0) {
            int32_t off = 0;
            while (off < n) {
                int w = write(fd, buf + off, n - off);
                if (w <= 0) {
                    if (follow)
                        break;  /* stdout 暂时不可写: 下一轮重试 */
                    break;      /* 磁盘满等: 本块丢弃, ring 里还有 */
                }
                off += w;
            }
            if (!follow) {
                written += n;
                /* 每 5s 落一次盘: 冻结排查场景下系统可能随时断电/复位,
                 * 不 sync 的话日志只活在 sdfsd 的脏页里。 */
                if (now_ms() - last_sync >= 2000u) {  /* 冻结瞬间日志尽量不丢 */
                    sync();
                    last_sync = now_ms();
                }
                if (written > KLOG_MAX_FILE) {
                    close(fd);
                    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
                    if (fd < 0) {
                        return -1;
                    }
                    written = 0;
                }
            }
        }
        usleep(200000);
    }
    return 0;
}
