#ifndef BSP_SD_H
#define BSP_SD_H

/* Initialize libsd and (re)establish this process's RAM-disk mapping.
 * fork() does not inherit SYS_MEM_MAP windows, so init calls this again in
 * every forked child before read_fs(); the mapping work must therefore be
 * repeated per process rather than cached in the parent.  This port has no
 * physical-card backend: the boot RAM disk is the only block device. */
int bsp_sd_init(void);
/* Physical microSD (SDHCI) backend for a persistent rootfs. */
int bsp_sd_init_physical(void);
static inline int bsp_sd_flush(void) {
	return 0;
}

#endif