#include <stdint.h>
#include <bsp/bsp_wdt.h>
#include <ewoksys/syscall.h>
#include <ewoksys/klog.h>
#include <orin_net.h>

/*
 * TEST-ONLY SBSA Generic Watchdog driver (Tegra234).  Single-stage mode
 * (WS0 ignored, WS1 resets at timeout).  Protocol from upstream sbsa_gwdt.c:
 *   WOR = (cntfrq / 2) * timeout   (reset happens at WOR*2 == timeout)
 *   WCV = system_counter + WOR     (refreshed on enable / pet)
 *   enable: WCS = EN ; pet: write WRR ; stop: WCS = 0
 * The system counter is CNTVCT_EL0 (matches arch_timer_read_counter); on
 * T234 CNTVOFF is 0 so it equals the physical counter the WDT compares.
 * Hardware-unverified; the reset path (WS1 -> SoC reset) is confirmed by
 * NVIDIA to fire on a CPU hang.
 */

#define WDT_CTRL_VA    0x69000000ULL
#define WDT_REFRESH_VA 0x69010000ULL

static volatile uint8_t* _ctrl;
static volatile uint8_t* _refresh;
static uint32_t _cntfrq;
static int _version;
static int _inited;

static inline uint32_t rd32(volatile uint8_t* b, uint32_t off) {
    return *(volatile uint32_t*)(b + off);
}
static inline void wr32(volatile uint8_t* b, uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(b + off) = v;
}
static inline uint64_t rd64(volatile uint8_t* b, uint32_t off) {
    uint32_t lo = *(volatile uint32_t*)(b + off);
    uint32_t hi = *(volatile uint32_t*)(b + off + 4);
    return ((uint64_t)hi << 32) | lo;
}
static inline void wr64(volatile uint8_t* b, uint32_t off, uint64_t v) {
    *(volatile uint32_t*)(b + off) = (uint32_t)v;
    *(volatile uint32_t*)(b + off + 4) = (uint32_t)(v >> 32);
}

static inline uint64_t counter(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}

static int map(uint64_t va, uint64_t phy, uint32_t size) {
    return syscall3(SYS_MEM_MAP, (ewokos_addr_t)va, (ewokos_addr_t)phy,
            (ewokos_addr_t)size) == (ewokos_addr_t)va ? 0 : -1;
}

int bsp_wdt_init(void) {
    if (_inited)
        return 0;
    if (map(WDT_CTRL_VA, ORIN_WDT_CTRL_PHY, ORIN_WDT_FRAME_SIZE) != 0 ||
            map(WDT_REFRESH_VA, ORIN_WDT_REFRESH_PHY, ORIN_WDT_FRAME_SIZE) != 0) {
        klog("orin wdt: frame map failed\n");
        return -1;
    }
    _ctrl = (volatile uint8_t*)(uintptr_t)WDT_CTRL_VA;
    _refresh = (volatile uint8_t*)(uintptr_t)WDT_REFRESH_VA;
    uint32_t frq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
    _cntfrq = frq ? frq : 31250000u;
    uint32_t iidr = rd32(_ctrl, ORIN_WDT_W_IIDR);
    _version = (iidr >> 16) & 0xf;
    _inited = 1;
    klog("orin wdt: SBSA GWDT cntfrq=%u version=%d iidr=%08x wcs=%08x\n",
            _cntfrq, _version, iidr, rd32(_ctrl, ORIN_WDT_WCS));
    return 0;
}

static void write_wor(uint64_t ticks) {
    if (_version == 0)
        wr32(_ctrl, ORIN_WDT_WOR, (uint32_t)ticks);
    else
        wr64(_ctrl, ORIN_WDT_WOR, ticks);
}

int bsp_wdt_start(uint32_t timeout_sec) {
    if (bsp_wdt_init() != 0)
        return -1;
    if (timeout_sec == 0)
        timeout_sec = ORIN_WDT_RUN_TIMEOUT;
    /* Single-stage: reset at WOR*2, so WOR = cntfrq*timeout/2. */
    uint64_t wor = ((uint64_t)_cntfrq / 2u) * timeout_sec;
    write_wor(wor);
    wr64(_ctrl, ORIN_WDT_WCV, counter() + wor);
    wr32(_ctrl, ORIN_WDT_WCS, ORIN_WDT_WCS_EN);  /* enable + explicit refresh */
    klog("orin wdt: enabled, timeout=%us (wor=%llu ticks)\n",
            timeout_sec, (unsigned long long)wor);
    return 0;
}

void bsp_wdt_pet(void) {
    if (_inited)
        wr32(_refresh, ORIN_WDT_WRR, 0);
}

void bsp_wdt_stop(void) {
    if (_inited)
        wr32(_ctrl, ORIN_WDT_WCS, 0);
}

int bsp_wdt_enabled(void) {
    return _inited && (rd32(_ctrl, ORIN_WDT_WCS) & ORIN_WDT_WCS_EN);
}

int bsp_wdt_reset_caused(void) {
    return _inited && (rd32(_ctrl, ORIN_WDT_WCS) & ORIN_WDT_WCS_WS1);
}