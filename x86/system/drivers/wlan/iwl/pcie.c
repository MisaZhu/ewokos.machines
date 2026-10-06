/*
 * pcie.c - PCI discovery, BAR mapping, CSR/PRPH access, reset & APM
 * for the Intel AX210/BZ family NICs (polled, userspace).
 *
 * Register sequences follow Linux iwlwifi pcie/trans-gen2.c and
 * pcie/trans.c (iwl_trans_pcie_sw_reset, prepare_card_hw, gen2_apm_init).
 */
#include "../iwlm.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ewoksys/proc.h>
#include <ewoksys/syscall.h>
#include <bsp/x86_pio.h>

#define PCI_CFG_ADDR_PORT 0xCF8
#define PCI_CFG_DATA_PORT 0xCFC

#define IWL_BAR_VA   0x50000000u   /* our virtual window for BAR0 */
#define IWL_BAR_SIZE 0x40000u      /* AX210/BZ BAR0 is 16K..256K; map 256K */

/* ---------------- pci config space (port-based, cf. bsp_eth.c) ---------------- */

static u32 pci_addr(u8 bus, u8 dev, u8 fn, u8 off)
{
    return 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) |
           ((u32)fn << 8) | (off & 0xFCu);
}

static u32 pci_r32(u8 bus, u8 dev, u8 fn, u8 off)
{
    x86_outl(PCI_CFG_ADDR_PORT, pci_addr(bus, dev, fn, off));
    return x86_inl(PCI_CFG_DATA_PORT);
}

static u16 pci_r16(u8 bus, u8 dev, u8 fn, u8 off)
{
    u32 v = pci_r32(bus, dev, fn, off & ~3u);
    return (u16)(v >> ((off & 2) * 8));
}

static void pci_w16(u8 bus, u8 dev, u8 fn, u8 off, u16 val)
{
    u32 a = off & ~3u;
    u32 v = pci_r32(bus, dev, fn, a);
    if (off & 2)
        v = (v & 0x0000FFFFu) | ((u32)val << 16);
    else
        v = (v & 0xFFFF0000u) | val;
    x86_outl(PCI_CFG_ADDR_PORT, pci_addr(bus, dev, fn, a));
    x86_outl(PCI_CFG_DATA_PORT, v);
}

/* ---------------- discovery ---------------- */

int iwlm_pcie_probe(struct iwlm_dev *d)
{
    int bus, dev, fn;

    for (bus = 0; bus < 256 && !d->cfg; bus++) {
        for (dev = 0; dev < 32 && !d->cfg; dev++) {
            for (fn = 0; fn < 8 && !d->cfg; fn++) {
                u32 id = pci_r32(bus, dev, fn, 0);
                u16 vendor = id & 0xFFFF, device = id >> 16;
                u32 i;

                if (vendor != IWL_PCI_VENDOR)
                    continue;
                if (fn > 0 && !(pci_r32(bus, dev, 0, 0x0C) & 0x800000))
                    break;   /* not multi-function */
                for (i = 0; i < IWLM_NUM_DEVS; i++) {
                    if (iwlm_devs[i].device == device) {
                        d->cfg = &iwlm_devs[i];
                        d->bus = bus;
                        d->slot = dev;
                        d->fn = fn;
                        break;
                    }
                }
            }
        }
    }
    if (!d->cfg)
        return -1;

    /* enable memory space + bus master, disable INTx */
    {
        u16 cmd = pci_r16(d->bus, d->slot, d->fn, PCI_CFG_CMD);
        cmd |= PCI_CMD_MEM_ENABLE | PCI_CMD_BUS_MASTER | PCI_CMD_INTX_DISABLE;
        pci_w16(d->bus, d->slot, d->fn, PCI_CFG_CMD, cmd);
    }

    /* map BAR0 */
    {
        u32 bar = pci_r32(d->bus, d->slot, d->fn, PCI_CFG_BAR0) & ~0xFu;
        if (!bar)
            return -2;
        d->mmio_size = IWL_BAR_SIZE;
        d->mmio = (volatile u8 *)syscall3(SYS_MEM_MAP, IWL_BAR_VA, bar,
                                          IWL_BAR_SIZE);
        if (!d->mmio)
            return -3;
    }

    d->hw_rev = iwlm_read32(d, CSR_HW_REV);
    d->hw_rf_id = iwlm_read32(d, CSR_HW_RF_ID);
    return 0;
}

/* ---------------- CSR ---------------- */

void iwlm_write32(struct iwlm_dev *d, u32 ofs, u32 val)
{
    *(volatile u32 *)(d->mmio + ofs) = val;
}

u32 iwlm_read32(struct iwlm_dev *d, u32 ofs)
{
    return *(volatile u32 *)(d->mmio + ofs);
}

void iwlm_set_bit(struct iwlm_dev *d, u32 ofs, u32 bits)
{
    iwlm_write32(d, ofs, iwlm_read32(d, ofs) | bits);
}

void iwlm_clear_bit(struct iwlm_dev *d, u32 ofs, u32 bits)
{
    iwlm_write32(d, ofs, iwlm_read32(d, ofs) & ~bits);
}

int iwlm_poll_bit(struct iwlm_dev *d, u32 ofs, u32 bits, u32 expected, int ms)
{
    int i;
    for (i = 0; i < ms * 10; i++) {
        if ((iwlm_read32(d, ofs) & bits) == expected)
            return 0;
        usleep(100);
    }
    return -1;
}

/* ---------------- PRPH (indirect, needs NIC access) ---------------- */

/* grab nic access: request MAC clock, up to 1.5s like iwl_grab_nic_access */
u32 iwlm_grab_nic_access(struct iwlm_dev *d)
{
    int i;

    if (d->cfg->family == IWLM_FAM_BZ)
        iwlm_set_bit(d, CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_BZ_MAC_ACCESS_REQ);
    else
        iwlm_set_bit(d, CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);

    for (i = 0; i < 15000; i++) {
        u32 gp = iwlm_read32(d, CSR_GP_CNTRL);
        if (gp & (CSR_GP_CNTRL_REG_VAL_MAC_ACCESS_EN |
                  CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY))
            return 0;
        usleep(100);
    }
    return 1;
}

void iwlm_release_nic_access(struct iwlm_dev *d)
{
    if (d->cfg->family == IWLM_FAM_BZ)
        iwlm_clear_bit(d, CSR_GP_CNTRL,
                       CSR_GP_CNTRL_REG_FLAG_BZ_MAC_ACCESS_REQ);
    else
        iwlm_clear_bit(d, CSR_GP_CNTRL,
                       CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
}

void iwlm_write_prph(struct iwlm_dev *d, u32 ofs, u32 val)
{
    if (iwlm_grab_nic_access(d))
        return;
    iwlm_write32(d, HBUS_TARG_PRPH_WADDR, (ofs & 0x00FFFFFF) | (3 << 24));
    iwlm_write32(d, HBUS_TARG_PRPH_WDAT, val);
    iwlm_release_nic_access(d);
}

u32 iwlm_read_prph(struct iwlm_dev *d, u32 ofs)
{
    u32 val = 0;

    if (iwlm_grab_nic_access(d))
        return 0xdeadbeef;
    iwlm_write32(d, HBUS_TARG_PRPH_RADDR, (ofs & 0x00FFFFFF) | (3 << 24));
    val = iwlm_read32(d, HBUS_TARG_PRPH_RDAT);
    iwlm_release_nic_access(d);
    return val;
}

void iwlm_write_prph64(struct iwlm_dev *d, u32 ofs, u64 val)
{
    if (iwlm_grab_nic_access(d))
        return;
    iwlm_write32(d, HBUS_TARG_PRPH_WADDR, (ofs & 0x00FFFFFF) | (3 << 24));
    iwlm_write32(d, HBUS_TARG_PRPH_WDAT, (u32)val);
    iwlm_write32(d, HBUS_TARG_PRPH_WDAT, (u32)(val >> 32));
    iwlm_release_nic_access(d);
}

/* ---------------- reset / card preparation ---------------- */

/* iwl_trans_pcie_sw_reset (gen2): full chip reset */
int iwlm_sw_reset(struct iwlm_dev *d)
{
    if (d->cfg->family == IWLM_FAM_BZ) {
        iwlm_set_bit(d, CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_SW_RESET);
        usleep(10000);
        if (iwlm_poll_bit(d, CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_SW_RESET,
                          0, 20000))
            return -1;
    } else {
        iwlm_set_bit(d, CSR_RESET, CSR_RESET_REG_FLAG_SW_RESET);
        usleep(5000);
        if (iwlm_poll_bit(d, CSR_RESET, CSR_RESET_REG_FLAG_SW_RESET,
                          0, 5000))
            return -1;
    }
    return 0;
}

static bool iwlm_nic_ready(struct iwlm_dev *d)
{
    u32 v = iwlm_read32(d, CSR_HW_IF_CONFIG_REG);
    iwlm_set_bit(d, CSR_HW_IF_CONFIG_REG, CSR_HW_IF_CONFIG_REG_BIT_NIC_READY);
    return !iwlm_poll_bit(d, CSR_HW_IF_CONFIG_REG,
                          CSR_HW_IF_CONFIG_REG_BIT_NIC_READY,
                          CSR_HW_IF_CONFIG_REG_BIT_NIC_READY, 50000);
}

/* iwl_pcie_prepare_card_hw: take the device out of platform power states */
int iwlm_prepare_card_hw(struct iwlm_dev *d)
{
    int iter, ret = 0;

    iwlm_set_bit(d, CSR_HW_IF_CONFIG_REG, CSR_HW_IF_CONFIG_REG_BIT_NIC_READY);
    if (iwlm_nic_ready(d))
        goto out;

    /* if the card is ready, exit 0 */
    ret = 1;
    for (iter = 0; iter < 10; iter++) {
        /* set HW_PREPARE: request the platform to wake the NIC */
        iwlm_set_bit(d, CSR_HW_IF_CONFIG_REG,
                     CSR_HW_IF_CONFIG_REG_PREPARE);
        if (iwlm_poll_bit(d, CSR_HW_IF_CONFIG_REG,
                          CSR_HW_IF_CONFIG_REG_BIT_NIC_PREPARE_DONE,
                          CSR_HW_IF_CONFIG_REG_BIT_NIC_PREPARE_DONE, 750))
            continue;
        if (iwlm_nic_ready(d)) {
            ret = 0;
            break;
        }
    }
out:
    if (!ret) {
        /* HW keep-warm: tell the CSME the OS is alive */
        iwlm_set_bit(d, CSR_MBOX_SET_REG, CSR_MBOX_SET_REG_OS_ALIVE);
        usleep(1000);
    }
    return ret;
}

/* gen2 APM init (iwl_pcie_gen2_apm_init + iwl_finish_nic_init core) */
int iwlm_apm_init(struct iwlm_dev *d)
{
    /* Disable L0s without affecting L1 (ICH bug W/A) */
    iwlm_set_bit(d, CSR_GIO_CHICKEN_BITS,
                 CSR_GIO_CHICKEN_BITS_REG_BIT_L1A_NO_L0S_RX);

    /* Set FH wait threshold to maximum (HW error during stress W/A) */
    iwlm_set_bit(d, CSR_DBG_HPET_MEM_REG, CSR_DBG_HPET_MEM_REG_VAL);

    /* Enable HAP INTA to wake device's PCIe link L1a -> L0s */
    iwlm_set_bit(d, CSR_HW_IF_CONFIG_REG,
                 CSR_HW_IF_CONFIG_REG_BIT_HAP_WAKE_L1A);

    /* disable analog (S)POR?  apm_config for gen2: GIO L0S disable */
    iwlm_write32(d, CSR_GIO_REG, CSR_GIO_REG_VAL_L0S_DISABLED);

    /* clear "initialization complete" (move to D0U), then set MAC clock on */
    if (d->cfg->family == IWLM_FAM_BZ)
        iwlm_clear_bit(d, CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_MAC_INIT);
    else
        iwlm_clear_bit(d, CSR_GP_CNTRL, CSR_GP_CNTRL_REG_FLAG_INIT_DONE);

    /* wait for clock: CSR_GP_CNTRL MAC_CLOCK_READY after access request */
    if (iwlm_grab_nic_access(d))
        return -1;
    iwlm_release_nic_access(d);
    return 0;
}
