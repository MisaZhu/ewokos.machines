#!/bin/bash
# =====================================================================
# Q8B 真机自动化调试循环
#
#   EUD 复位 → PXE 下发 BOOTAA64.EFI/KERNEL.BIN → SD rootfs → 串口抓 log
#
# 用法:
#   ./test/board_cycle.sh                 # 部署最新 payload,跑 1 轮
#   CYCLES=5 ./test/board_cycle.sh        # 失败自动重试,最多 5 轮
#   DEPLOY=0 ./test/board_cycle.sh        # 不覆盖 TFTP,直接复位
#   SERIAL=/dev/cu.usbserial-0035E5971 ./test/board_cycle.sh
#
# 前置:
#   - eud 工具在 /Users/tony/work/q8b/eud(可用 EUD_DIR 覆盖)
#   - pxe/setup-sudoers.sh 已执行(service.sh 可免密 start)
#   - 40-pin UART 接在 $SERIAL,115200 8N1
#   - TFTP 目录下已有最新 payload(DEPLOY=1 时自动从本机 build 复制)
#
# 退出码:
#   0  内核已加载 init/打印 q8b: bootinfo ok(单核启动链跑通)
#   3  已过 ExitBootServices,但没看到内核串口输出(需查 UART/内核早期)
#   2  超时/没到 EBS
#   1  明确 FATAL/panic
# =====================================================================
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"                 # machines/q8b
EUD_DIR="${EUD_DIR:-/Users/tony/work/q8b/eud}"
PXE_DIR="$EUD_DIR/pxe"
TFTP_DIR="$PXE_DIR/tftp/q8b"
LOG_DIR="${LOG_DIR:-$EUD_DIR/logs/board}"
SERIAL="${SERIAL:-$(ls /dev/cu.usbserial-* 2>/dev/null | head -1)}"
BAUD="${BAUD:-115200}"
CAPTURE_SECS="${CAPTURE_SECS:-360}"
RESET="${RESET:-1}"   # 1=EUD SRST 复位；0=等你手动断电重启
ALLOW_WDT="${ALLOW_WDT:-0}"  # 1=即使上次是 watchdog 复位也继续
RESET_RETRIES="${RESET_RETRIES:-10}"
CYCLES="${CYCLES:-1}"
DEPLOY="${DEPLOY:-1}"
KERNEL_BIN="${KERNEL_BIN:-$ROOT/kernel/kernel_up.bin}"
STUB_EFI="${STUB_EFI:-$ROOT/kernel/boot/uefi/BOOTAA64.EFI}"
ROOTFS_IMG="${ROOTFS_IMG:-$ROOT/root_aarch64.img}"
SD_ROOTFS="${SD_ROOTFS:-1}"

mkdir -p "$LOG_DIR" || exit 1

say() { printf '%s\n' "$*"; }
die() { say "错误: $*" >&2; exit 1; }

[ -n "$SERIAL" ] || die "未找到 /dev/cu.usbserial-*,请接 40-pin UART 或设 SERIAL="
[ -x /usr/bin/python3 ] || die "缺少 python3"
[ -f "$EUD_DIR/server/eud_usb.py" ] || die "缺少 $EUD_DIR/server/eud_usb.py"
[ -f "$EUD_DIR/server/serial_link.py" ] || die "缺少 $EUD_DIR/server/serial_link.py"

deploy_payload() {
    [ -f "$STUB_EFI" ] || die "缺少 $STUB_EFI"
    [ -f "$KERNEL_BIN" ] || die "缺少 $KERNEL_BIN(先 make SMP=no)"
    if [ "$SD_ROOTFS" != 1 ]; then
        [ -f "$ROOTFS_IMG" ] || die "缺少 $ROOTFS_IMG(先 make -C system ROOTFS_MB=32 sd)"
    fi
    mkdir -p "$TFTP_DIR"
    cp "$STUB_EFI" "$TFTP_DIR/BOOTAA64.EFI"
    cp "$KERNEL_BIN" "$TFTP_DIR/KERNEL.BIN"
    if [ "$SD_ROOTFS" = 1 ]; then
        rm -f "$TFTP_DIR/ROOTFS.IMG"
    else
        cp "$ROOTFS_IMG" "$TFTP_DIR/ROOTFS.IMG"
    fi
    chmod 644 "$TFTP_DIR/BOOTAA64.EFI" "$TFTP_DIR/KERNEL.BIN"
    say ">> payload:"
    ls -l "$TFTP_DIR/BOOTAA64.EFI" "$TFTP_DIR/KERNEL.BIN"
    [ "$SD_ROOTFS" = 1 ] || ls -l "$TFTP_DIR/ROOTFS.IMG"
}

ensure_pxe() {
    if pgrep -f 'dnsmasq.*eud/pxe/dnsmasq.conf' >/dev/null 2>&1; then
        say ">> PXE: dnsmasq 已在运行"
        return 0
    fi
    say ">> PXE: 启动 dnsmasq ..."
    sh "$PXE_DIR/service.sh" start || die "PXE 启动失败(检查 sudoers/网口)"
}

check_tftp_files() {
    local f
    for f in BOOTAA64.EFI KERNEL.BIN; do
        [ -f "$TFTP_DIR/$f" ] || die "TFTP 缺文件: $TFTP_DIR/$f"
    done
}

one_cycle() {
    local tag log cap_pid reset_out ok=0 stage
    tag="$(date +%Y%m%d-%H%M%S)"
    log="$LOG_DIR/board-$tag.log"
    say ""
    say "===== Q8B cycle $tag ====="
    say ">> serial: $SERIAL @$BAUD  ->  $log"

    pkill -f 'serial_link.py capture' 2>/dev/null || true
    sleep 0.5
    : > "$log"
    : > "$log.err"
    # 板子断电时 USB-UART 可能短暂重枚举；wrapper 自动重开并续写同一份 log。
    (
        end=$(( $(date +%s) + CAPTURE_SECS ))
        dev="$SERIAL"
        while [ "$(date +%s)" -lt "$end" ]; do
            [ -e "$dev" ] || dev="$(ls /dev/cu.usbserial-* 2>/dev/null | head -1)"
            [ -n "$dev" ] || { sleep 1; continue; }
            remain=$((end - $(date +%s)))
            [ "$remain" -gt 0 ] || break
            python3 "$EUD_DIR/server/serial_link.py" capture -d "$dev" -b "$BAUD" \
                --seconds "$remain" >>"$log" 2>>"$log.err"
            sleep 1
        done
    ) &
    cap_pid=$!
    sleep 1

    if [ "$RESET" = 1 ]; then
        reset_out=""
        for i in $(seq 1 "$RESET_RETRIES"); do
            reset_out="$(python3 "$EUD_DIR/server/eud_usb.py" reset 2>&1)"
            if printf '%s' "$reset_out" | grep -q "CLR: OK / SET: OK"; then
                ok=1
                break
            fi
            say ">> EUD reset 未就绪($i/$RESET_RETRIES): $(printf '%s' "$reset_out" | tr '\n' ' ')"
            if grep -aqE "EwokOS Q8B UEFI loader|UEFI Ver" "$log"; then
                say ">> 检测到板子已在上电启动,停止 EUD 重试,继续抓取。"
                break
            fi
            kill -0 "$cap_pid" 2>/dev/null || break
            sleep 2
        done
        if [ "$ok" != 1 ]; then
            say ">> EUD 9501 未枚举(板子可能未上电/USB 未枚举)。"
            say ">> 转为冷启动等待: 请现在给板子上电或断电重启;串口仍在抓,不退出。"
        else
            say ">> EUD SRST_N 复位已发出"
        fi
    else
        say ""
        say ">> RESET=0: 请现在给板子断电重启(保持 40-pin 串口;脚本已在抓 log)"
        say ">> 注意: 拔的是板子供电(USB-C PD 电源口/12-20V header),不是 EUD 数据线。"
        say ">> 冷启动成功标志: SBL 首个 'B -' 时间戳 <10s,随后 UEFI Ver/PXE。"
        say ">> 等待 PXE -> stub -> kernel/rootfs,最长 ${CAPTURE_SECS}s ..."
    fi

    # 阶段轮询:看到成功/失败/抓取结束就退出
    local seen_stub=0 seen_sbl=0 seen_bootinfo=0 seen_kwin=0 seen_kernel=0
    local seen_rootfs=0 seen_ebs=0 seen_kstart=0 seen_kmmu=0 seen_klog=0 seen_fail=0
    local seen_btime=0 seen_raw=0
    local ksize rsize
    ksize="$(stat -f %z "$TFTP_DIR/KERNEL.BIN")"
    rsize=0
    [ "$SD_ROOTFS" = 1 ] || rsize="$(stat -f %z "$TFTP_DIR/ROOTFS.IMG")"
    local deadline=$(( $(date +%s) + CAPTURE_SECS ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if [ "$seen_btime" = 0 ]; then
            first_b="$(grep -a -m1 -E '^B - +[0-9]+' "$log" 2>/dev/null | grep -oE '[0-9]+' | head -1)"
            if [ -n "$first_b" ]; then
                seen_btime=1
                if [ "$first_b" -gt 10000000 ]; then
                    say "   [!] SBL 计时 ${first_b}us (>10s): 本轮没有真正断电，板子未复位。"
                else
                    say "   [stage] SBL 从 ${first_b}us 开始(已真正复位)"
                fi
            fi
        fi
        if [ "$seen_sbl" = 0 ] && grep -aqE "ENUM success|UEFI Ver" "$log"; then
            seen_sbl=1; say "   [stage] SBL/UEFI 已过 USB 枚举"
        fi
        if [ "$seen_raw" = 0 ] && grep -aq "K12" "$log"; then
            seen_raw=1; say "   [stage] 内核入口裸串口 K/1/2 已出现"
        fi
        if [ "$seen_stub" = 0 ] && grep -aq "EwokOS Q8B UEFI loader" "$log"; then
            seen_stub=1; say "   [stage] stub 已运行"
        fi
        if [ "$seen_bootinfo" = 0 ] && grep -aq "bootinfo window reserved" "$log"; then
            seen_bootinfo=1; say "   [stage] bootinfo 页已预留"
        fi
        if [ "$seen_kwin" = 0 ] && grep -aq "kernel window reserved" "$log"; then
            seen_kwin=1; say "   [stage] kernel 页已预留"
        fi
        if [ "$seen_kernel" = 0 ] && grep -aq "MTFTP4 ok, $ksize" "$log"; then
            seen_kernel=1; say "   [stage] KERNEL.BIN 已下载"
        fi
        if [ "$SD_ROOTFS" = 1 ]; then
            seen_rootfs=1
        elif [ "$seen_rootfs" = 0 ] && grep -aq "MTFTP4 ok, $rsize" "$log"; then
            seen_rootfs=1; say "   [stage] ROOTFS.IMG 已下载"
        fi
        if [ "$seen_ebs" = 0 ] && grep -aqE "Exit EBS|UEFI End" "$log"; then
            seen_ebs=1; say "   [stage] 已过 ExitBootServices"
        fi
        if [ "$seen_kstart" = 0 ] && grep -aq "q8b: _boot_start" "$log"; then
            seen_kstart=1; say "   [stage] 内核已到 _boot_start(C 入口)"
        fi
        if [ "$seen_kmmu" = 0 ] && grep -aq "q8b: boot page tables on" "$log"; then
            seen_kmmu=1; say "   [stage] 内核 MMU/页表已开启"
        fi
        if [ "$seen_klog" = 0 ] && grep -aqE "q8b: bootinfo ok|loading init" "$log"; then
            seen_klog=1; say "   [stage] 内核已打印 bootinfo/init"
        fi
        if [ "$seen_fail" = 0 ] && grep -aqE "FATAL|panic|Exception|Data abort|Instruction abort" "$log"; then
            seen_fail=1; say "   [stage] 检测到 FATAL/panic/abort"
        fi
        if [ "$seen_klog" = 1 ]; then
            break
        fi
        sleep 2
    done

    kill "$cap_pid" 2>/dev/null || true
    pkill -f 'serial_link.py capture' 2>/dev/null || true
    wait "$cap_pid" 2>/dev/null || true

    say ">> 日志: $log ($(wc -c < "$log" 2>/dev/null || echo 0) 字节)"
    say ">> 阶段汇总:"
    ksize="$(stat -f %z "$TFTP_DIR/KERNEL.BIN")"
    rsize=0
    [ "$SD_ROOTFS" = 1 ] || rsize="$(stat -f %z "$TFTP_DIR/ROOTFS.IMG")"
    for m in "EwokOS Q8B UEFI loader" "bootinfo window reserved" \
             "kernel window reserved" "MTFTP4 ok, $ksize" \
             "Exit EBS" "UEFI End" \
             "K12" "q8b: _boot_start" "q8b: boot page tables on" \
             "q8b: bootinfo ok" "loading init"; do
        if grep -aq "$m" "$log"; then say "   [ok] $m"; else say "   [--] $m"; fi
    done
    if [ "$SD_ROOTFS" = 1 ]; then
        say "   [ok] rootfs: SD ext3 (PXE ROOTFS.IMG skipped)"
    elif grep -aq "MTFTP4 ok, $rsize" "$log"; then
        say "   [ok] MTFTP4 ok, $rsize"
    else
        say "   [--] MTFTP4 ok, $rsize"
    fi
    if grep -aqE "FATAL|panic|Exception|Data abort|Instruction abort" "$log"; then
        say ">> 最后 30 行:"
        strings -n 2 "$log" | tail -30
        return 1
    fi
    if grep -aqE "q8b: bootinfo ok|loading init" "$log"; then
        say ">> 结果: 内核已启动(单核启动链通过)"
        return 0
    fi
    if grep -aqE "Exit EBS|UEFI End" "$log"; then
        say ">> 结果: 已过 EBS,但没有内核串口输出(检查 UART / 内核早期 / 色块)"
        return 3
    fi
    say ">> 结果: 超时,未到 ExitBootServices"
    say ">> 最后 30 行:"
    strings -n 2 "$log" | tail -30
    return 2
}

main() {
    say "Q8B board cycle: EUD_DIR=$EUD_DIR  SERIAL=$SERIAL"
    if [ "$ALLOW_WDT" != 1 ] && ls "$LOG_DIR"/board-*.log >/dev/null 2>&1; then
        last="$(ls -t "$LOG_DIR"/board-*.log | head -1)"
        if grep -aq "FAULT WD TO S3" "$last"; then
            say "!! 上一轮日志检测到 watchdog 复位: $last"
            say "!! 先修 watchdog/避免 EBS 后挂住，再上电测试。"
            say "!! 如确需继续: ALLOW_WDT=1 ./test/board_cycle.sh"
            exit 4
        fi
    fi
    if [ "$DEPLOY" = 1 ]; then deploy_payload; fi
    check_tftp_files
    ensure_pxe

    local i rc
    for i in $(seq 1 "$CYCLES"); do
        one_cycle
        rc=$?
        say ">> cycle $i 退出码: $rc"
        [ "$rc" = 0 ] && exit 0
        [ "$i" -lt "$CYCLES" ] && say ">> 准备下一轮 ..." && sleep 2
    done
    exit "${rc:-2}"
}

main "$@"
