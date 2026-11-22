# Orin Nano self-root GUI profile (packed into the boot RAMFS as /etc/init.rd).
#
# init's rootfs=self path runs this straight from the RAMFS (switch_sys), then
# falls through to rescue_shell which adds uartd + the serial maintenance
# session.  Core services (timerd/piped/ramfsd/nulld) and USB HID input are
# already up from /etc/init.ramfs.rd, so this profile only brings up the
# framebuffer display stack and a graphical console session.
#
# Display is CPU-rendered: Orin has no usable GPU (signed falcon fw blocker),
# so no g2dd is started -- displayd falls back to its CPU blit/rotate kernels
# and fbdisplayd flushes the linear scan-out FB with memcpy.  The FB phys is
# the UEFI provider's engine-reported scan-out surface (read_provider_fb).
@/bin/ipcserv /drivers/displaymand
@/bin/ipcserv /drivers/orin.nano/fbdisplayd /dev/disp0
@/bin/ipcserv /drivers/fontd
@/bin/ipcserv /drivers/consoled
@set_stdio /dev/console0
@/bin/ipcserv /sbin/sessiond
@/bin/bgrun /bin/session -r -t /dev/console0