#----- NVIDIA Jetson Orin Nano (Tegra234 / T234, Cortex-A78AE) -----
ARCH        = aarch64
ARCH_VER    = v8

# The board boots through NVIDIA's on-board EDK2 UEFI firmware (no U-Boot).
# ../efi/BOOTAA64.EFI places the kernel at this physical address and jumps
# to its first byte; the kernel is linked with .init at the same physical
# address and with its high-half VMA offset by RAM_BASE.
# Tegra234 DRAM starts at 0x80000000; the 0x90000000 window is above the
# low firmware carveouts (TZDRAM/OP-TEE region) and is claimed through UEFI
# AllocatePages before use.
LOAD_ADDRESS = 0x90080000
RAM_BASE     = 0x80000000

QEMU_MACHINE = orin.nano

#----multi core(SMP)------
# Orin Nano has 6 A78AE cores in two clusters (4+2).  `make SMP=no`
# produces kernel_up.bin in its own object directory (orin.nano_up).
CFLAGS += -DORIN_BOARD
# TEST-only hardware watchdog (WATCHDOG=1): arm SBSA WDT early in kernel boot.
# Removable; default off.  See README "Test watchdog".
ifeq ($(WATCHDOG),1)
CFLAGS += -DORIN_WATCHDOG_TEST
endif
ifeq ($(SMP),)
SMP=yes
endif
ifeq ($(SMP),no)
MACHINE = orin.nano_up
KERNEL_VARIANT = 
else
MACHINE = orin.nano
KERNEL_VARIANT =
endif

# Tegra234 supports 4K/16K/64K granules. 16K matches the other aarch64
# ports (raspi5/machine.virt/8gen3).
PAGE_SIZE=16k
