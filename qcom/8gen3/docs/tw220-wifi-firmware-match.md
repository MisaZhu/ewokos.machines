# TW220 WiFi 模组固件匹配确认与"无法连接"原因分析

- 整理日期：2026-11-06
- 范围：先按 `../q8b` 项目已收集资料确认 **TW220 板载 WiFi 模组固件与当前 ewokos 项目是否匹配**，再基于固件匹配结论分析"TW220 无法连接、Q8B 正常"的原因。
- 结论先行：**模组/固件层面匹配一致，不存在不匹配**；固件不是连接失败的根因，问题指向板级 bring-up / 链路阶段（详见 §5，需 TW220 实机日志定位）。

---

## 1. 结论摘要

| 检查项 | 结论 |
|---|---|
| TW220 模组身份 | 当前实机日志为 **WCN6855 hw2.0**：PCI `17cb:1103`，子系统 `17cb:0108`，QMI `chip-id=2 / board-id=255` |
| AMSS / M3 固件 | ✅ 一致。与 Q8B 完全相同：驱动按子系统 `17cb:0108` 选择 `ath11k/WCN6855/hw2.1/nfa765/`（`WLAN.HSP.1.1-04866.5-...IOE-1`），md5 `e38e434e...` 与实机部署一致 |
| 板卡文件 BDF（board-2.bin） | ✅ 齐备。当前固件树已含 TW220 专用条目 `variant=NTM_TW220`（block#117，60008 B，sha256 `77b1b9ef...`），数据与 ThinkPad X13s（`LE_X13S`）相同，**与上游 ath11k 提交附件逐字节一致** |
| 驱动板卡查找 | 当前驱动按 QMI `chip=2,board=255` 自动选择 `HW_GK3`（60008 B）；`NTM_TW220` 条目只存在于 chip18，不能把 chip18 校准数据套给 chip2。 |
| 固件匹配判定 | **一致**。TW220 模组所需 AMSS/M3/BDF 在当前项目固件树中全部具备，且与 Q8B 走完全相同的选择路径 |

---

## 2. TW220 模组身份：证据链

来源全部来自 `/Users/tony/work/q8b` 已收集资料 + 上游公开提交：

1. **设备树对比资料**（`/Users/tony/work/q8b/TW220与Q8B设备树对比.md` §2/§3.3）：
   - TW220：板载 **WCN6855**，PCIe `pcie4`（`pci17cb,1103`，`calibration-variant "TW220"`，`max-link-speed 2`）+ `uart2` BT；`wcn6855-pmu` 电源树两板同构。
2. **社区 DTS**（`/Users/tony/work/q8b/robo-kala-ntmer-tw220/devicetree/003-tw220-add-devicetree.patch`）：
   - `&pcie4_port0 { wifi@0 { compatible = "pci17cb,1103"; ... qcom,calibration-variant = "TW220"; } }`；`perst` tlmm141 / `wake` tlmm139；`vreg_wlan = VCC_WLAN_3R9`（pmr735a GPIO1，3.9 V）。
3. **板卡文件补丁**（`/Users/tony/work/q8b/robo-kala-ntmer-tw220/wifi/add-tw220-board-file.patch`）：
   - TW220 缺失的板名 = `bus=pci,vendor=17cb,device=1103,subsystem-vendor=17cb,subsystem-device=0108,qmi-chip-id=18,qmi-board-id=255,variant=TW220`，映射到 `...LE_X13S.bin`（ThinkPad X13s 板卡数据）。
4. **上游 ath11k 邮件列表**（2025-07-06，Pengyu Luo，“ath11k: WCN6855 hw2.1: board file for Ntmer TW220”，<https://lore.kernel.org/ath11k/CAH2e8h7BHvRQNo9gK+LPV4EB6WOtm-ahw_c0V00waBs9j99W9g@mail.gmail.com/>）：
   - 明确 **WCN6855 hw2.1**；提交 ids 为 `...subsystem-device=0108,qmi-chip-id=18,qmi-board-id=255,variant=NTM_TW220`；
   - 数据 **复用 LE_X13S**（"reusing the one originally used for LE_X13S, it works well with good RF performance"）；
   - 附件 sha256 = `77b1b9ef636ce8492723aa7c2ff2c42c2e5e81a43967d7f32bfdef93a31bf8c4`（60008 B）。
5. **主线 DTS 提交**（“arm64: dts: qcom: Introduce Ntmer TW220”，<https://lore.kernel.org/all/20250617092929.1492750-1-mitltlatltl@gmail.com/>）：
   - 支持列表含 "Bluetooth & Wi-Fi (board file regeneration required)"；维护者 Konrad 要求补丁作者把 variant 提交到 ath11k 列表（即证据 4）。
6. **Q8B 实测对照**（`/Users/tony/work/ewokos/.tmp/q8b-wifi/normal-progress.log`、`/Users/tony/work/ewokos/reports/QCNFA765-WiFi-performance-2026-10-31.txt`）：
   - Q8B 的 QCNFA765 实机 QMI 值为 `chip=12 board=ff`（日志使用十六进制，0x12=18、0xff=255）；当前 TW220 日志为 `chip=2`，两者不能直接共用 chip-specific BDF。

> 结论：两板 PCI 子系统相同，但当前 QMI chip-id 不同，不能据此认定需要完全相同的 chip-specific 校准。

---

## 3. 当前项目（ewokos）固件侧核对

路径：`machines/q8b/firmware/ath11k/WCN6855/hw2.1/`

| 文件 | 内容 / 版本 | 对 TW220 的意义 |
|---|---|---|
| `amss.bin` | 通用镜像 `WLAN.HSP.1.1-03125-QCAHSPSWPL_V1_V2_SILICONZ_LITE-...` | 不作为本卡使用 |
| `nfa765/amss.bin` | 专用镜像 `WLAN.HSP.1.1-04866.5-QCAHSPSWPL_V1_V2_SILICONZ_IOE-1`，md5 `e38e434e815db0a1f041bc72212d9c94` | ✅ 与实机部署一致（`machines/q8b/docs/usb-pcie-sync-debug.md` 记录的设备端 md5）；TW220 子系统 `17cb:0108` 命中该目录 |
| `nfa765/m3.bin` | `WLAN_UCODE_HASTINGSPRIME_4.0.4-00000` | ✅ 同上 |
| `board-2.bin` | 解析出 126 块 / 176 个板名（见 §4） | ✅ 含 TW220 条目 |
| `regdb.bin` | – | ✅ |

驱动侧选择逻辑（`machines/q8b/system/drivers/wlan/`）：

- `wcn_mhi.c::wcn_firmware_path()`：子系统 == `17cb:0108`（QCOM 形）或 `105b:e130`（替代形）→ 加载 `hw2.1/nfa765/{amss,m3}.bin`。TW220 命中第一种，与 Q8B 相同。
- `wcn_board.c::wcn_board_data()`：按 PCI 子系统 + QMI chip/board 拼精确板名（**不带 variant**）在 board-2.bin 中查找。

---

## 4. board-2.bin 解析：QMI 自动选择 HW_GK3

用与 `wcn_board.c` 相同的容器解析逻辑读取当前固件树 `hw2.1/board-2.bin`：

| 条目（板名） | 数据块 | 大小 | sha256 | 说明 |
|---|---|---|---|---|
| `...subsystem-device=0108,qmi-chip-id=2,qmi-board-id=255`（无 variant） | block#6 | 60048 B | — | chip2 通用回退 BDF |
| `...subsystem-device=0108,qmi-chip-id=2,qmi-board-id=255,variant=HW_GK3` | — | 60008 B | `3ee912c4bf015d314198c7c50829deb611b97e2499c13debd736485e72c67fa0` | **TW220 HW_GK3 BDF**，由 QMI chip=2 自动选择 |
| `...subsystem-device=0108,qmi-chip-id=18,qmi-board-id=255`（无 variant） | block#5 | 60036 B | `9c5266d14cbe3eb4...` | **Q8B 通用回退 BDF**：同一份数据被 14cd/17aa/17cb 等 7 个 ODM 子系统下的同 chip/board 条目共享 |
| `...qmi-board-id=255,variant=NTM_TW220` | block#117 | 60008 B | `77b1b9ef636ce8492723aa7c2ff2c42c2e5e81a43967d7f32bfdef93a31bf8c4` | **TW220 专用 BDF**，== `LE_X13S`（block#105）数据，== 上游 ath11k 提交附件（逐字节一致） |

要点：

1. 当前项目固件树**已经包含** chip2 的 HW_GK3 板卡数据和 chip18 的 Q8B/TW220 条目。
2. `wcn_board.c` 现在在收到 QMI `chip=2,board=255` 时自动追加 `variant=HW_GK3`；Q8B 的 chip18 不会命中该条目。
3. `/etc/wlan.board` 仍可用于显式覆盖，但正常部署不再需要该文件。

---

## 5. 原因分析（固件一致 ⇒ 问题在板级 bring-up / 链路）

固件匹配确认后，"Q8B 能获取 IP / TW220 无法连接"的差异只能来自两块板在 **PCIe/供电时序、射频环境、或上层网络路径**上的不同。按 ewokos wlan 的启动阶段给出核对表（正常锚点参考 `.tmp/q8b-wifi/normal-progress.log`）：

| 阶段 | Q8B 正常日志锚点 | TW220 需要核对的点 |
|---|---|---|
| 0. PCIe4 枚举 | `pcieprobe`: `ethernet_pf=2 wifi=1`；bsp `PCIe4 radios enabled GPIO133/134` | pcie4 时钟/电源、`perst`(141)、`wake`(139)、radios(133/134)、**PMR735a GPIO1**（Q8B=3.3 V E-key；TW220=3.9 V `VCC_WLAN_3R9`）。bsp `pcie.c::ekey_3v3_set()` 已实现该 GPIO（SPMI），但上电时序（Q8B DTS `startup-delay-us=3000`）需与 TW220 板载模组核对 |
| 1. MHI BHI/BHIE + AMSS | `wcn: loading firmware` → `firmware loaded bytes=...` | 若停在 load-firmware / MHI 无响应 → PCIe 链路或模组未上电；检查 `devcmd /dev/wl0 state` 是否永远 idle |
| 2. QMI caps / BDF / M3 | `wcn: QMI cap chip=12 board=ff` → `bdf=1 m3=1` | 拿到 chip/board 后**逐值比对是否 18/255**；BDF 上传失败会打印 `missing BDF` |
| 3. WMI READY / DP | WMI READY、DP `active=1 failed=0` | 停在 WMI → 排查 CE 配置 / BDF 内容 |
| 4. 扫描 | `scan`/`list` 返回真实热点 | **扫描为空** → 射频路径：BDF 变体（§4）、regdb/国家码、天线、信道 |
| 5. 关联 / WPA | `wl0 state` connected、`ok=true` | 能看到热点但连不上 → WPA2 参数、密码/pmk、5 GHz 信道兼容 |
| 6. DHCP / IP | `net0 ip 192.168.5.x`，网关可达 | netd/DHCP 层（与 Q8B 同栈，通常不是差异点） |

基于现有资料的最可能原因排序（**需 TW220 实机日志确认**）：

1. **板级 PCIe4 枚举 / 模组上电时序**（TW220 板载模组 vs Q8B M.2 卡槽）：若 TW220 上 wlan 从未进入 QMI（状态恒为 idle、无 `QMI cap` 日志），问题在阶段 0/1 —— 供电轨（PMR735a GPIO1 时序）、perst 时序。
2. **射频环境/配置差异**：若扫描正常但关联失败，核对 WPA2 凭据与 5 GHz 信道/国家码（Q8B 用 CN/regdb，AP 在 5240 MHz-160 MHz）。
3. **BDF 变体（次要）**：当前驱动对两板加载相同通用 BDF；在 TW220 上如出现射频异常，改用 block#117（X13s）数据可排除此因素。

需要采集的证据（拿齐才能定论）：

- TW220 串口完整启动日志（wcn 的 MHI/QMI/WMI 各行）；
- `devcmd /dev/wl0 log`、`devcmd /dev/wl0 state`、`devcmd /dev/wl0 scan|list`；
- PCIe 探测输出（wifi 端点是否枚举、子系统值）。

---

## 6. 建议动作

1. **先定位阶段**：采集 TW220 的 wlan 启动全日志，与 `.tmp/q8b-wifi/normal-progress.log` 逐段对齐，确认停在第几阶段（0–6）。
2. **固件侧处理**：`wcn_board.c` 按 QMI 身份自动选择 BDF；当前 TW220 的 `chip=2, board=255` 自动命中 `HW_GK3`，Q8B 的 `chip=18` 继续使用自身条目。`/etc/wlan.board` 仅作为可选覆盖，避免 `missing BDF` 和跨芯片误用校准数据。
3. **核对上电时序**：TW220 为板载模组，确认 `VCC_WLAN_3R9`（PMR735a GPIO1）→ perst 释放的时序与 `startup-delay`，必要时在 bsp 加 TW220 专用序列。

---

## 7. 补充：TW220 实测症状"能扫描到 SSID 但连接失败"

（2026-11-06 补充。该症状说明：PCIe4 枚举、MHI/AMSS、QMI/BDF/M3、WMI/DP、RX 射频全部正常 —— 问题只可能在 **associate() 的准入检查** 或 **关联/4-way 握手** 阶段。）

### 7.1 驱动内失败点分类（据 `wcn_sta.c` / `wcn_wpa.c` / `wcn_wmi.c`）

**A 类：进入 WMI 前立即拒绝（`wcn_sta_associate()` 返回 -1，station 保持 idle，无错误串）**

| 检查 | 代码位置 | 触发条件 |
|---|---|---|
| `select_rsn()` | `wcn_sta.c:55-73` | **只接受纯 WPA2-PSK/CCMP**：单播/组播必须 CCMP(00-0f-ac:04)、AKM 必须 PSK(00-0f-ac:02)；RSN 任一含 **PMF 位（capability bit6=0x40 或 bit7）即整包拒绝**；WPA1/WEP/TKIP/WPA3-SAE/6GHz OWE 全部拒绝 |
| `wcn_wmi_find_bss()` | `wcn_wmi.c:602-616` | SSID 匹配的所有 BSS 的 `wcn_wmi_channel_power(mhz)<0`（受 regulatory 限制）→ 选不出 BSS |
| 信道/宽度功率 | `wcn_wmi.c:637-664,774` | `station_width_power()` 对 160 MHz 要求主信道+全部子信道在规则内且功率合法 |
| 配置文件 | `wcn_sta.c:89-99,101-109` | `/etc/wlan.width`、`/etc/wlan.phy` 存在但内容畸形 → 直接 -1 |

**B 类：关联/握手阶段失败（station 进入流程，`devcmd state` 会给出 error= 串 + status= 原因码）**

| error= | 含义 | 常见触发 |
|---|---|---|
| `authentication-rejected` / `association-rejected` / `AP-disconnected` | AP 回 deauth/disassoc/拒绝，status 为 802.11 原因码 | AP 强制 PMF(802.11w)、MAC 白名单、并发/带宽策略 |
| `handshake-timeout` / `message3-MIC` / `group-key...` | EAPOL 4-way 失败（20 s 超时） | 密码不符；或 EAPOL 帧 RF 丢包严重（TX 功率/校准差） |
| `WPA-initialization` / `PTK-derivation` | 本地计算失败 | 极少见 |

### 7.2 与"Q8B 正常"对照的最可能原因（按概率）

1. **AP 安全模式不同（最高概率之一）**
   - 驱动**拒绝任何带 PMF 的 RSN**（`wcn_sta.c:68` 的 `&64` 检查）。如今多数新款路由器默认启用 WPA3/PMF，或同 SSID 双频里只有一个 BSS 是纯 WPA2。
   - 若 TW220 测试环境中该 SSID 由另一台/新固件的路由器提供（与 Q8B 成功时不是同一设备），扫描能列出（标签可能显示 sae），但 `select_rsn` 拒绝 → 立即失败。
   - **验证**：`devcmd /dev/wl0 list` 查看该 SSID 各 BSS 的 `auth/cipher` 标签（应为 wpa2-psk/ccmp；若为 sae 或含 tkip 即命中）。

2. **BDF 变体（项目内可修，嫌疑上升）**
   - 当前 TW220 chip2 使用通用 BDF（block#6）；容器中的 NTM_TW220（block#117）是 chip18 条目，不能直接用于当前 hw2.0。
   - 若 TW220 板载模组的 TX 校准在通用 BDF 下偏差大 → 扫描（RX）正常、关联/握手帧（TX）丢失 → B 类超时/MIC 失败。
   - 若要为 hw2.0 使用专用 TW220 校准，需要取得明确以 `qmi-chip-id=2` 发布的 TW220 BDF；不能用当前 chip18 NTM_TW220 条目替代。

3. **regulatory / 配置文件差异**
   - TW220 镜像若无 `/etc/wlan.country`（Q8B 用 CN），默认 world(00) 对部分 5 GHz 信道限功率，`channel_power<0` → A 类拒绝。核对两板镜像的 `/etc/wlan.country`、`/etc/wlan.width`、`/etc/wlan.phy`。

4. **MAC / AP 过滤（概率较低）**
   - 无 OTP 的模组由硬件种子派生稳定的本地管理 MAC（`wcn_wmi.c:178-184`），两板一致；仅当 AP 开 MAC 白名单且未收录 TW220 的 MAC 时才会被拒（表现为 `association-rejected`/`AP-disconnected`）。

### 7.3 定位指令（一条命令区分上述各类）

在 TW220 上连接失败后执行：

```
devcmd /dev/wl0 state     # station=… error=… status=<802.11原因码> wpa=…
devcmd /dev/wl0 list      # 该 SSID 各 BSS 的 auth/cipher 标签
```

对照：
- `station=idle` 且立即失败 → A 类：先看 `list` 的 auth/cipher（PMF/SAE？），再核对 `/etc/wlan.country`、`/etc/wlan.width`、`/etc/wlan.phy`；
- `error=WPA-failed` 或 `handshake-timeout` 或 `message3-MIC` → B 类：优先怀疑 BDF 变体（§7.2-2）与密码；
- `error=AP-disconnected`/`association-rejected` + 原因码 → 查 AP 侧（PMF 强制、MAC 过滤）。

---

### 7.4 曾怀疑的 MAC 冲突与后续证伪（2026-11-06 实机证据）

**新增证据（实机串口日志 + Q8B 对照日志）：**

| 机器 | `WMI READY MAC` | `MAC source` | 来源 |
|---|---|---|---|
| Q8B（Radxa Dragon） | **`1e:b9:7a:e5:8e:ed`** | `deterministic hardware identity seed` | `.tmp/q8b-wifi/normal-progress.log`（`machine radxa-dragon-q8b`） |
| TW220（Robo & Kala） | **`1e:b9:7a:e5:8e:ed`** | `deterministic hardware identity seed` | TW220 实机串口日志 |

- 两台不同板子的 WLAN 派生 MAC **完全相同**。
- seed 公式（`wcn_mhi.c:189-197`）：常量 ^ (VID/DID/SSVID/SSDID) ^ BAR0 ^ (bus/device/function)，仅当端点有 PCIe Device Serial 时才混入 serial。两台板子的端点芯片相同（17cb:1103/17cb:0108）、无 OTP/serial/VPD → **seed 相同 → MAC 相同**。这是必然结果，代码注释亦自述 "cannot distinguish two identical unprogrammed cards"。
- TW220 串口日志失败模式：`association 成功`（状态机进入 WPA 阶段）但 **20 秒收不到 EAPOL-M1 → `WPA failed=handshake-timeout` → `station failure=WPA-failed status=0`**（无 MIC/bad 类错误，纯超时）。
- 机制（同一 AP、同一 SSID —— 用户已确认）：Q8B 先以 `1e:b9:7a:e5:8e:ed` 完成 4-way 并保持授权会话；TW220 用**同一 MAC** 关联后，AP 视其为已授权站点、**不再重新发起 EAPOL 握手** → M1 永不发送 → 超时。与"扫描正常、关联成功、握手超时"的症状完全吻合。

> 这组现象最初支持“派生 MAC 冲突”，但后续清空 AP 状态并重启路由器后仍可复现，因此不能作为最终根因。

**更新（2026-11-06 「重启路由器后现象一样」）：MAC 假说被证伪，转入数据面/RF 排查**

- 路由器重启（站点表清空）+ Q8B 断电后，TW220 仍然 `WPA failed=handshake-timeout`。
- 后续 TW220 日志还显示同一设备两次恢复的派生 MAC 从
  `1e:b9:7a:e5:8e:ed` 变为 `1e:b9:7a:05:8e:ed`。原实现把 PCIe BAR0
  放进 MAC seed；BAR0 可在 stop/start 后被重新分配，不能作为硬件身份。
  现已从 `wcn_mhi.c` 的 seed 移除 BAR0，并保留 `/etc/wlan.mac` 覆盖入口。
- 这些实验不支持用原 AP 的残留会话解释全部故障。当前代码在等待 M1 时会发送
  EAPOL-Start，用户日志也有 `data_tx=1`，因此之前“不发任何帧、纯 RX 侧”的
  推断不成立。`messages=0` 只能确认 M1 尚未交付 supplicant。
- 仍需区分描述符/固件配置、数据 TX/RX、模组实例、板级 RF 和协商模式等因素，
  不能仅凭管理面成功排除驱动数据路径问题。
**计数器定位（2026-11-06 用户提供 `station`/`rings`/`list` 输出）：**

- `station=associated-awaiting-wpa mhz=2447` → **关联在 2.4 GHz ch8**（Q8B 参考成功链路为 5 GHz ch48/160 MHz）。
- `tx=2 done=2 rx=2 lmac=0 data_rx=0 eapol_rx=0` → 管理面双向正常（auth/assoc 请求+响应），**数据面 RX 零交付**。
- `rings`：`DP active=1`，RX PHY 统计为初始值，RX 完成环未推进 → 主机尚未观察到数据 RX 完成，不能据此断言空口没有数据帧。
- `wpa=…message1… messages=0 bad=0` → supplicant 干净地等 M1 至超时。
- `list`：该 SSID 是 **hidden**，缓存中可见 BSS 全在 ch8，且 BSSID 均为本地管理位（ce:…/fe:…，疑似 mesh/中继组网）；**未见 5 GHz BSS**（若有，find_bss 会给 5 GHz +40/+25 偏置而选中）。

**后续更新：** 用户已确认单独手机热点同样失败，不能再将 mesh/隐藏 SSID
作为充分解释。2447 MHz 关联成功而 M1 未交付是可重复观察；5 GHz 扫描、
BDF 和 RF 仍可作为实验变量，但不能替代对驱动数据收发路径的核查。

### 7.5 修复（已落地代码 + 部署步骤）

1. **代码**（`machines/q8b/system/drivers/wlan/wcn_wmi.c`，MAC 优先级调整）：
   - 新优先级：真实模组地址（permanent/extra）> **`/etc/wlan.mac` 显式配置** > 确定性派生 > 无。
   - 之前 `/etc/wlan.mac` 只在"无硬件 seed"时才生效；现改为在"只有会冲突的确定性派生"时**优先**使用显式配置。
   - Q8B 无 `/etc/wlan.mac` → 行为不变；真实模组地址仍然优先于配置文件。
   - 已用 `aarch64-elf-gcc -fsyntax-only` 编译验证（RC=0）。
2. **部署**（TW220 持久 rootfs）：
   - 重建并部署 wlan 驱动（`make -C machines/q8b/system/drivers/wlan` → SCP 到 `/drivers/q8b/wlan`）。
   - 新建 `/etc/wlan.mac`，写入 TW220 特有的单播地址（17 字符 `aa:bb:cc:dd:ee:ff`，首字节 bit0=0 且非全零），例如 `02:1e:b9:7a:e5:8e:ee`。
   - 重启后日志应出现 `WLAN MAC source=/etc/wlan.mac override`，且与 Q8B 不再相同。
3. **零代码快速验证**（可选，先于重建）：把 Q8B 从该 AP 断开（或断电），单独让 TW220 连接——若成功即坐实 MAC 冲突。

4. **撤回按频段选择 TCL LMAC 的实验（2026-11-07）**：之前认定 2.4 GHz
   必须使用 LMAC1 的结论有误。Linux ath11k 对 WCN6855 hw2.0/hw2.1 使用
   单个 host pdev，host pdev index 0 映射为 TCL LMAC0；该字段选择 AST
   查找命名空间，不能直接按物理频段赋值。`wcn_sta.c` 恢复所有频段使用
   `lmac=0`。这能撤销错误改动，但原始故障在 LMAC0 下也出现过，不能据此
   宣称已解决。新增数据 TX completion 来源、原因和原始字段日志，以及失败前
   DP 状态快照，用于继续区分 TX 未获 ACK 与 RX 数据交付异常。

5. **TW220 BDF A/B 实验**：驱动现在按 QMI `chip=2, board=255` 自动选择
   `HW_GK3`，启动日志应出现
   `BDF variant=HW_GK3 (QMI chip=2 board=255 default)`，随后加载 60008 字节
   的 HW_GK3 BDF。Q8B 的 `chip=18` 不会命中该变体。若手机 AP 仍为
   `eapol_rx=0`，说明 BDF 切换尚未解决问题；驱动数据路径与天线/供电/RF
   仍需分别验证。

### 7.6 长期改进建议

- 派生 seed 中混入**板级唯一**信息（Radxa EEPROM 序列号、NVMe 序列号或 UEFI SMBIOS UUID），使未编程模组的各板派生 MAC 天然不同，避免每台设备都要手工配置 `/etc/wlan.mac`。
- 对量产 QCNFA765 卡，优先从模组 OTP/VPD 导出永久 MAC（现已有 `permanent_mac`/VPD 路径，仅在模组有校准且可读时生效）。

---

## 附：证据可复现性

- board-2.bin 解析脚本（与 `wcn_board.c` 同偏移 20 的容器解析）：见本次分析临时脚本 `/tmp/bdparse*.py`（QCA-ATH11K-BOARD 容器，name TLV=0 / data TLV=1）。
- 上游引用：
  - ath11k ML 板卡提交：https://lore.kernel.org/ath11k/CAH2e8h7BHvRQNo9gK+LPV4EB6WOtm-ahw_c0V00waBs9j99W9g@mail.gmail.com/
  - Ntmer TW220 主线 DTS 提交：https://lore.kernel.org/all/20250617092929.1492750-1-mitltlatltl@gmail.com/
  - R-K Linux bring-up 仓库：`/Users/tony/work/q8b/robo-kala-ntmer-tw220/`
