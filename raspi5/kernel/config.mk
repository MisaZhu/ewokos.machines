#-----raspberry pi 5 (BCM2712) aarch64 config----------
ARCH        = aarch64
ARCH_VER    = v8

# firmware loads kernel8.img at 0x80000
LOAD_ADDRESS = 0x80000

# upstream QEMU has no raspi5 machine model yet, keep the name for
# a clear error message; this port targets real hardware.
QEMU_MACHINE = raspi5b

#----multi core(SMP)------
SMP=yes

#PAGE_SIZE=4k
#PAGE_SIZE=64k
PAGE_SIZE=16k
