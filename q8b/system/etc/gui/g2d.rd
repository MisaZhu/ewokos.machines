# Shared by the graphical-console and full-desktop boot profiles.
# ipcserv waits for g2dd's readiness notification before clients start.
# Firmware is installed under /etc/firmware/adreno by the rootfs build.
@export ADRENO_ENABLE=1
@/bin/ipcserv /drivers/q8b/g2dd /dev/g2d
