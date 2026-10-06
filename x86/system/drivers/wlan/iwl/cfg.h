/*
 * Device configuration table: PCI IDs, firmware file prefixes and
 * transport parameters for the supported NIC families.
 *
 * SO (AX210 MAC family, CNVio2): AX211, AX411 (+ AX201/AX210 ids kept for
 * completeness of the SO table). Firmware API 67..77 covers all firmware
 * still distributed for these; we require >= 71 for the gen3 boot flow
 * and wide-command-header everywhere.
 *
 * BZ (BE200 MAC family): BE200, BE202 and their Killer variants.
 * Firmware API 90..92.
 */
#ifndef __IWLM_CFG_H__
#define __IWLM_CFG_H__

#include <linux/types.h>

#define IWL_PCI_VENDOR 0x8086

enum iwlm_family {
    IWLM_FAM_SO = 0,   /* AX210 MAC family: gen3, fw api 67..77 */
    IWLM_FAM_BZ = 1,   /* BE200 MAC family: gen3, fw api 90..92 */
};

struct iwlm_dev_cfg {
    u16 device;          /* PCI device id, 0 = match any in family range */
    u8  family;          /* enum iwlm_family */
    u8  integrated;      /* CNVio2 (RF on package): 64B RX chunk */
    u8  api_min;
    u8  api_max;
    const char *fw_pre;  /* firmware file name prefix (without -NN.ucode) */
    const char *name;
};

/*
 * AX211/AX411 firmware: iwlwifi-so-a0-gf-a0 / -gf4 (GF4 is the CDB variant
 * used by AX411; we pick gf-a0 first and fall back to gf4-a0 if absent,
 * matching what linux-firmware ships for SO CRFs).
 */
static const struct iwlm_dev_cfg iwlm_devs[] = {
    /* SO MAC family (AX210 gen): device ids from Linux pcie/drv.c, api 77-89 */
    { 0x2725, IWLM_FAM_SO, 0, 77, 89, "iwlwifi-ty-a0-gf-a0", "Intel AX210" },
    { 0x2726, IWLM_FAM_SO, 1, 77, 89, "iwlwifi-so-a0-gf-a0", "Intel AX211/AX411 (SO)" },
    { 0x51F0, IWLM_FAM_SO, 1, 77, 89, "iwlwifi-so-a0-gf-a0", "Intel AX211" },
    { 0x51F1, IWLM_FAM_SO, 1, 77, 89, "iwlwifi-so-a0-gf4-a0", "Intel AX411" },
    { 0x54F0, IWLM_FAM_SO, 1, 77, 89, "iwlwifi-so-a0-gf-a0", "Intel AX211" },
    { 0x7A70, IWLM_FAM_SO, 1, 77, 89, "iwlwifi-so-a0-gf-a0", "Intel AX211 (long latency)" },
    { 0x7AF0, IWLM_FAM_SO, 1, 77, 89, "iwlwifi-so-a0-gf-a0", "Intel AX211" },
    { 0x7E40, IWLM_FAM_SO, 1, 77, 89, "iwlwifi-so-a0-gf-a0", "Intel AX211/AX411 (MA)" },
    /* BZ MAC family (BE200 gen): BE200/BE202 */
    { 0x272B, IWLM_FAM_BZ, 0, 90, 92, "iwlwifi-gl-c0-fm-c0", "Intel BE200" },
    { 0xA840, IWLM_FAM_BZ, 1, 90, 92, "iwlwifi-gl-c0-fm-c0", "Intel BE202/BE200 (CNVio)" },
    { 0x7740, IWLM_FAM_BZ, 1, 90, 92, "iwlwifi-gl-c0-fm-c0", "Intel BE202" },
    { 0x4D40, IWLM_FAM_BZ, 1, 90, 92, "iwlwifi-gl-c0-fm-c0", "Intel BE202" },
};

#define IWLM_NUM_DEVS (sizeof(iwlm_devs) / sizeof(iwlm_devs[0]))

/* transport constants */
#define IWLM_NUM_RBDS        512          /* MQ rx queue entries (hw: 512) */
#define IWLM_RX_BUF_SIZE     4096         /* 4K RBs (A-MSDU friendly) */
#define IWLM_CMD_QUEUE       0            /* host cmd queue id (gen3: mtr) */
#define IWLM_DATA_QUEUE      1            /* our single data tx queue */
#define IWLM_DATA_QUEUE_SIZE 256
#define IWLM_NUM_STA         1            /* just the AP */

/* PCI config space offsets we use */
#define PCI_CFG_CMD          0x04
#define PCI_CFG_BAR0         0x10
#define PCI_CFG_REVISION     0x08
#define PCI_CMD_MEM_ENABLE   0x0002
#define PCI_CMD_BUS_MASTER   0x0004
#define PCI_CMD_INTX_DISABLE 0x0400

#endif
