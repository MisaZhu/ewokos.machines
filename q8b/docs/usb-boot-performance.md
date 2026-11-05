# Q8B 全 USB 启动读取性能

2026-11-06，SanDisk `0781:5591`，xHCI2/root port 10，USB 3。
ESP 为 LBA 2048..526335，ext3 根分区从 LBA 526336 开始。

## 定位

基线是 UEFI、内核、initramfs、持久根文件系统均从 USB 启动，8 核、
1024Hz 调度时钟、3840×2160 GOP；没有使用 PXE 代替最终启动路径。

- 00:50:13.941：UEFI `boot source=usb`。
- 00:50:14.326：16MiB initramfs 加载完成；SFS 读取约 330ms。
- 00:50:18.252：USB ext3 根文件系统挂载、switchroot。
- 00:51:45.859：串口 login 提示；距根挂载 87.607 秒。
- WLAN 内部时钟：AMSS 文件读取 19,581ms，后续 QMI 阶段约 59,201ms。

`sdfsd` 的 USB 回调把所有 ext3 读取拆成 512 字节。即使 ext3 已将连续
文件块合并，`usb_ext_read_blocks()` 仍逐块、逐扇区发 IPC。每扇区都要付出
IPC 以及 BOT 的 CBW、DATA、CSW 三段传输开销。USB transport 和 xHCI 已支持
一次 8 扇区（4096 字节），原根文件系统回调没有使用这项能力。

只读实机对照：读取相同 1MiB 原始范围（LBA 526336，2048 扇区），
单扇区 2048 次请求耗时 1732ms，8 扇区 256 次请求耗时 97ms。
两份内容逐字节相同，MD5 为 `c0eb12586aab1169db509efd4e5e3a94`。
该结果是原始传输测量，不能直接当作完整启动加速倍数。

## 修复与兼容性

`system/basic/sys/rootfs/usb_ext_read.h` 将块区间转换为连续扇区区间，
每次最多读取 `USBMSC_MAX_SECTORS`。单块读取也使用同一路径。
读取失败后重试原位置，退回单扇区模式；后续请求继续使用兼容模式。
模式切换使用原子读写，以支持多个并发文件系统读取 worker。
转换前检查整个区间，防止块号、数量及分区偏移相加溢出。

保持现有写入、journal flush、ext3 私有间接块快照及 USB Bluetooth
延后固件加载逻辑。没有通过串行化所有文件读取来换取稳定性。

`espwrite -u /dev/usb0` 使用现有 usbhostd 更新 FAT32 ESP，不创建另一个
xHCI owner。FAT 回调限制在 ESP 范围，写入仍是单扇区，put 后重新挂载并
逐字节比较。更新前提取原始 initramfs 到持久根目录作为恢复备份。

## 回归方法

主机测试：

```sh
sh system/basic/sys/rootfs/tests/run_usb_read.sh
sh system/basic/libs/ext3/tests/run_read_reentrancy.sh
sh machines/q8b/test/run-usb-bt-setup.sh
sh machines/q8b/test/run-wlan-boot-progress.sh
```

上述测试通过 ASan/UBSan。USB 测试覆盖 1K/2K/4K 块、请求数量预算、
缓冲区边界、批量失败回退、真实读取错误和 LBA/数量溢出。
打包后的 initramfs 通过 `e2fsck -fn`。

只读实机工具源码为 `machines/q8b/test/usb-read-bench.c`：

```text
usb-read-bench raw /dev/usb0 526336 2048
usb-read-bench file /etc/firmware/ath11k/WCN6855/hw2.1/nfa765/amss.bin
```

完整文件系统读取 AMSS 的修复前基线为 5,079,040 字节 / 9489ms，MD5
`e38e434e815db0a1f041bc72212d9c94`。启动期间有其他读取负载，不能把
空闲时文件读取时间与并行启动阶段的内部时间混用。

## 部署后结果

USB ESP 原镜像已完整提取到 `/home/root/initramfs.usb-before.img`，其 MD5
`07cf778db5255e3bd0594e2eca721bbb` 与主机备份一致。候选镜像上传后 MD5
`4d7010c3d347e4d98c4ab84bcd444465` 与主机一致；`espwrite put` 完成
全文件写入和独立读回逐字节比较。原、新镜像全部程序文件中只有
`sbin/sdfsd` 改变。持久 `/sbin/sdfsd` 也同步，MD5 为
`54b813321834cc951c801bd28a4e90fe`。

01:10:20 请求 EUD 复位，01:11:00.518 UEFI 再次确认 `boot source=usb`，
01:11:04.708 选择 USB 根，01:11:16.742 出现 login 提示。
同一硬件、显示分辨率和启动配置下的前后对照：

| 测量范围 | 修复前 | 修复后 |
| --- | ---: | ---: |
| USB 根挂载到 login 提示 | 87.607s | 12.034s |
| 空闲时完整文件系统读取 5MB AMSS | 9489ms | 1045ms |
| 启动中 AMSS 文件读取（WLAN 内部计时） | 19581ms | 2721ms |
| QMI cap 到校准阶段结束（WLAN 内部计时） | 59201ms | 7838ms |

这是一次同配置重启对照，不是启动时间分布测试。没有观察到传输错误、
单扇区回退或同步异常。

验证结果：

- WLAN 从持久配置自动连接 `home`，DHCP `192.168.5.74/24`，网关
  `192.168.5.1`；SSH 可用。
- 01:11:26.913 蓝牙报告 `firmware operational, USB HCI ready`。
- `g2dd`、`xserverd`、`xwm_misa`、statusbar、xlauncher 存活。
- 四个 worker，各四轮读取 xserverd 和 AMSS，共 16 次校验全部通过。
  xserverd MD5 `cf320dc39d1ae7ed81c553f0df20ad1a`，AMSS MD5 与基线一致。

恢复原镜像时，使用已部署的 USB 工具：

```text
/home/root/espwrite -u /dev/usb0 put /home/root/initramfs.usb-before.img initramfs.img
```

需等待 `verified initramfs.img` 后再重启。持久 sdfsd 的原副本在
`/home/root/sdfsd.usb-before`。正常测试结束保留加速版本和全 USB 启动顺序。

本地原始证据、镜像和测量记录保存在 `.tmp/q8b-usb-boot/`。

## 锁竞争与嵌套诊断（2026-11-06）

在最终全 USB 镜像上读取 5,079,040 字节 AMSS 时，诊断计数为：

- 文件系统共享读锁等待累计 52 微秒；写锁等待 1 微秒。单次最大读锁等待约 69.9 毫秒，来自启动期一次写者进入，未形成持续竞争。
- USB MSC 命令 1,791 个、0 错误，最大命令嵌套深度为 1。USB 控制器保持单 owner，未发现递归 USB 命令或死锁。
- USB 服务累计 835,827 微秒，xHCI 完成等待 820,028 微秒；短忙轮询后再休眠使等待休眠次数从 9,613 降到 5,854。
- AMSS 读取从 1,080ms 降到 910ms；MD5 仍为 `e38e434e815db0a1f041bc72212d9c94`。

结论是瓶颈在每个 BOT CBW、数据阶段、CSW 的 xHCI 完成等待，而不是 `_fs_lock` 竞争。保留共享读锁是必要的，因为蓝牙固件读取会嵌套回到 USB MSC；修复改为每次读取保存三级间接块快照，避免双重/三重间接文件为每个数据块重复读索引块，并在 xHCI 等待中加入 80 微秒短忙轮询窗口。超时路径仍休眠并保留原有恢复逻辑。

原始 4MiB 对照在最终镜像上：单扇区 4,616ms，8 扇区 250ms，内容比较 PASS。Wi-Fi 仍通过 DHCP 获得 `192.168.5.74/24`、网关 `192.168.5.1`；蓝牙报告 `firmware operational, USB HCI ready`。
