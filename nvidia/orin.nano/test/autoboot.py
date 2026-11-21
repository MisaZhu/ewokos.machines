#!/usr/bin/env python3
"""Orin Nano automated build/boot/test pipeline (board-pxe MCP).

Cycle: build -> deploy -> reset (PSCI SYSTEM_RESET via /bin/reboot) ->
wait for the serial login prompt -> run test commands -> report PASS/FAIL.
Replaces the manual power-cycle loop.  Requires the board-pxe MCP server
running on TCP 8899 (sudo ~/work/pxe/start.sh) and the board on PXE.

Subcommands:
  build           build kernel(UP)/system/efi and deploy into the PXE dir
  reset           login + /bin/reboot (PSCI SYSTEM_RESET)
  boot            wait for the login prompt (after a reset/power-on)
  login           login root/root
  test            login + run --test commands, assert output
  cycle           build -> reset -> boot -> test  (one full iteration)

Examples:
  python3 autoboot.py cycle
  python3 autoboot.py cycle --test "uname; ps" --timeout 180
  python3 autoboot.py reset && python3 autoboot.py boot
"""
import argparse
import json
import os
import re
import socket
import subprocess
import sys
import time

DEFAULT_REPO = os.path.expanduser("~/work/ewokos")
ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")


def log(tag, msg):
    print("[%s] %s" % (tag, msg), flush=True)


def strip_ansi(s):
    return ANSI.sub("", s)


class MCP:
    """Minimal board-pxe MCP TCP JSON-RPC client."""

    def __init__(self, host="127.0.0.1", port=8899):
        self.host, self.port = host, port

    def call(self, tool, args=None):
        with socket.create_connection((self.host, self.port), timeout=90) as s:
            self._send(s, {"jsonrpc": "2.0", "id": 1, "method": "initialize",
                           "params": {"protocolVersion": "2025-06-18",
                                      "capabilities": {},
                                      "clientInfo": {"name": "autoboot", "version": "1.0"}}})
            self._recv(s)
            self._send(s, {"jsonrpc": "2.0", "method": "notifications/initialized"})
            self._send(s, {"jsonrpc": "2.0", "id": 2, "method": "tools/call",
                           "params": {"name": tool, "arguments": args or {}}})
            r = self._recv(s)
        if "error" in r:
            raise RuntimeError("MCP error: %s" % r["error"])
        res = r.get("result", {})
        text = "\n".join(c.get("text", "") for c in res.get("content", [])
                         if c.get("type") == "text")
        if res.get("isError"):
            raise RuntimeError("tool %s failed: %s" % (tool, text))
        return text

    @staticmethod
    def _send(s, o):
        s.sendall((json.dumps(o, ensure_ascii=False) + "\n").encode())

    @staticmethod
    def _recv(s):
        buf = b""
        while not buf.endswith(b"\n"):
            c = s.recv(65536)
            if not c:
                raise ConnectionError("MCP connection closed")
            buf += c
        return json.loads(buf.decode())


class Rig:
    def __init__(self, repo, port=8899, user="root", password="root"):
        self.repo = repo
        self.machine = os.path.join(repo, "machines/nvidia/orin.nano")
        self.efi = os.path.join(self.machine, "efi")
        self.mcp = MCP(port=port)
        self.user, self.password = user, password

    # ---- serial helpers ----
    def read(self, lines=200, since=None):
        args = {"action": "read", "lines": lines}
        if since is not None:
            args["since"] = str(since)
        return strip_ansi(self.mcp.call("serial_log", args))

    def send(self, command, wait=2):
        return strip_ansi(self.mcp.call(
            "serial_log", {"action": "send", "command": command, "wait": wait}))

    def status(self):
        return self.mcp.call("serial_log", {"action": "status"})

    # ---- build / deploy ----
    def _make(self, cwd, args):
        cmd = ["make", "-C", cwd] + args
        log("BUILD", " ".join(cmd))
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.returncode != 0:
            sys.stderr.write(p.stdout[-4000:] + "\n" + p.stderr[-4000:] + "\n")
            raise RuntimeError("make failed in %s" % cwd)
        return p.stdout

    def build(self, smp=False):
        kern = os.path.join(self.machine, "kernel")
        # CFLAGS flag changes are not tracked; force a clean BSP rebuild.
        variant = "orin.nano" if smp else "orin.nano_up"
        subprocess.run(["rm", "-rf", os.path.join(kern, "bsp/aarch64", variant)])
        self._make(kern, ["SMP=%s" % ("yes" if smp else "no")])
        self._make(os.path.join(self.machine, "system"), [])
        self._make(self.efi, ["efi", "initramfs"])
        self.deploy(smp=smp)

    def deploy(self, smp=False):
        # The kernel Makefile's objcopy always writes kernel.bin (the fresh
        # build output, UP or SMP per the SMP flag); kernel_up.bin is a stale
        # manual copy, so deploy kernel.bin directly.
        kern = os.path.join(self.machine, "kernel")
        src = os.path.join(kern, "kernel.bin")
        dst = os.path.join(self.efi, "kernel.bin")
        subprocess.run(["cp", src, dst], check=True)
        log("DEPLOY", "kernel.bin <- kernel/kernel.bin (%s)" %
            ("SMP" if smp else "UP"))

    # ---- console interaction ----
    def login(self, tries=3):
        for _ in range(tries):
            tail = self.read(lines=40)
            if re.search(r"\]:/.*#", tail):  # already at a shell prompt
                return True
            self.send(self.user, wait=2)
            self.send(self.password, wait=2)
            tail = self.read(lines=40)
            if re.search(r"\]:/.*#", tail):
                return True
            time.sleep(1)
        return False

    def reset(self):
        if not self.login():
            raise RuntimeError("cannot login to issue reboot")
        t0 = time.time()
        log("RESET", "PSCI SYSTEM_RESET via /bin/reboot")
        self.send("reboot", wait=2)
        return t0

    def wait_boot(self, since=None, timeout=180):
        since = since if since is not None else time.time()
        deadline = time.time() + timeout
        log("BOOT", "waiting for login prompt (timeout %ds)" % timeout)
        while time.time() < deadline:
            txt = self.read(lines=4000, since=since)
            if "Synchronous Exception" in txt or "Unhandled Exception in EL3" in txt:
                raise RuntimeError("boot crashed:\n" + "\n".join(txt.splitlines()[-15:]))
            if re.search(r"\[/dev/tty0\] login:", txt):
                log("BOOT", "login prompt reached (%.0fs)" % (time.time() - since))
                return True
            time.sleep(3)
        raise RuntimeError("boot timeout: no login prompt in %ds" % timeout)

    def run_test(self, commands):
        if not self.login():
            raise RuntimeError("login failed before test")
        results = []
        for cmd in commands:
            out = self.send(cmd, wait=2)
            ok = "not found" not in out and "failed" not in out.lower()
            results.append((cmd, ok, out))
            log("TEST", "%s -> %s" % (cmd, "ok" if ok else "FAIL"))
        return results


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["build", "deploy", "reset", "boot", "login", "test", "cycle"])
    ap.add_argument("--repo", default=DEFAULT_REPO)
    ap.add_argument("--port", type=int, default=8899)
    ap.add_argument("--smp", action="store_true", help="build/deploy the SMP kernel")
    ap.add_argument("--timeout", type=int, default=180, help="boot wait timeout (s)")
    ap.add_argument("--test", default="uname", help="semicolon-separated test commands")
    ap.add_argument("--user", default="root")
    ap.add_argument("--password", default="root")
    a = ap.parse_args()

    rig = Rig(a.repo, port=a.port, user=a.user, password=a.password)
    try:
        if a.cmd == "build":
            rig.build(smp=a.smp)
            log("DONE", "build + deploy ok")
        elif a.cmd == "deploy":
            rig.deploy(smp=a.smp)
        elif a.cmd == "reset":
            rig.reset()
            log("DONE", "reset issued")
        elif a.cmd == "boot":
            rig.wait_boot(timeout=a.timeout)
        elif a.cmd == "login":
            if not rig.login():
                raise RuntimeError("login failed")
            log("DONE", "logged in")
        elif a.cmd == "test":
            cmds = [c.strip() for c in a.test.split(";") if c.strip()]
            res = rig.run_test(cmds)
            if not all(ok for _, ok, _ in res):
                raise RuntimeError("test failed")
            log("PASS", "all tests ok")
        elif a.cmd == "cycle":
            rig.build(smp=a.smp)
            t0 = rig.reset()
            rig.wait_boot(since=t0, timeout=a.timeout)
            cmds = [c.strip() for c in a.test.split(";") if c.strip()]
            res = rig.run_test(cmds)
            if not all(ok for _, ok, _ in res):
                raise RuntimeError("test failed")
            log("PASS", "cycle ok (build->reset->boot->test)")
    except Exception as e:
        log("FAIL", str(e))
        sys.exit(1)


if __name__ == "__main__":
    main()