#ifndef UC_LOG_H
#define UC_LOG_H

/*
 * Real diagnostics channel for the uConsole display bring-up.
 *
 * Every uc_log() line goes two ways:
 *   1. kout() -> SYS_KPRINT (kernel log; also the UART console).
 *   2. an in-process buffer that uc_log_save() flushes to /fbd.log on
 *      the SD-card rootfs (sdfsd/ext2 writes are synchronous
 *      write-through, so the file survives a power cycle).
 *
 * To read the log on a host machine, pull the SD card and extract the
 * file from the ext2 root partition, e.g.:
 *   debugfs -R "cat /fbd.log" /dev/diskNs2      (e2fsprogs)
 */
void uc_log(const char* fmt, ...);

/* Flush the buffered log to /fbd.log. Returns 0 on full write. */
int  uc_log_save(void);

#endif
