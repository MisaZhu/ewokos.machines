# Q8B USB-C host 启动

Q8B 的 USB-A 与两个数据 USB-C 接口连接到三个独立的 DWC3/xHCI 控制器，
不能通过扫描一个控制器的所有 root ports 覆盖整板 USB。

| EwokOS 日志 | SoC 设备树节点 | 核心物理地址 | 接口 |
|---|---|---|---|
| xhci0 / usb2-mp | usb_2_dwc3 | 0x0a400000 | 多端口 USB-A host |
| xhci1 / usb0-type-c | usb_0_dwc3 | 0x0a600000 | 数据 USB-C connector@0 |
| xhci2 / usb1-type-c | usb_1_dwc3 | 0x0a800000 | 数据 USB-C connector@1 |

依据是 `~/work/q8b/sc8280xp.dtsi` 的三个独立 DWC3 节点，以及
`sc8280xp-radxa-dragon-q8b.dts` 中 connector@0/1 的 HS 和 SS endpoint
连接；两个 Type-C DWC3 在板级设备树中均设置为 `dr_mode = "host"`。
connector 编号表示设备树连接关系，不用于判断机壳左右或上下位置。
单独的 Type-C 电源输入不属于这两个数据接口。

## 实现

`bsp_usb.c` 映射三个控制器，分别接管后将它们的 root ports 合并到同一个
`/dev/usb0` 服务。设备、Hub、HID 和 MSC 的传输通过各自的 `xhci_hc_t`
路由；`/dev/usb0` 是 USB 服务节点，不表示 SoC 的 `usb_0_dwc3`。

`dwc3.c` 在两个 Type-C 控制器的 xHCI 初始化之前检查 Synopsys ID 和
`GCTL.PRTCAPDIR`。固件留下 host 模式时保留配置；device/OTG 模式时先停止
gadget DMA，再进行核心角色切换。清除 USB2/USB3 接口的 SUSPHY 后选择 host，
随后由 xHCI 完成 HCRST、环配置和端口上电。

此路径依赖 UEFI 完成电源域、GCC 时钟、PHY 校准、Type-C CC/VBUS 协商和
SuperSpeed 插头方向配置；没有实现 PMIC GLINK/UCSI 的 PD 管理、DP Alt Mode
重配置或 EUD 路径切换。因此，host 寄存器接管成功不等于实机 Type-C 数据链路
已经验证，EUD 是否占用某个 Type-C 仍需实机日志确认。

1 MB DMA 池中，每个控制器独占 128 KB 的环/上下文/scratchpad 区域，
设备使用池尾的 16 个 32 KB arena。HCRST 后只回收该控制器的区域和 arena，
避免重置一个控制器时清空其它控制器正在使用的 DMA 数据。

root-port 扁平编号在一次控制器集合初始化后保持固定；一个控制器运行期出错
不会导致另一个控制器的端口重新编号。完整 BSP reinit 才重新建立编号，
并由 usbhostd 清理枚举表重新扫描。

## 构建与验证

```sh
sh machines/q8b/system/libs/bsp/tests/run-typec.sh
make -C machines/q8b/system ramfs
```

寄存器/DMA 模拟测试执行真实的 DWC3、xHCI 和 BSP 代码，检查角色切换、
设备停止超时、三个控制器的端口路由、控制器缺失、HCRST 超时、跨控制器
DMA 隔离、scratchpad 对齐和运行期失败后的端口编号。测试通过不代表硬件验证。

启动镜像是 `machines/q8b/ramfs_aarch64.img`（16 MB ext3），将其作为
`RAMFS.IMG` 同步到当前启动存储。无需因此修改 bootloader 或 kernel。
镜像中的 `/drivers/q8b/usbhostd` 包含三个控制器及 Type-C handoff 实现。

新实机日志应先出现两个 Type-C 控制器各自的 `dwc3N: ... role=...`、
`host ready`、`xhciN ... phy=a600000/a800000` 和 root ports 范围，然后才是
U 盘枚举、MSC claim 和 `sdfsd -u /dev/usb0 /`。若控制器成功但端口没有 CCS，
下一步检查 Type-C 的供电、方向、PHY/firmware 状态及 EUD 占用，不能据此归因
于 ext3 或分区表。
