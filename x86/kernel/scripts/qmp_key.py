#!/usr/bin/env python3
"""qmp_key.py — 经 QMP input-send-event 模拟一次真实按键(按下-保持-释放)。

HMP sendkey 的按下/释放间隔只有几毫秒, xHCI 的 16ms 中断轮询会把按下和
释放快照塌缩进 hid_keybd 的同一个耗尽窗口(它只保留最新快照), 按键就像
没发生过。真实键盘的按下持续几十到几百毫秒, 这里按同样语义注入:
按住 hold_ms 再释放, 保证多个轮询周期看到按下状态。

用法: qmp_key.py <qmp.sock> <qcode> [hold_ms, 默认 400]
"""
import json
import socket
import sys
import time


def main():
    sock_path, qcode = sys.argv[1], sys.argv[2]
    hold_ms = int(sys.argv[3]) if len(sys.argv) > 3 else 400

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    f = s.makefile("rw")
    f.readline()  # greeting
    f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n")
    f.flush()
    f.readline()

    def key_event(down):
        ev = {"type": "key",
              "data": {"down": down, "key": {"type": "qcode", "data": qcode}}}
        f.write(json.dumps({"execute": "input-send-event",
                            "arguments": {"events": [ev]}}) + "\n")
        f.flush()
        f.readline()

    key_event(True)
    time.sleep(hold_ms / 1000.0)
    key_event(False)
    s.close()


if __name__ == "__main__":
    main()
