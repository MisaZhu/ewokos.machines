# Q8B USB / WLAN 并发读取导致同步异常

调试日期：2026-11-05。按 `skills/q8b-kernel-test/SKILL.md` 使用 EUD 复位、PXE 和常驻串口缓存。

## 定位证据

USB 根文件系统来自 SanDisk `0781:5591`，挂在 xHCI2/root10；WLAN 为 PCIe4 上的 WCN6855。实际 PXE 服务的 `tftp-root` 是 `/Users/tony/work/q8b/eud/pxe/tftp`，同时保留 `/Users/tony/work/q8b/boot/tftp/q8b` 镜像。

原内核把 EC=0 误判为异步 external abort，设置 PSTATE.A 后返回同一 PC，形成同步异常死循环。移除该分支后获得完整现场，故障进程会退出，系统仍可登录、执行 ps。

两次诊断启动中 xserverd 均在初始入口 `PC=LR=0x4be60`、`ESR=0x02000000` 触发 EC=0。该类异常的 FAR 不构成有效故障地址，不能用 `FAR=0x2dfe0` 推断 DMA 地址冲突。

| 内容 | 入口前 8 个 32 位字 |
| --- | --- |
| 正常 xserverd 文件偏移 0x4be60 | d11203ff 52800001 d2804002 a9007bfd 910003fd a90363f7 9101a3f7 aa1703e0 |
| 异常进程入口内存 | 75044000 00004047 9120c216 de1b4380 7a104015 9122c057 00004047 9120c259 |
| nfa765/amss.bin 文件偏移 0x4be60 | 75044000 00004047 9120c216 de1b4380 7a104015 9122c057 00004047 9120c259 |

同一 USB 文件 `/drivers/xserverd` 的串行 MD5 先返回 `dfc6370fb23ea7b7304196a036f0601c`，随后恢复正确值 `cf320dc39d1ae7ed81c553f0df20ad1a`。读出的字节数完整，不是文件永久损坏或短读。

## 修复

`system/basic/sys/rootfs/sdfsd.c` 以 multi_task IPC 服务文件系统请求。此前 read/get/kids/mount 使用共享读锁，但 `ext3_get_data_block()` 在读取单重间接块时会刷新全局 `_cached_indirect_block` 和 `_cached_indirect_blk`。设备 IPC 可以阻塞，一个读请求尚未消费缓存中的块号，另一个请求便可能覆盖缓存；可执行文件因此读到了固件的数据块。

`system/basic/libs/ext3/src/ext3fs.c` 改为在读请求内保存私有间接块快照。`ext3_read()` 在扫描连续数据块时复用该快照，单个请求只需读取一次间接块；目录与部分块读取也使用私有缓冲区。读取仍优先观察 deferred-write 缓存中的未提交指针，但不会替换、刷新或写回该共享缓存。全局可变缓存仅由已有独占写锁保护的分配路径更新，sdfsd 原有读写锁策略保持不变。

整请求独占锁曾用于验证定位，16 次校验全部正确，但正常启动复验中 USB 蓝牙固件加载的嵌套读取受到阻塞，因此没有采用该方案。

`kernel/platform/aarch64/arch/common/src/irq.c` 移除 EC=0 的原地重试，使用现有诊断和用户进程终止路径。未添加“吞掉同步异常”或关闭 USB/PCIe 的绕过措施。

## 回归方法

```sh
make -C machines/q8b/kernel
make -C machines/q8b/system
make -C machines/q8b/efi
make -C machines/q8b/test
sh system/basic/libs/ext3/tests/run_read_reentrancy.sh
```

`test/usb-read-check.c` 运行四个线程：两个反复读取 xserverd，两个反复读取 5,079,040 字节的 WLAN 固件；每次完整读取后与主机构建产物的 MD5 比较。测试只读文件，不改持久根文件系统。

```text
usb-read-check /drivers/xserverd cf320dc39d1ae7ed81c553f0df20ad1a /etc/firmware/ath11k/WCN6855/hw2.1/nfa765/amss.bin e38e434e815db0a1f041bc72212d9c94 4
```

本次旧 VFS 引导环境无法访问保存的 `/boot`，因此将工具加入 initramfs，在 `init.ramfs.rd` 里后台启动相同命令并加 `--wait-root`；工具等待 USB 根文件系统出现，再延迟 35 秒开始。对照版与修复版使用相同内核、测试程序、配置、PCIe/USB 二进制，只替换 initramfs 的 `/sbin/sdfsd`。WLAN 保持自动启动/重试，以覆盖 USB 文件读取与 PCIe 固件 DMA 重叠的场景。

主机回归 `read_reentrancy_test.c` 在设备回调已复制元数据、尚未返回时嵌入另一个文件的公开 `ext3_read()`，确定性重现错误交错。旧源码触发数据校验断言；修复源码在 1024/2048/4096 字节块大小下均通过 ASan/UBSan，覆盖完整块、部分块、连续读缓存复用以及 deferred-write 指针可见性和不被读者写回的约束。

对照轮次（22:30:46–22:34:09）完成 16 次整文件校验，10 次失败：xserverd 3/8 失败，固件 7/8 失败。每次返回的长度均完整。串口结果：

```text
usb-read-check: FAIL workers=4 checks=16 failures=10
```

最终私有缓冲区版本（22:52:07–22:55:35）完成相同 16 次校验，全部正确。该轮未出现 core dump、EC=0 异常现场、SError 或 xHCI HSE/HCE 错误。root 登录成功，22:55:54 的 ps 确认 xserverd、xlauncher、xwm_misa 和 statusbar 均正常存活。

```text
usb-read-check: PASS workers=4 checks=16 failures=0
```

两个 TFTP 目录已同步正常镜像：恢复原始 init.ramfs.rd，仅更新 sdfsd，不保留自动测试任务。PCIe/WLAN 和 USB 驱动二进制保持原样。原始镜像、测试镜像和串口缓存摘录保存在工作区 `.tmp/q8b-usb-pcie/`，其中 `concurrent-baseline.log` 与 `concurrent-private.log` 是最终对照。

去掉测试任务后再次 EUD/PXE 启动，23:00:06 出现登录界面，root 登录成功。23:03:04 的 ps 确认 xserverd、xlauncher、xwm_misa、statusbar 正常存活，USB host 约占单核 10%，无同步异常或异常循环。该轮缓存保存在 `normal-private.log`。

正常启动载荷 SHA-256：

```text
KERNEL.BIN     e23a07d47e9dbec378433b917647976c69ca2b4f317e6db40f66851147c09958
INITRAMFS.IMG  86a8ef8ff77a93a02b90b37165cdd1bd5b6d5f320aa8ba79efc19b2961d765ba
```

WLAN 的既有固件启动故障仍存在：本轮在 BHI/BHIE 阶段失败后重试，`devcmd /dev/wl0 state` 显示 `init_failed=true`、无 MAC/IP。这与已复现并消除的文件读取内容串扰分开记录，不能把本轮结果表述为无线网络已经可用。

本轮覆盖并发 USB 文件读取与 WLAN 固件启动 DMA，没有执行持久 USB 写入压力或无线网络持续吞吐测试。
