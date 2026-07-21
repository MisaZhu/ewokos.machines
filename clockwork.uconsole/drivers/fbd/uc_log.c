#include "uc_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>

#include <ewoksys/klog.h>

/*
 * 24KB is enough for the whole bring-up transcript including full
 * register dumps at every stage; overflow just drops the tail (the
 * kout() copy of every line is unconditional).
 */
#define UC_LOG_CAP 24576U

static char _log_buf[UC_LOG_CAP];
static uint32_t _log_len = 0;

void uc_log(const char* fmt, ...) {
	char line[256];
	int n;
	va_list ap;

	va_start(ap, fmt);
	n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (n <= 0) {
		return;
	}
	if ((uint32_t)n >= sizeof(line)) {
		n = (int)sizeof(line) - 1;
	}

	kout(line, (uint32_t)n);

	if (_log_len + (uint32_t)n < UC_LOG_CAP) {
		memcpy(_log_buf + _log_len, line, (uint32_t)n);
		_log_len += (uint32_t)n;
	}
}

int uc_log_save(void) {
	int fd;
	uint32_t off = 0;

	fd = open("/fbd.log", O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (fd < 0) {
		return -1;
	}
	while (off < _log_len) {
		int w = write(fd, _log_buf + off, _log_len - off);
		if (w <= 0) {
			break;
		}
		off += (uint32_t)w;
	}
	close(fd);
	return (off == _log_len) ? 0 : -1;
}
