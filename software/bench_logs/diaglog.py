#!/usr/bin/env python3
"""diaglog.py — single-owner diag UART logger + command injector.

Usage: diaglog.py <dev> <logfile> [--cmd X --after SECS]...

Owns the port exclusively for its lifetime. Appends all RX bytes to <logfile>.
Optional --cmd/--after pairs send single command characters at scheduled
times (e.g. 'p' for the PIO/timing dump). Runs until killed or --duration.
"""
import argparse
import sys
import time

import serial

p = argparse.ArgumentParser()
p.add_argument("dev")
p.add_argument("logfile")
p.add_argument("--duration", type=float, default=0, help="exit after N s (0 = forever)")
p.add_argument("--cmd", action="append", default=[], help="command char to send")
p.add_argument("--after", action="append", type=float, default=[], help="seconds after start to send matching --cmd")
args = p.parse_args()

sends = sorted(zip(args.after, args.cmd))
s = serial.Serial(args.dev, 230400, timeout=0.2)
start = time.time()
with open(args.logfile, "ab", buffering=0) as f:
    f.write(f"=== diaglog start {time.strftime('%Y-%m-%d %H:%M:%S')} ===\n".encode())
    while True:
        d = s.read(4096)
        if d:
            f.write(d)
        now = time.time() - start
        while sends and now >= sends[0][0]:
            _, ch = sends.pop(0)
            s.write(ch[:1].encode())
            f.write(f"\n=== diaglog sent '{ch[:1]}' @ {now:.1f}s ===\n".encode())
        if args.duration and now >= args.duration:
            break
    f.write(f"=== diaglog end @ {time.time()-start:.1f}s ===\n".encode())
