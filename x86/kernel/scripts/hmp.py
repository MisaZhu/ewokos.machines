#!/usr/bin/env python3
"""hmp.py — 向 QEMU HMP monitor unix socket 发命令
用法: hmp.py <sock> <cmd> [cmd...]   例: hmp.py out/mon.sock 'screendump out/s.ppm'
"""
import socket, sys, time

def hmp_cmd(sock_path, cmd, timeout=5.0):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(sock_path)
    time.sleep(0.3)
    try:
        s.recv(65536)          # banner
    except socket.timeout:
        pass
    s.sendall((cmd + "\n").encode())
    time.sleep(1.0)
    out = b""
    try:
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            out += chunk
    except socket.timeout:
        pass
    s.close()
    return out.decode(errors="replace")

if __name__ == "__main__":
    print(hmp_cmd(sys.argv[1], " ".join(sys.argv[2:])))
