# Q8B WLAN 复位与 USB 蓝牙固件加载

调试日期：2026-11-05 至 2026-11-06。通过 `skills/q8b-kernel-test/SKILL.md` 的 EUD/PXE 与常驻串口缓存验证；本轮接续 `usb-pcie-sync-debug.md` 的 ext3 并发读修复。

## WLAN 复位顺序

原板上二进制含未合入的 PCIe DMA quiesce 实验：停止 WLAN 时关闭 PCI Command 的 Memory Space/Bus Master 与 MSI，下一次启动却先访问 BAR 执行软件复位，再恢复 PCI Command。此时复位写入不能依赖，重试停在 BHI，出现 EE=2、MHI=00000101、BHI=0、error=cafecace。

`wcn_mhi.c` 在首次复位 BAR 访问前调用 pcied 的 `prepare_wlan_dma`，恢复内存译码、DMA 与现有轮询 MSI sink。SOC reset 后仍保留原有恢复步骤。PCIe 配置访问继续由 pcied 独占。

同时恢复 Linux ath11k `ath11k_pci_clear_dbg_registers()` 的寄存器访问与等待顺序：读取 Q6 cookie/WARM_SW_ENTRY、等待、清零 warm entry、等待并回读，最后读取 WLAON_SOC_RESET_CAUSE。最后一个寄存器有 read-to-clear 副作用，不能随调试输出一起删除。保留复位结果和失败阶段诊断（包括 BHIE status）。本机读到 reset cause=0，因此不能把此次恢复单独归因于该 read-to-clear 操作。

本轮正式 pcied 从仓库源码重建，不保留板上旧 payload 的 quiesce 实验。没有根据 EC=0 的 FAR 推断 DMA 地址冲突。

## Wi-Fi 就绪后暴露的 USB 死锁

WLAN 获得 MAC 后，btd 才请求 `USBHCI_CMD_SETUP`。旧 USB host 在 IPC handler 内同步执行 `qca_setup()`，其固件位于这个 USB host 自己服务的 USB 根文件系统。固件 open/read 经 sdfsd 回调同一个 USB host 的 MSC handler，但前一个 IPC handler 尚未返回，形成自调用等待，启动卡住。

`bsp_usbbt.c` 改为在 SETUP handler 中设置 `setup_pending` 并立即返回未就绪。USB host 主循环的 `bsp_usb_bt_poll()` 消费请求并加载固件；文件系统等待期间 MSC IPC 可以得到处理。`busy` 防止重复初始化，已有 btd 重试接口保持兼容。成功后 SETUP 返回 0，失败后允许新的请求重试；reset/detach 清除待处理状态。

## 正常启动的健康检查误复位

去掉早期诊断 WLAN、由持久根在 GPU 后启动正式 WLAN 时，BHI/BHIE 已成功，但控制器反复重试。日志显示 QMI cap 在 tick 145582 到达，板级固件读入/上传后至 tick 207961 才继续。此前固定的 60 秒健康检查在 tick 207940 安排复位；下一轮也在 WMI READY 到达前安排了复位，最终第三次才稳定就绪。

`wcn_qmi.c` 记录成功发出请求和接受握手响应/指示的时间。慢文件读取完成后发出下一上传请求，即记录新的进展。`wlan.c` 的健康检查按最后进展后的 60 秒判断停滞，连接/重启状态中的 45 秒启动期限也随有效进展延后。没有取消停滞恢复，也没有单纯无限加长总启动等待。

## 主机回归

```sh
sh machines/q8b/test/run-usb-bt-setup.sh
sh machines/q8b/test/run-wlan-boot-progress.sh
```

测试包含真实 BSP 源码，模拟 USB 传输与 USB 根固件文件，断言 SETUP handler 不执行硬件/文件 IO；固件读取回调期间重新进入 USBHCI INFO，确认未就绪状态。覆盖请求合并、固件加载/HCI reset、文件打开失败后重试、reset 取消请求，以及已就绪时重复 SETUP。ASan/UBSan 通过；把 `USB_BT_SOURCE` 指向修复前源码时，在 `!in_handler` 断言确定性失败。

`wlan-boot-progress-test.c` 包含真实 WLAN lifecycle 源码，重放 65 秒 USB 读取后继续 QMI 的时序，验证空闲启动和显式连接启动都不会误复位，并确认停滞 60/45 秒仍触发恢复。ASan/UBSan 通过；修复前源码在首次“不应重启”断言失败。

## 真机记录

诊断启动中已完成 BHI → BHIE → AMSS/IPCR → QMI → WMI READY，DP `active=1 failed=0`。扫描返回真实热点；stop/start 后重新达到 WMI READY，23:53 的再次扫描也返回热点。23:47:39 出现 `q8b bt: firmware operational, USB HCI ready`，随后可 root 登录，ps 显示 GPU、SSH 与桌面进程存活。

当前固件未提供有效的独立 MAC，驱动沿用既有确定性硬件身份派生地址；本轮没有变更该策略。

00:08 的正式驱动已与用户指定的 home 完成 WPA2-PSK/CCMP 关联，station 为 connected、network_ready=1，WPA 无 MIC/replay 错误。netd 的 `ip` 查询确认 DHCP 地址 192.168.5.74/24、网关 192.168.5.1；00:10 的网关 ICMP 回复已验证收发。`wl0 state` 的 ip/ip_ready 目前沿用 pi5 的空字段，IP 由 netd 持有，不能据该空字段认定 DHCP 失败。凭据不写入本报告或提交。

主机到该 Wi-Fi 地址的 SSH 在 5 秒内超时；本轮没有把它当作板子挂死的证据，也不以此否定已经验证的网关通信。

## 部署与回退

USB 根上的原驱动已保存为 `/drivers/q8b/wlan.before-20261105`。临时安装程序在 RAMFS 阶段将候选二进制读入内存，等待 USB 根出现、DP 就绪和真实扫描结果后，备份旧文件，再写入正式 WLAN，fsync 后逐字节回读比较。00:15:33 安装输出为：

```text
wifi-install: PASS bytes=362824 md5=cdf67c39e1d83b8089d1156fb79c4f24 backup=/drivers/q8b/wlan.before-20261105
```

两个 TFTP 目录恢复相同的正常 initramfs：原始 init.ramfs.rd，修复后的 sdfsd/usbhostd，以及从仓库源码重建的 pcied。不包含早期 WLAN、探针或安装程序；WLAN 从持久 USB 根按原正常顺序启动。标准系统构建目录里的 wlan/pcied/usbhostd 也已同步为本轮产物。

```text
KERNEL.BIN     e23a07d47e9dbec378433b917647976c69ca2b4f317e6db40f66851147c09958
INITRAMFS.IMG  6748a2a2bd8e307bb79f133bc9cf03ddf271a11d6382bec9e58f1715b8872a2e
```

串口证据与测试前源码放在工作区 `.tmp/q8b-wifi/`，包括 `early-bt-deferred.log`、`normal-before-progress.log` 与 `install-progress.log`。保存的连接测试日志已将输入凭据替换为 `[redacted]`。

## 最终正常冷启动复验

00:17 EUD/PXE 冷启动使用上述正常镜像，从 USB 根启动持久 WLAN。00:21 回读日志确认仅一次 `start begin`：QMI cap tick 105180，板级上传完成 tick 167576，WMI READY tick 169062，自动连接 home 在 tick 174053 成功，`link ready (attempts=0 recovered=1)`。这次再次覆盖了超过 60 秒的 QMI 板级固件阶段，没有发生健康检查误复位。

00:20:42 的 `wl0 state` 为 connected、ok=true、init_failed=false；00:20:56 的 `net0 ip` 确认自动 DHCP 再次取得 192.168.5.74/24、网关 192.168.5.1；00:21:08 收到网关 ICMP 回复。00:20:13 蓝牙 firmware operational/USB HCI ready，00:21:02 的 ps 确认 xserverd、xlauncher、xwm_misa、statusbar、btd、netd、sshd 正常存活。

本轮缓存未出现同步异常、core dump、SError 或 xHCI HSE/HCE。最终日志为 `.tmp/q8b-wifi/normal-progress.log`。没有运行长时间无线吞吐或 USB 写入压力测试。
