# TW220 原装 Windows WLAN 载荷提取与对照

2026-11-19，从 OrbStack `debian` VM 中的 TW220 原系统盘提取。核对 `/dev/sda4` 的 NTFS UUID 为 `EA74EACC74EA9A99`，以 `ntfs-3g -o ro` 挂载至 `/mnt/win`。没有写入原装 Windows 分区。

## 对应设备与安装配置

`Windows/System32/DriverStore/FileRepository/qcwlan8280.inf_arm64_aa7d04e9c1c6b8b5` 的 INF 匹配 `PCI\VEN_17CB&DEV_1103&SUBSYS_010817CB`，驱动版本 `09/21/2023,1.0.3553.9400`。

只读解析 SYSTEM hive 的 Select/Current=1 后，找到 `ControlSet001\Enum\PCI\VEN_17CB&DEV_1103&SUBSYS_010817CB&REV_01`，其 Driver 指向网络类 `{4d36e972-e325-11ce-bfc1-08002be10318}\0001`。该实例的 `MatchingDeviceId`、DriverVersion 与上述 INF 一致，服务为 `qcwlan`，配置 `FWFileName=wlanfw.mbn`、`BDFileName=bdwlan.elf`、`smpsDynamic=0`。

这是注册表配置的基础文件名，不等同于取得 Windows 运行时固件加载日志；驱动可能按芯片版本或 board ID 继续选择带后缀的文件。为保留对照依据，提取了包内全部 BDF 变体、hw2.0 镜像、M3、regdb、驱动 SYS/INF/CAT 和日志字典 `Data.msc` / `Data20.msc`，另包含 WLAN 热管理与休眠管理包，共 38 个文件、16,412,038 字节。

## 文件摘要

| 文件 | 字节 | SHA-256 |
|---|---:|---|
| wlanfw.mbn | 4,835,104 | `37c739de0fdcca5b0791df99dbad645a89711190d2313f874614142ea5ad632e` |
| wlanfw20.mbn | 4,849,664 | `5461bbbe9d43c16b97ea6321ab995c3bad5437516bcd5f2d8095d51d8b383b6a` |
| m3.bin / m320.bin | 各 266,684 | `431b8877d17323e9ff5040fc1cb823beb4f02878dbaa642a12db56d38869cfaa` |
| bdwlan.elf | 60,004 | `16d75436bcde8a08642cc1fa4f22e43cd46a7489d43c993fa933d33a725eb16b` |
| regdb.bin | 24,278 | `f69f33d7f2477a0ed66d1d5a4e7e02fa4241095cd97b5a2bcbb0fc7e473412f7` |

`System32/drivers` 中以上文件、Data 字典及 `qcwlan8280.sys` 均与 DriverStore 提取版本逐文件 SHA-256 一致。完整清单和 WLAN 范围注册表记录在工作区 `.tmp/q8b-dualboot-20261119/windows-original/{manifest.json,installed-manifest.json,wlan-registry.json}`；未导出整份 SYSTEM hive或无线网络密钥。

三个系统目前涉及不同的 AMSS 版本：

| 来源 | 版本 |
|---|---|
| 原装 Windows | `WLAN.HSP.1.1.c5-00135-QCAHSPSWPL_V1_V2_SILICONZ_WOS-1` |
| 原生 Linux 7.2.12 | `WLAN.HSP.1.1-03125-QCAHSPSWPL_V1_V2_SILICONZ_LITE-3.6510.41` |
| 此前 EwokOS | `WLAN.HSP.1.1-04866.5-QCAHSPSWPL_V1_V2_SILICONZ_IOE-1` |

Windows M3 的版本字符串同为 `WLAN_UCODE_HASTINGSPRIME_4.0.4-00000`，但摘要与另外两套载荷不同，不能仅凭该字符串混用。

## 校准与测试准备

本机实测 QMI `chip=0x12`（十进制 18）、board=255、SoC=0x400c1211。原生 Linux 的设备树属性 `qcom,calibration-variant` 为 `TW220`；此前 EwokOS 日志选择不带 variant 的通用 BDF，60,036 字节。仓库还存在 `NTM_TW220` / `LE_X13S` 的 60,008 字节 BDF。这些文件均未与提取的 Windows BDF 完全一致，不能认为此前校准已等同原装。

Windows `bdwlan.elf` 的 ELF PT_LOAD 校准区为 58,180 字节，SHA-256 `12686285c6e37f0541a68968a5b78a919ae5f8bb50c02388e3833053cee58152`。它与仓库所含 BDF 数据区也有差异；完整 ELF 文件长度不同并非唯一差异。比较清单在 `bdf-comparison.json` 与 `bdf-payload-comparison.json`。

准备成套载荷对照包 `.tmp/q8b-dualboot-20261119/windows-fw-payload.tar.gz`：将未修改的 `wlanfw.mbn` 和 `m3.bin` 放至设备实际使用的 nfa765 路径；把未修改的 `bdwlan.elf` 和 `regdb.bin` 包入驱动现有读取格式的 `board-2.bin`。新容器只含本机确切 PCI/QMI 身份的 BDF 条目及 `bus=pci` regdb 条目，解析回读核对原文件摘要一致。该容器仅用于此设备的受控实验，没有改写仓库的分发固件或默认驱动选择策略。

主动探测工具在启动 45 秒后记录 station、netd IP 配置，并分别连接网关 `10.91.8.1:22` 与测试主机 `10.91.9.151:50004`，结果逐步关闭文件落盘至 `/tmp/wifi-active-probe.log`。配合原有独立日志进程，可区分管理入口失效与设备主动访问也失败。工具编译链接通过，尚需真机验证。

兼容性与稳定性结论应以实际加载、WMI/HTT 初始化、连接与双向收发结果判断；原装 Windows 使用这套文件不自动证明 EwokOS 的宿主协议实现可直接使用，也不构成固件崩溃已经修复的证据。

## 首轮结果及回退

15:12 的部署逐项摘要校验通过，备份为设备 `/var/backups/wifi-before-windows-fw-20261119-151222/`。约 15:13 正常重启，用户观察到无法联网；随后切回 Linux 读取磁盘日志。

本轮反复停在 `BHIE`，`ee=1`、MHI=0x201，约 20 秒下载等待后超时；QMI、BDF、M3、WMI、HTT 均未进入，station=idle。约 199 秒期间已尝试六次启动，仍无有效 MAC。`netd` 因等待 MAC 超时反复重新打开接口，探测时只有 link-local 与 loopback，网关为 0。两个主动 TCP 连接均立即失败，不能用于评价 AP 或空口质量。没有新 SYSERR 或新 RDDM；取回的 RDDM SHA-256 仍为此前 12:48 的文件。

这轮说明整套原装载荷尚不能经当前 EwokOS 启动路径进入运行态，不能继续对其进行稳定性验收，也不能仅据超时认定签名、镜像布局、芯片匹配或某一传输步骤是根因。由于失败发生在 QMI 之前，本轮也没有检验 Windows BDF、regdb 或 M3 的实际加载兼容性。

15:21 已核对备份摘要并恢复此前 `04866.5 ... IOE-1` 的 AMSS/M3 和原 `board-2.bin`，保留主动探测工具和诊断驱动。Windows 失败记录另保存在设备 `/var/backups/wifi-windows-fw-result-20261119-152119/` 及主机 `.tmp/q8b-dualboot-20261119/diag-windows-fw/`，回退日志为 `restore-ioe.log`。随后重启，对可运行固件继续检查 ARP/IP 可达性；修复和双频稳定性目标尚未完成。
