# Orin Nano self-root xwin desktop profile (packed as /etc/init.rd).
#
# init's rootfs=self path runs this straight from the RAMFS (switch_sys, root),
# then falls through to rescue_shell which adds uartd + the serial maintenance
# session.  Core services (timerd/piped/ramfsd/nulld) and USB HID input
# (hid_keybd /dev/keyb0, hid_moused /dev/mouse0) are already up from
# /etc/init.ramfs.rd, so this profile only brings up the display stack, the X
# server and the misawm desktop session.
#
# 2D offload is OFF by default: the two commented lines below start g2dd
# (shared 2D daemon, NEON engine) with the VIC engine taking large 1:1 blits,
# right-angle rotates and the scan-out push.  It works, but once /dev/g2d
# exists displayd/xserverd allocate contiguous canvases, which the kernel
# maps uncached, and the compositor's CPU alpha blends on them get several
# times slower than on cached memory: a full recomposite of a maximized
# glass window measured 135 ms with g2dd+VIC against 43 ms without.  To try
# it, uncomment both lines; g2dd must start before displaymand.
#
# No touch (xtouch), wlan, btd, battery (powerd/adspas) or audio on Orin: the
# desktop is driven purely by the USB keyboard/mouse through xim_none/xmouse.
@/bin/bgrun /sbin/sudod

@/bin/ipcserv /sbin/sessiond

# Display bring-up (4K probe + DCE init + modeset) now runs in-process inside
# fbdisplayd's init, scanning out the kernel-reserved 4K FB (pd.fb) it renders
# into.  No separate dceprobe launch is needed; fbdisplayd's residency keeps the
# DCE channels/instance memory alive.
#@export VIC2D_ENABLE=1
#@/bin/ipcserv /drivers/orin.nano/g2dd /dev/g2d
@/bin/ipcserv /drivers/displaymand
@/bin/ipcserv /drivers/orin.nano/fbdisplayd /dev/disp0
@/bin/ipcserv /drivers/fontd
# The desktop owns the panel; the graphical console (consoled) is not started.
# The serial rescue session from rescue_shell stays available on /dev/tty0.

@/bin/ipcserv /drivers/xserverd          /dev/x
@/bin/bgrun /sbin/x/xmouse  /dev/mouse0
@/bin/bgrun /sbin/x/xim_none /dev/keyb0
@/bin/bgrun /bin/x/xsession misa