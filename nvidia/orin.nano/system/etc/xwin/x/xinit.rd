# Orin Nano desktop session (run by xsession misa as the misa user).
# misawm window manager + statusbar + launcher.  No mp3player/start sound:
# Orin has no audio driver.  CPU-rendered over fbdisplayd (no GPU/g2dd).
@export XTHEME=misawm
@/bin/ipcserv /sbin/x/xwm_misa

@/bin/x/statusbar &
@/bin/x/xlauncher &