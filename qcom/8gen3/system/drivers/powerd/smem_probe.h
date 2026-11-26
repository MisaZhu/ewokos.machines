/* Read-only SMEM inspector used by "powerd --smem" during the ADSP
 * PMIC GLINK bring-up.  See smem_probe.c for the layouts and rationale. */
#ifndef SMEM_PROBE_H
#define SMEM_PROBE_H

/* Map the SMEM region once, print the header / TOC / partitions / GLINK
 * descriptor, then scan for GLINK and battmgr channel names.  Returns 0 on
 * success, -1 when the region cannot be mapped. */
int smem_probe_run(void);

#endif
