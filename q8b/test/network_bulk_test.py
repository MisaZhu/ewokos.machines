#!/usr/bin/env python3
"""Unthrottled SCP round trips, independent hashes, and post-load liveness.

Run from any directory. The board needs net/netd/sshd and /bin/md5.
Artifacts (including the full command output) stay in --output.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import random
import re
import subprocess
import threading
import time


def hashes(path):
    md5, sha256 = hashlib.md5(), hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            md5.update(block)
            sha256.update(block)
    return {'bytes': path.stat().st_size, 'md5': md5.hexdigest(),
            'sha256': sha256.hexdigest()}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--board', default='192.168.8.50')
    ap.add_argument('--mib', type=int, default=64)
    ap.add_argument('--rounds', type=int, default=2)
    ap.add_argument('--timeout', type=int, default=600)
    ap.add_argument('--duplex', action='store_true',
                    help='also upload/download simultaneously using separate board files')
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    if args.mib <= 0 or args.rounds <= 0 or args.timeout <= 0:
        ap.error('--mib, --rounds and --timeout must be positive')
    args.output.mkdir(parents=True, exist_ok=True)
    payload = args.output / 'payload.bin'
    rng = random.Random(0x9563)
    with payload.open('wb') as f:
        for _ in range(args.mib):
            f.write(rng.randbytes(1024 * 1024))
        # Exercise a non-block-aligned final packet too.
        f.write(rng.randbytes(137))
    expected = hashes(payload)
    report = {'board': args.board, 'expected': expected, 'steps': [], 'passed': False}
    env = dict(os.environ, SSH_ASKPASS=str(Path(__file__).with_name('askpass_root.sh')),
               SSH_ASKPASS_REQUIRE='force', DISPLAY='none')
    options = ['-o', 'StrictHostKeyChecking=no', '-o', 'UserKnownHostsFile=/dev/null',
               '-o', 'ConnectTimeout=10', '-o', 'ServerAliveInterval=5',
               '-o', 'ServerAliveCountMax=3']
    remote = f'root@{args.board}'
    remote_path = '/tmp/q8b-network-bulk.bin'
    report_lock = threading.Lock()

    def run(label, cmd):
        print(f'{label}: starting', flush=True)
        start = time.monotonic()
        try:
            p = subprocess.run(cmd, env=env, stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               timeout=args.timeout)
            output, code = p.stdout.decode(errors='replace'), p.returncode
        except subprocess.TimeoutExpired as e:
            output, code = (e.stdout or b'').decode(errors='replace'), 124
        seconds = time.monotonic() - start
        step = {'label': label, 'command': cmd, 'exit': code, 'seconds': seconds,
                'output': output}
        with report_lock:
            report['steps'].append(step)
            (args.output / 'report.json').write_text(json.dumps(report, indent=2))
        print(f'{label}: exit={code}, {seconds:.2f}s', flush=True)
        if code:
            raise RuntimeError(f'{label} failed: {output[-1500:]}')
        return step

    def check_health(label):
        stats = run(f'stats-{label}', ['ssh', '-n', *options, remote,
                                       '/bin/devcmd /dev/eth0 stats'])
        counters = re.search(r'errors=(\d+).*fatal=([0-9a-fA-F]+)', stats['output'])
        if counters is None or int(counters[1]) or int(counters[2], 16):
            raise RuntimeError('missing driver counters or RX/DMA error reported')
        ping = run(f'ping-{label}', ['ping', '-c', '5', args.board])
        loss = re.search(r'(\d+(?:\.\d+)?)% packet loss', ping['output'])
        if loss is None or float(loss[1]) != 0:
            raise RuntimeError('post-transfer ping packet loss')

    try:
        for n in range(args.rounds):
            run(f'upload-{n}', ['scp', '-O', '-q', *options, str(payload),
                               f'{remote}:{remote_path}'])
            check = run(f'board-hash-{n}', ['ssh', '-n', *options, remote,
                                           f'/bin/md5 {remote_path}'])
            if expected['md5'] not in check['output'].lower().splitlines():
                raise RuntimeError('board MD5 mismatch')
            downloaded = args.output / f'roundtrip-{n}.bin'
            run(f'download-{n}', ['scp', '-O', '-q', *options,
                                 f'{remote}:{remote_path}', str(downloaded)])
            got = hashes(downloaded)
            report['steps'][-1]['hashes'] = got
            if got != expected:
                raise RuntimeError('round-trip size/MD5/SHA256 mismatch')
            check_health(n)
        if args.duplex:
            uploaded = remote_path + '.duplex'
            downloaded = args.output / 'duplex-roundtrip.bin'
            with ThreadPoolExecutor(max_workers=2) as pool:
                up = pool.submit(run, 'duplex-upload', ['scp', '-O', '-q', *options,
                                 str(payload), f'{remote}:{uploaded}'])
                down = pool.submit(run, 'duplex-download', ['scp', '-O', '-q', *options,
                                   f'{remote}:{remote_path}', str(downloaded)])
                up.result()
                down_step = down.result()
            got = hashes(downloaded)
            down_step['hashes'] = got
            if got != expected:
                raise RuntimeError('duplex round-trip size/MD5/SHA256 mismatch')
            check = run('duplex-board-hash', ['ssh', '-n', *options, remote,
                                             f'/bin/md5 {uploaded}'])
            if expected['md5'] not in check['output'].lower().splitlines():
                raise RuntimeError('duplex board MD5 mismatch')
            check_health('duplex')
        report['passed'] = True
    except (RuntimeError, OSError) as e:
        report['error'] = str(e)
        print(str(e), flush=True)
    finally:
        (args.output / 'report.json').write_text(json.dumps(report, indent=2))
    print(json.dumps({'passed': report['passed'], 'expected': expected}), flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
