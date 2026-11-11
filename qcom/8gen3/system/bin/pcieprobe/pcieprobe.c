/* Q8B PCIe enumeration client.  The pcied service owns power-on and ECAM. */
#include <bsp/pcie.h>
#include <stdio.h>
#include <string.h>

static const char *class_name(uint32_t class_code) {
	if (class_code == 0x010802) return "NVMe";
	if (class_code == 0x020000) return "Ethernet";
	if (class_code == 0x028000) return "network";
	if ((class_code >> 8) == 0x0604) return "bridge";
	return "device";
}

int main(int argc, char **argv) {
    q8b_pcie_device_t device;
    int n = q8b_pcie_service_get_count();
	int check_q8b = argc > 1 && !strcmp(argv[1], "--check-q8b");
	int ethernet_pf = 0;
	int wifi = 0;
	if (n < 0) {
        printf("pcieprobe: /dev/pcie0 unavailable (%d)\n", n);
		return 1;
	}
    printf("pcieprobe: PCIe service, %d function(s)\n", n);
	for (int i = 0; i < n; i++) {
        const q8b_pcie_device_t *d;
        if (q8b_pcie_service_get_device((unsigned)i, &device))
            return 1;
        d = &device;
		printf("  pcie%u %02x:%02x.%u %04x:%04x class=%06x (%s) "
				"sub=%04x:%04x link=Gen%u x%u bar0=%llx->%llx cmd=%04x\n",
				d->controller, d->bus, d->device, d->function, d->vendor_id, d->device_id,
				d->class_code, class_name(d->class_code), d->subsystem_vendor,
				d->subsystem_device, d->link_speed, d->link_width,
				(unsigned long long)d->bar[0],
				(unsigned long long)d->bar_phys[0], d->command);
		if (d->controller == Q8B_PCIE3A && d->vendor_id == 0x1179 &&
				d->device_id == 0x0220)
			ethernet_pf++;
		if (q8b_pcie_is_wlan(d))
			wifi++;
	}
	if (check_q8b) {
		printf("pcieprobe: q8b check ethernet_pf=%d wifi=%d\n",
				ethernet_pf, wifi);
		return ethernet_pf >= 2 && wifi >= 1 ? 0 : 2;
	}
	return 0;
}
