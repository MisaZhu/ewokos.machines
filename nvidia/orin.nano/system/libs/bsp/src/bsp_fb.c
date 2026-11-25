#include <stdint.h>
#include <string.h>
#include <bsp/bsp_fb.h>
#include <ewoksys/klog.h>
#include <ewoksys/sys.h>
#include <ewoksys/syscall.h>
#include <sysinfo.h>
#include <qcom8gen3_platform.h>

/*
 * Orin Nano framebuffer: the UEFI firmware (GOP) has already programmed the
 * scan-out when a display is attached.  We only map it into this process and
 * report its geometry; the caller (fbdisplayd) does pixel writes.
 *
 * The kernel reserves the GOP range out of its heap and lets SYS_MEM_MAP map
 * it (Normal Non-Cacheable), so writes reach DRAM without cache aliasing.
 * Headless boots hand off fb.phy_base=0 and bsp_fb_init fails cleanly.
 */

#define ORIN_FB_VA 0x50000000ULL

static disp_info_t _fbinfo;

disp_info_t* bsp_get_fbinfo(void) {
    return &_fbinfo;
}

int32_t bsp_fb_init(uint32_t w, uint32_t h, uint32_t dep) {
    sys_info_t sysinfo;
    if (sys_get_sys_info(&sysinfo) != 0) {
        klog("orin fb: SYS_GET_SYS_INFO failed\n");
        return -1;
    }

    const qcom8gen3_platform_data_t* pd =
        (const qcom8gen3_platform_data_t*)sysinfo.platform_data;

    if (pd->fb.phy_base == 0 || pd->fb.width == 0 || pd->fb.height == 0 ||
            pd->fb.pitch == 0) {
        klog("orin fb: no GOP handoff base=0x%llx %ux%u pitch=%u (headless?)\n",
                (unsigned long long)pd->fb.phy_base, pd->fb.width,
                pd->fb.height, pd->fb.pitch);
        return -1;
    }

    uint64_t bytes64 = (uint64_t)pd->fb.pitch * pd->fb.height;
    uint64_t map_bytes64 = (bytes64 + 4095u) & ~4095ULL;
    if (bytes64 == 0 || map_bytes64 > UINT32_MAX) {
        klog("orin fb: GOP mapping too large: %llu bytes\n",
                (unsigned long long)bytes64);
        return -1;
    }
    uint32_t bytes = (uint32_t)bytes64;
    uint32_t bytes_max = (uint32_t)map_bytes64;

    memset(&_fbinfo, 0, sizeof(_fbinfo));
    _fbinfo.pointer = ORIN_FB_VA;
    _fbinfo.size = bytes;
    _fbinfo.size_max = bytes_max;
    _fbinfo.width = pd->fb.width;
    _fbinfo.height = pd->fb.height;
    _fbinfo.vwidth = pd->fb.width;
    _fbinfo.vheight = pd->fb.height;
    _fbinfo.depth = pd->fb.bpp;
    _fbinfo.pitch = pd->fb.pitch;
    _fbinfo.dma_id = -1;
    _fbinfo.phy_base = pd->fb.phy_base;
    _fbinfo.bus_base = pd->fb.phy_base;

    ewokos_addr_t mapped = syscall3(SYS_MEM_MAP, (ewokos_addr_t)ORIN_FB_VA,
                (ewokos_addr_t)pd->fb.phy_base,
                (ewokos_addr_t)bytes_max);
    if (mapped != (ewokos_addr_t)ORIN_FB_VA) {
        klog("orin fb: SYS_MEM_MAP failed pa=0x%llx va=0x%llx size=%u ret=0x%llx\n",
                (unsigned long long)pd->fb.phy_base,
                (unsigned long long)ORIN_FB_VA, bytes_max,
                (unsigned long long)mapped);
        memset(&_fbinfo, 0, sizeof(_fbinfo));
        return -1;
    }

    klog("orin fb: mapped pa=0x%llx va=0x%llx %ux%u bpp=%u pitch=%u format=%u size=%u\n",
            (unsigned long long)pd->fb.phy_base,
            (unsigned long long)ORIN_FB_VA, pd->fb.width, pd->fb.height,
            pd->fb.bpp, pd->fb.pitch, pd->fb.format, bytes);

    (void)w;
    (void)h;
    (void)dep;
    return 0;
}