/* Restricted Q8B battery-manager (ADSP PMIC GLINK) platform-control ABI.
 *
 * The TW220's real battery data (SOC, charge state, health, energy) is
 * owned by the ADSP firmware battery manager behind the PMIC_RTR_ADSP_APPS
 * GLINK channel ("msm/adsp/charger_pd").  The kernel owns that channel
 * (ADSP PAS boot + GLINK FIFO + PDR tracking); user space only exchanges
 * already-encoded battmgr wire messages (see powerd/battmgr_wire.h) through
 * this narrow door.  As with q8b_npu.h, no caller-selected destination,
 * address or SMC function is ever accepted.
 *
 * Layout keeps the NPU block (0x51385100..0x513851ff) untouched; the
 * dispatcher in bsp/gpu_pas.c routes this range to bsp/batt_glink.c.
 */
#ifndef Q8B_BATT_H
#define Q8B_BATT_H

#include <stdint.h>

#define Q8B_BATT_ABI_BASE   0x51385200u
#define Q8B_BATT_ABI        0x51385201u

/* op codes. */
#define Q8B_BATT_QUERY  (Q8B_BATT_ABI_BASE + 0x00u) /* 0, 0 -> capability bits */
#define Q8B_BATT_SEND   (Q8B_BATT_ABI_BASE + 0x01u) /* owned DMA PA, size */
#define Q8B_BATT_RECV   (Q8B_BATT_ABI_BASE + 0x02u) /* owned DMA PA, max size
                                                     * -> wire length or 0 */
#define Q8B_BATT_STATUS (Q8B_BATT_ABI_BASE + 0x03u) /* 0, 0 -> state bits */
#define Q8B_BATT_BOOT   (Q8B_BATT_ABI_BASE + 0x04u) /* owned DMA PA holding the
                                                     * ADSP image (qcadsp8280.mbn),
                                                     * size -> 0 or Q8B_BATT_*.
                                                     * The buffer must also cover
                                                     * 16 KB of PAS metadata after
                                                     * the image. Aligned to 16. */

/* QUERY/STATUS capability bits. */
#define Q8B_BATT_CAP_ADSP_BOOTED   (1u << 0) /* ADSP firmware is running */
#define Q8B_BATT_CAP_CHANNEL_OPEN  (1u << 1) /* PMIC_RTR_ADSP_APPS opened */
#define Q8B_BATT_CAP_PDR_READY     (1u << 2) /* charger_pd domain is ready */
#define Q8B_BATT_CAP_NOTIFY_ARMED  (1u << 3) /* notification requested */

/* Error returns (values distinct from the NPU block). */
#define Q8B_BATT_BUSY        (-1201)
#define Q8B_BATT_UNSUPPORTED (-1202) /* no ADSP channel yet: caller must fall
                                      * back to the SPMI-only data path */
#define Q8B_BATT_INVALID     (-1203)
#define Q8B_BATT_FAILED      (-1204)

#endif
