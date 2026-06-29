@export X_AUTO_FULL_SCREEN=yes

#@export XTHEME=openlook
#@/bin/ipcserv /sbin/x/xwm_openlook

#@export XTHEME=mac1984
#@/bin/ipcserv /sbin/x/xwm_mac1984

@export XTHEME=opencde
@/bin/ipcserv /sbin/x/xwm_opencde

#@/bin/x/statusbar &
#@/bin/x/xlauncher &
@/apps/xapps/xapps -l --item_size=80 --font_size=16 &

@/bin/mp3player /usr/system/sounds/start.mp3 &