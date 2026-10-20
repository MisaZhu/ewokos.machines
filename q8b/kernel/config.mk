#----- Radxa Dragon Q8B (Qualcomm SC8280XP / Snapdragon 8cx Gen 3) -----
ARCH        = aarch64
ARCH_VER    = v8

# The machine boots through its on-board EDK2 UEFI firmware (no U-Boot).
# boot/uefi/BOOTAA64.EFI places the kernel at this physical address and
# jumps to its first byte; the kernel is linked with .init at the same
# physical address and with its high-half VMA offset by RAM_BASE.
ifeq ($(TEST_QEMU),1)
# QEMU virt smoke test: same kernel code, QEMU's DRAM base and PL011/GICv2.
# The kernel still links .init at 0x80080000 so the same UEFI stub/test
# payload layout applies.
LOAD_ADDRESS = 0x80080000
RAM_BASE     = 0x40000000
else
LOAD_ADDRESS = 0x90080000
RAM_BASE     = 0x80000000
endif

# QEMU has no sc8280xp machine model; the name only produces a clear
# error if someone tries `make run`.
QEMU_MACHINE = q8b

#----multi core(SMP)------
# First hardware bring-up is single-core: `make SMP=no` produces
# kernel_up.bin in its own object directory (q8b_up), so the SMP and UP
# objects can never be mixed by make's timestamp rules.
ifeq ($(TEST_QEMU),1)
SMP=no
CFLAGS += -DQ8B_QEMU_TEST
MACHINE = q8b_qemu
KERNEL_VARIANT = _qemu
else
CFLAGS += -DQ8B_BOARD
ifeq ($(SMP),)
SMP=yes
endif
ifeq ($(SMP),no)
MACHINE = q8b_up
KERNEL_VARIANT = _up
else
MACHINE = q8b
KERNEL_VARIANT =
endif
endif

# QCOM SC8280XP supports 4K/16K/64K granules. 16K matches the existing
# aarch64 port (raspi5/machine.virt) and avoids the lightly used 4K path.
#PAGE_SIZE=4k
PAGE_SIZE=16k
#PAGE_SIZE=64k
