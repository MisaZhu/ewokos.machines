#!/usr/bin/env python3
"""sendkey_seq.py — 经 HMP monitor 把一串文本逐键注入 guest (VGA 键盘)。

用于无 display 的 GRUB 等 guest: 先把它的控制台切到串口, 之后输入全走串口。
qcode 映射覆盖小写字母/数字/空格/下划线/常用符号; 大写与符号用 shift-<键>。

用法: sendkey_seq.py <hmp.sock> <text> [--ret]
"""
import socket
import sys
import time

QCODE = {
    ' ': 'spc', '-': 'minus', '=': 'equal', '+': 'equal', '.': 'dot',
    ',': 'comma', '/': 'slash', ';': 'semicolon', "'": 'apostrophe',
    '(': '9', ')': '0', '_': 'minus', ':': 'semicolon',
}


def key_cmd(ch):
    if ch.isalpha():
        code = ch.lower()
        shift = ch.isupper()
    elif ch.isdigit():
        code, shift = ch, False
    elif ch in QCODE:
        shift = ch in '_:(+)'
        code = QCODE[ch]
    else:
        raise SystemExit(f"no qcode mapping for {ch!r}")
    return f"sendkey shift-{code}" if shift else f"sendkey {code}"


def main():
    sock_path, text = sys.argv[1], sys.argv[2]
    ret = "--ret" in sys.argv
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock_path)
    s.settimeout(2)
    time.sleep(0.3)
    try:
        s.recv(65536)  # banner
    except socket.timeout:
        pass

    def cmd(c):
        s.sendall((c + "\n").encode())
        time.sleep(0.08)
        try:
            s.recv(65536)
        except socket.timeout:
            pass

    for ch in text:
        cmd(key_cmd(ch))
    if ret:
        cmd("sendkey ret")
    s.close()


if __name__ == "__main__":
    main()
