/*
 * adreno_flag.c - runtime bring-up switches without a POSIX shell.
 *
 * The Q8B sshd executes commands directly: there is no /bin/sh, so a test
 * harness cannot do `export ADRENO_SKIP_PDC=1` over ssh.  Flags therefore
 * come from either the process environment (init.rd's @export, the normal
 * production path) or a small file written by the harness:
 *
 *     /tmp/adreno.cfg      NAME=1
 *                          ADRENO_SKIP_PDC=1
 *
 * A flag is on when getenv(NAME) is exactly "1" or the cfg file contains a
 * line "NAME=1".  /tmp is the ramfs, so it is writable over scp and vanishes
 * on reboot (which is what we want: every boot starts from a known state).
 */
#include <adreno/adreno_drv.h>
#include <ewoksys/vfs.h>
#include <stdlib.h>
#include <string.h>

#define ADRENO_CFG_PATH "/tmp/adreno.cfg"

int adreno_flag(const char *name)
{
	const char *v;
	int size = 0;
	char *buf;
	char *p, *end;
	size_t n;

	if (name == NULL || name[0] == '\0')
		return 0;

	v = getenv(name);
	if (v != NULL && v[0] == '1' && v[1] == '\0')
		return 1;

	buf = vfs_readfile(ADRENO_CFG_PATH, &size);
	if (buf == NULL || size <= 0) {
		if (buf)
			free(buf);
		return 0;
	}

	n = strlen(name);
	p = buf;
	end = buf + size;
	while (p < end) {
		char *e = p;
		while (e < end && *e != '\n' && *e != '\r')
			e++;
		if ((size_t)(e - p) >= n + 2 &&
		    strncmp(p, name, n) == 0 &&
		    p[n] == '=' && p[n + 1] == '1') {
			free(buf);
			return 1;
		}
		p = (e < end) ? e + 1 : end;
	}

	free(buf);
	return 0;
}

/* Numeric switch: getenv(name) or a "name=<number>" line in /tmp/adreno.cfg.
   Used where the value is not a boolean (e.g. ADRENO_GFX_CORNER=4). */
int adreno_cfg_int(const char *name, int def)
{
	const char *v;
	int size = 0;
	char *buf;
	char *p, *end;
	size_t n;
	int out = def;

	if (name == NULL || name[0] == '\0')
		return def;

	v = getenv(name);
	if (v != NULL && v[0] != '\0')
		return atoi(v);

	buf = vfs_readfile(ADRENO_CFG_PATH, &size);
	if (buf == NULL || size <= 0) {
		if (buf)
			free(buf);
		return def;
	}
	n = strlen(name);
	p = buf;
	end = buf + size;
	while (p < end) {
		char *e = p;
		while (e < end && *e != '\n' && *e != '\r')
			e++;
		if ((size_t)(e - p) > n + 1 && strncmp(p, name, n) == 0 &&
		    p[n] == '=') {
			out = atoi(p + n + 1);
			break;
		}
		p = (e < end) ? e + 1 : end;
	}
	free(buf);
	return out;
}
