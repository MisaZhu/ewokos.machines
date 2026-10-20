#ifndef BSP_SD_H
#define BSP_SD_H

/* Initialize libsd and (re)establish this process's SD/RAM-disk mappings.
 * fork() does not inherit SYS_MEM_MAP windows, so init calls this again in
 * every forked child before read_fs(); the mapping work must therefore be
 * repeated per process rather than cached in the parent. */
int bsp_sd_init(void);
/* Force the physical card even when a firmware RAM disk is present; used by
 * the offline rootfs updaters (sdinstall/sdupdate/sdprobe). */
int bsp_sd_init_physical(void);
static inline int bsp_sd_flush(void) {
	return 0;
}

#endif
