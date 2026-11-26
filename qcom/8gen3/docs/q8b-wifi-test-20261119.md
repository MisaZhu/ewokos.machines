# Q8B 更换 Wi-Fi 模组后的实测（2026-11-19）

网络为用户更新后的混合 SSID `M`，使用现有 Q8B PXE 与 self-root Wi-Fi RAMFS。原装 SD 系统能识别新模组由用户确认；本报告中的数据测试全部来自 EwokOS。密码未写入报告。

## 加载与硬件

- PCIe4 成功建链，端点 `17cb:1103`，子系统 `17cb:0108`，`snapshot ethernet_pf=0 wifi=1`。
- MHI、QMI、WMI、DP 成功就绪，使用 WCN6855 `hw2.1/nfa765` 固件及 chip-id 18 / board-id 255 的 BDF。
- MAC `fe:7f:16:4d:0a:62`；DHCP `10.91.8.159/23`，网关 `10.91.8.1`。
- 测试主机 `10.91.9.151`，通过现有无线网络与板子传输。吞吐是本次主机/AP/板子的端到端应用吞吐。
- 11:31 的首次观察中 PCIe 初始化第一次返回 -4，内置重试随后成功；11:40、11:50、11:59、12:14 的后续 PXE 启动均枚举成功。旧模组损坏及替换后原装系统可识别由用户确认，不能把硬件替换后的恢复归因于软件修复。

## 测试方法

每个频段在启动时通过 `--band` 选择；以实际 `mhz`、BSSID、channel 核实频段。每轮连续 ping 600 次、间隔 0.5 秒，约 5 分钟；同时进行 3 次 TCP 连接，每次上传和下载各 8 MiB，接收方逐字节校验固定测试图样。每 30 秒采集 station/WPA/重试计数，并记录测试前后的 RX/TX 环和驱动日志。

`recovered=1` 是首次成功关联计数，不能解读为测试期发生一次恢复；本次判定依据是窗口内该值与 attempts 是否变化，以及真实 ICMP/TCP 结果。

## 2.4 GHz

- 11:51:37 至 11:56:37，BSSID `44:df:65:e1:4c:c3`，2412 MHz / channel 1，HT20，NSS=2。
- 600 发 / 600 收，丢包 0%；RTT min/avg/max/stddev = 3.410/10.804/98.660/9.659 ms。
- TCP 双向各 24 MiB，三轮均校验通过；汇总上传 10.22 Mbit/s，下载 21.27 Mbit/s。
- station 始终 connected，attempts=0、recovered=1 未变化；未记录到固件异常、握手错误、MIC 错误或重放错误。

## 5 GHz

- 12:00:15 至 12:05:15，BSSID `44:df:65:e1:4c:c2`，5200 MHz / channel 40，HE160，NSS=2。
- 600 发 / 599 收，丢包 1 包（0.167%，ping 输出按一位小数显示 0.2%）；RTT min/avg/max/stddev = 2.917/9.629/137.908/11.333 ms。
- TCP 双向各 24 MiB，三轮均校验通过；汇总上传 50.83 Mbit/s，下载 51.61 Mbit/s。
- station 始终 connected，attempts=0、recovered=1 未变化；未记录到固件异常、握手错误、MIC 错误或重放错误。
- 单次 ICMP 超时后立即恢复，尚未定位在主机/AP/开发板路径中的具体丢包点，不能据此归因于驱动或宣称零丢包。

## 软件重连对照

首次自动连接后通过软件重启控制器，两个频段均曾出现认证/关联成功但 EAPOL M1 接收计数为 0，最终 handshake-timeout。重新 PXE 启动并直接选择相应频段可首次连接成功。板子侧这些计数不能证明 AP 在空口未发送 M1。

分别尝试过“控制器复位前发送 deauthentication”和“每次启动显式配置两个 MAC 的 RX filter”。某轮切换中可自动恢复并完成 ping，但后续 5→2.4 GHz 切换仍多次握手超时，至 12:22:10 才观察到 connected，累计 attempts=12、recovered=5。因此未把它们认定为修复，最终代码及部署镜像已撤回这两项候选改动；实验补丁保存为 `reconnect-experiment-withdrawn.patch`。

候选版本另做过 5 GHz 一分钟回归，120/120 个 ping 返回、三轮双向数据校验通过，但首次 8 MiB 上传耗时 35.55 秒，后两轮约 0.7 秒。该结果属于被撤回的实验版本，不代表最终镜像性能验证。最终 2.4 GHz 回归计划在等待重连时停止，没有生成对应的流量测试结果。

最终保留 `band [auto|2.4|5]`、`--band` 和打包参数 `WIFI_BAND`，支持混合 SSID 下按频段测试。`state.band` 是请求过滤值，实际频段仍以 `station.mhz` / BSSID 确认。动态改变 band 会重启控制器，存在上述重连限制；本次两个五分钟窗口均从 PXE 启动时指定频段，窗口内未重连。

## 最终交付镜像

最终配置使用混合 SSID `M`，`--band auto`；WLAN 驱动定向重编译、同步到 EFI 打包输入后重新打包。`e2fsck -fn` 通过。镜像保留串口控制台及本次诊断用 SSH/数据校验工具，`rootfs=self`，不挂载或改写原装 SD 系统。

- 构建产物：`.tmp/q8b-wifi-20261119/final-auto.img`。
- PXE 服务目录经 `pxe_status` 确认：`machines/qcom/8gen3/efi`，bootfile `q8b/BOOTAA64.EFI`，`q8b -> .`。
- 已同步 `efi/initramfs.img` 与 `efi/initramfs-wifi.img`。
- 最终 initramfs SHA-256：`119c81c1fb7c691cd6a354aa79e2c5aa77ff4fffce8d4e8fae7bf80791ab61ad`。
- 本次五分钟测试的 2.4 GHz 镜像 SHA-256：`e9761d89549f4109b955b1be7cae345f2c2c71436e1a052e044ba3494eead4ff`；5 GHz：`2303477090c14a91957a813cc7f5d0ced8acaa7a2c03f42c3e34547cce91dfdc`。

最终镜像于 12:24 从 `boot source=net` 启动，加载 754024 字节 kernel 和 25165824 字节 initramfs；PCIe4 建链并枚举 `17cb:1103/17cb:0108`。12:25:43 确认自动选中 5200 MHz、HE160，`network_ready=1`、`attempts=0 recovered=1`。

最终自动选频镜像于 12:26–12:27 做一分钟收发确认：120 发 / 120 收，丢包 0%，RTT min/avg/max/stddev = 2.934/10.340/101.045/13.667 ms，station 持续 connected，attempts=0 / recovered=1 未变化。三轮双向各 8 MiB 校验均通过。三轮上传耗时分别为 0.660 / 19.506 / 15.123 秒，下载为 0.754 / 2.012 / 1.241 秒。因此，撤回重连实验后仍能观察到明显的上传速度波动；原因尚未定位，不能把最初五分钟窗口的平均吞吐作为稳定性能保证。驱动日志还出现部分 data TX completion `reason=2 success=0 tx_count=21`，虽未导致本次 ICMP 丢包或数据校验失败，仍需进一步定位。

12:27:30 在板上运行 `/bin/md5 /drivers/8gen3/wlan` 得到 `4b9f4f56c91a30a3c9512878af6f9876`，与最终打包输入及从镜像提取的驱动一致。

## 证据

主机工作区 `.tmp/q8b-wifi-20261119/` 保存 `2g/5g-ping.log`、`2g/5g-traffic.log`、`2g/5g-serial.log`、`2g/5g-run.log`、`boot-and-diagnosis.log`、测试程序源码与镜像 SHA-256。最终镜像另有 `final-auto-boot.log`、`auto-5g-ping.log`、`auto-5g-traffic.log`、`auto-5g-serial.log`、`auto-5g-run.log`、`final-checksum-and-state.log`；被撤回实验保留 `deauth-reconnect.log`、`dual-filter-cycles.log`、`last-experimental-2g.log` 及补丁。测试结果仅覆盖本次近距离、单 AP、各约 5 分钟窗口，未覆盖数小时持续运行、多 AP 漫游或可靠的软件重连。
