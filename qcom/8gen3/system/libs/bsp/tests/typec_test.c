/* Exercise the real Q8B host handoff, xHCI allocator and BSP routing with
 * register/DMA peers. No USB hardware or firmware is required. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ewoksys/mmio.h>
#include <bsp/xhci.h>

static uint32_t regs[3][0x10000 / 4];
static uint64_t elapsed_ms;
static int stuck_device, stuck_reset, fail_mapping;
static unsigned reg_writes, role_resets;
static unsigned char dma_memory[1024 * 1024 + 0x10000];
static uint32_t fake_get32(ewokos_addr_t addr);
static void fake_put32(ewokos_addr_t addr, uint32_t value);
static int fake_usleep(useconds_t us);

#undef get32
#undef put32
#define get32(addr) fake_get32(addr)
#define put32(addr, value) fake_put32(addr, value)
#define usleep fake_usleep
#include "../src/dwc3.c"
#include "../src/xhci.c"
#include "../src/bsp_usb.c"
#undef usleep

static int reg_index(ewokos_addr_t addr, uint32_t* offset) {
    assert(addr >= XHCI_BAR_VA_BASE);
    uint32_t rel = (uint32_t)(addr - XHCI_BAR_VA_BASE);
    int id = rel / XHCI_BAR_MAP_SIZE;
    assert(id < 3 && (rel & 3) == 0);
    assert(id != fail_mapping);
    *offset = rel % XHCI_BAR_MAP_SIZE;
    return id;
}

static uint32_t fake_get32(ewokos_addr_t addr) {
    uint32_t off;
    int id = reg_index(addr, &off);
    return regs[id][off / 4];
}

static void fake_put32(ewokos_addr_t addr, uint32_t value) {
    uint32_t off;
    int id = reg_index(addr, &off);
    uint32_t* r = regs[id];
    ++reg_writes;
    r[off / 4] = value;
    if (off == DWC3_GCTL && (value & GCTL_CORESOFTRESET))
        ++role_resets;
    if (off == DWC3_DCTL && !(value & DCTL_RUN_STOP) && id != stuck_device)
        r[DWC3_DSTS / 4] |= DSTS_DEVCTRLHLT;
    if (off == 0x20 + XHCI_USBCMD) {
        if ((value & USBCMD_HCRST) && id != stuck_reset)
            r[off / 4] = 0; /* HCRST self clears */
        r[(0x20 + XHCI_USBSTS) / 4] = (value & USBCMD_RUN) ? 0 : USBSTS_HCH;
    }
    if (off == 0x1000) { /* command doorbell */
        xhci_hc_t* hc = &_xhcs[id];
        uint32_t* e = &hc->evt[hc->evt_deq * 4];
        e[0] = (uint32_t)hc->cmd_trb_phys;
        e[1] = (uint32_t)(hc->cmd_trb_phys >> 32);
        e[2] = CC_SUCCESS << 24;
        e[3] = TRB_TYPE(TRB_EVT_CMD) | (1u << 24) | hc->evt_cycle;
    }
}

static int fake_usleep(useconds_t us) {
    elapsed_ms += (us + 999) / 1000;
    return 0;
}
uint64_t kernel_tic_ms(int zone) { (void)zone; return elapsed_ms; }
void klog(const char* fmt, ...) { (void)fmt; }
ewokos_addr_t dma_alloc(int32_t block, uint32_t size) {
    (void)block;
    assert(size == sizeof(dma_memory));
    return (ewokos_addr_t)dma_memory;
}
ewokos_addr_t dma_phy_addr(int32_t block, ewokos_addr_t addr) {
    (void)block;
    assert(addr == (ewokos_addr_t)dma_memory);
    return 0xc8600000;
}
int sys_get_sys_info(sys_info_t* info) {
    memset(info, 0, sizeof(*info));
    info->page_size = 16384;
    return 0;
}
ewokos_addr_t syscall3_raw(int code, ewokos_addr_t va, ewokos_addr_t pa,
        ewokos_addr_t size) {
    assert(code == SYS_MEM_MAP && size == XHCI_BAR_MAP_SIZE);
    int id = (va - XHCI_BAR_VA_BASE) / XHCI_BAR_MAP_SIZE;
    assert(id >= 0 && id < 3 && pa == Q8B_XHCI_PHY_BASE[id]);
    return id == fail_mapping ? 0 : va;
}

static void setup(void) {
    memset(regs, 0, sizeof(regs));
    memset(_xhcs, 0, sizeof(_xhcs));
    memset(_xhc_mapped, 0, sizeof(_xhc_mapped));
    _inited = false;
    stuck_device = stuck_reset = fail_mapping = -1;
    elapsed_ms = reg_writes = role_resets = 0;
    for (int i = 0; i < 3; ++i) {
        uint32_t* r = regs[i];
        r[0] = 0x01100020;
        r[XHCI_HCSPARAMS1 / 4] = ((i == 0 ? 6u : 2u) << 24) | 16;
        r[XHCI_HCSPARAMS2 / 4] = 2u << 27; /* two scratchpads */
        r[XHCI_HCCPARAMS1 / 4] = 1u << 2; /* 64-byte contexts */
        r[XHCI_DBOFF / 4] = 0x1000;
        r[XHCI_RTSOFF / 4] = 0x2000;
        r[(0x20 + XHCI_USBSTS) / 4] = USBSTS_HCH;
        r[(0x20 + XHCI_PAGESIZE) / 4] = 1 | 4; /* 4K and 16K */
        r[DWC3_GSNPSID / 4] = 0x5533330a;
        r[DWC3_GCTL / 4] = GCTL_PRTCAP_HOST | 0x100;
        r[DWC3_GUSB2PHYCFG / 4] = GUSB2PHYCFG_SUSPHY | 0x200;
        r[DWC3_GUSB3PIPECTL / 4] = GUSB3PIPECTL_SUSPHY | 0x80;
    }
}

static void test_roles(void) {
    setup();
    ewokos_addr_t base = XHCI_BAR_VA_BASE + XHCI_BAR_MAP_SIZE;
    assert(dwc3_host_prepare(1, base) == 0);
    assert(role_resets == 0); /* preserve firmware host setup */
    assert(regs[1][DWC3_GCTL / 4] == (GCTL_PRTCAP_HOST | 0x100));
    assert(regs[1][DWC3_GUSB2PHYCFG / 4] == 0x200);
    assert(regs[1][DWC3_GUSB3PIPECTL / 4] == 0x80);
    regs[1][DWC3_GCTL / 4] = (2u << 12) | 0x100;
    regs[1][DWC3_DCTL / 4] = DCTL_RUN_STOP;
    assert(dwc3_host_prepare(1, base) == 0);
    assert(role_resets == 1 && elapsed_ms == 100);
    assert(!(regs[1][DWC3_DCTL / 4] & DCTL_RUN_STOP));
    assert(regs[1][DWC3_GCTL / 4] == (GCTL_PRTCAP_HOST | 0x100));

    setup();
    regs[1][DWC3_GCTL / 4] = 2u << 12;
    regs[1][DWC3_DCTL / 4] = DCTL_RUN_STOP;
    stuck_device = 1;
    assert(dwc3_host_prepare(1, base) == -1);
    assert(elapsed_ms > 500 && role_resets == 0);
    assert(regs[1][DWC3_GCTL / 4] == 2u << 12);
    setup();
    regs[1][DWC3_GSNPSID / 4] = 0xffffffff;
    assert(dwc3_host_prepare(1, base) == -1 && reg_writes == 0);
}

static void test_controllers(void) {
    setup();
    assert(bsp_usb_init() == 0);
    assert(bsp_usb_root_port_count() == 10);
    for (int p = 1; p <= 10; ++p) {
        int local;
        xhci_hc_t* hc = flat_to_xhc(p, &local);
        int id = p <= 6 ? 0 : (p <= 8 ? 1 : 2);
        assert(hc == &_xhcs[id]);
        assert(local == p - (id == 0 ? 0 : (id == 1 ? 6 : 8)));
        assert((hc->cmd_phys & 0xffff) + RING_BYTES <= 0x10000);
        uint64_t* scratch = (uint64_t*)(_pool.virt + hc->dcbaa[0] - _pool.bus);
        assert((scratch[0] & 0x3fff) == 0 && (scratch[1] & 0x3fff) == 0);
    }
    assert(flat_to_xhc(11, NULL) == NULL);

    /* A reset of USB-C0 must not damage USB-A or USB-C1, even with the same
     * local slot id on all controllers. Run real command/event processing. */
    xhci_dev_t devices[3];
    for (int i = 0; i < 3; ++i) {
        assert(xhci_device_attach(&_xhcs[i], 1, XHCI_SPEED_HIGH, NULL, 0,
                    &devices[i]) == 0);
        assert(devices[i].slot_id == 1);
        devices[i].eps[1].ring[0] = 0xaabb0000u + i;
        _xhcs[i].cmd[0] = 0xccdd0000u + i;
    }
    uint64_t cmd_addr = _xhcs[1].cmd_phys;
    assert(q8b_xhci_start(1) == 0);
    assert(_xhcs[1].cmd_phys == cmd_addr); /* recycle, without leaking */
    for (int i = 0; i < 3; i += 2) {
        assert(_xhcs[i].cmd[0] == 0xccdd0000u + i);
        assert(devices[i].eps[1].ring[0] == 0xaabb0000u + i);
        assert(_xhcs[i].slot_dev[1] == &devices[i]);
        assert(((xhci_arena_t*)devices[i].arena)->used);
        assert(xhci_configure_hub(&devices[i], 4) == 0);
    }
    assert(!((xhci_arena_t*)devices[1].arena)->used);
    assert(xhci_device_attach(&_xhcs[1], 1, XHCI_SPEED_HIGH, NULL, 0,
                &devices[1]) == 0);
    assert(devices[0].arena != devices[1].arena &&
            devices[1].arena != devices[2].arena);

    regs[2][(0x20 + XHCI_PORTSC(2)) / 4] = PORTSC_CCS | PORTSC_CSC;
    assert(bsp_usb_root_port_connected(10));
    assert(bsp_usb_root_port_changes() & (1u << 9));
    _xhcs[0].present = false;
    assert(bsp_usb_root_port_count() == 10);
    assert(flat_to_xhc(1, NULL) == NULL);
    int local;
    assert(flat_to_xhc(9, &local) == &_xhcs[2] && local == 1);
    regs[2][(0x20 + XHCI_PORTSC(2)) / 4] = PORTSC_CCS | PORTSC_CSC;
    assert(bsp_usb_root_port_changes() & (1u << 9));
}

static void test_missing_controller(void) {
    setup();
    fail_mapping = 1;
    assert(bsp_usb_init() == 0);
    assert(bsp_usb_root_port_count() == 8);
    int local;
    assert(flat_to_xhc(7, &local) == &_xhcs[2] && local == 1);
    assert(!_xhc_mapped[1] && _xhcs[0].present && _xhcs[2].present);
    setup();
    stuck_reset = 1;
    assert(bsp_usb_init() == 0);
    assert(!_xhcs[1].present && _xhcs[0].present && _xhcs[2].present);
    assert(bsp_usb_root_port_count() == 8);
}

int main(void) {
    test_roles();
    test_controllers();
    test_missing_controller();
    puts("Q8B Type-C: roles, three controllers, DMA isolation and port routing passed");
    return 0;
}
