# 串口控制台 (ttyd) 已关闭: 目标机无串口, 悬空 RX/TX 只会带来噪声输入与
# 回显洪水 (ttyd 高占用的根源)。需要串口台时取消下行注释。
#@/bin/ipcserv /drivers/x86/ttyd /dev/tty0

@/bin/ipcserv /drivers/timerd
@/bin/ipcserv /drivers/piped  /dev/pipe0
@/bin/ipcserv /drivers/ramfsd /tmp
@/bin/ipcserv /drivers/nulld /dev/null
@/bin/ipcserv /sbin/sessiond

# USB: usbhostd(UHCI) 枚举; MSC 盘由 bsp_usb 自动挂到 /mnt/udisk0
@/bin/ipcserv /drivers/x86/usbhostd    /dev/hid0
@/bin/ipcserv /drivers/x86/hid_keybd   /dev/keyb0  /dev/hid0
@/bin/ipcserv /drivers/x86/hid_moused  /dev/mouse0 /dev/hid0

# VGA 控制台: 用户态接管 (console_handoff 之后), 系统输出经 /dev/vga0 可见;
# GOP 机器 (UEFI-only, 无文本区) 上 vgacond 写 GOP 帧缓冲 —— 唯一可见输出
@/bin/ipcserv /drivers/x86/vgacond /dev/vga0

# GOP 图形会话: 登录/shell 渲染到 vgacond (真机 UEFI 唯一可交互控制台)

# 存储驱动: 按硬件存在性自动挂载 (探测失败则自行退出, 不影响启动)
# 注: atafd 不在此启动 —— 根盘位于 AHCI 时 atafs 会与 sdfsd 双挂载同一
# HBA 端口导致死锁; SATA 数据盘测试用 scripts/periph_test.exp (pc+ahci0)
@/bin/bgrun /drivers/x86/nvmefsd /mnt/nvme

#@/bin/bgrun /bin/session -r -t /dev/tty0
@/bin/bgrun /bin/session -r -t /dev/vga0
