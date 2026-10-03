#include <Hypervisor/hv.h>
#include <Hypervisor/hv_vmx.h>
#include <stdio.h>
int main(void) {
#if defined(__x86_64__)
    printf("arch: x86_64 (Rosetta)\n");
    hv_return_t r = hv_vm_create(0);
    printf("hv_vm_create(0): 0x%x %s\n", r, r == HV_SUCCESS ? "SUCCESS" : "FAILED");
    if (r != HV_SUCCESS) return 1;
    hv_vcpuid_t vcpu;
    r = hv_vcpu_create(&vcpu, 0);
    printf("hv_vcpu_create: 0x%x %s (vcpu=%llu)\n", r, r == HV_SUCCESS ? "SUCCESS" : "FAILED", (unsigned long long)vcpu);
#else
    printf("arch: arm64 (native)\n");
    hv_return_t r = hv_vm_create(NULL);
    printf("hv_vm_create(NULL): 0x%x %s\n", r, r == HV_SUCCESS ? "SUCCESS" : "FAILED");
#endif
    return 0;
}
