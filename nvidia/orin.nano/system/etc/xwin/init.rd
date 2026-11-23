# Orin Nano self-root xwin desktop profile (packed as /etc/init.rd).
#
# init's rootfs=self path runs this straight from the RAMFS (switch_sys, root),
# then falls through to rescue_shell which adds uartd + the serial maintenance
# session.  Core services (timerd/piped/ramfsd/nulld) and USB HID input
# (hid_keybd /dev/keyb0, hid_moused /dev/mouse0) are already up from
# /etc/init.ramfs.rd, so this profile only brings up the display stack, the X
# server and the misawm desktop session.
#
# Display is CPU-rendered: Orin has no usable GPU (signed falcon fw blocker),
# so no g2dd is started -- displayd/xserverd fall back to their CPU blit/rotate
# kernels and fbdisplayd flushes the linear scan-out FB with memcpy.  The FB
# phys is the UEFI provider's engine-reported scan-out surface (read_provider_fb).
#
# No touch (xtouch), wlan, btd, battery (powerd/adspas) or audio on Orin: the
# desktop is driven purely by the USB keyboard/mouse through xim_none/xmouse.
@/bin/bgrun /sbin/sudod

@/bin/ipcserv /sbin/sessiond

# Display bring-up (4K probe + DCE init + modeset) now runs in-process inside
# fbdisplayd's init, scanning out the kernel-reserved 4K FB (pd.fb) it renders
# into.  No separate dceprobe launch is needed; fbdisplayd's residency keeps the
# DCE channels/instance memory alive.
@/bin/ipcserv /drivers/displaymand
@/bin/ipcserv /drivers/orin.nano/fbdisplayd /dev/disp0
@/bin/ipcserv /drivers/fontd
# The desktop owns the panel; the graphical console (consoled) is not started.
# The serial rescue session from rescue_shell stays available on /dev/tty0.

@/bin/ipcserv /drivers/xserverd          /dev/x
@/bin/bgrun /sbin/x/xmouse  /dev/mouse0
@/bin/bgrun /sbin/x/xim_none /dev/keyb0
@/bin/bgrun /bin/x/xsession misa