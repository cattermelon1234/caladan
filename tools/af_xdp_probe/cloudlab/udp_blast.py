#!/usr/bin/env python3
"""Send UDP datagrams to a host as fast as possible; needs no receiver.

Used to drive the AF_XDP probe, which swallows everything on its queue, so a
tool that expects replies (iperf3) cannot be used. Prints the count sent.
"""
import argparse
import socket
import time

p = argparse.ArgumentParser()
p.add_argument("dst")
p.add_argument("--port", type=int, default=9000)
p.add_argument("--size", type=int, default=64, help="payload bytes")
p.add_argument("--seconds", type=float, default=5.0)
p.add_argument("--flows", type=int, default=1, help="distinct source ports")
a = p.parse_args()

socks = []
for i in range(a.flows):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("", 0))
    s.connect((a.dst, a.port))
    socks.append(s)

payload = b"x" * a.size
sent = 0
t0 = time.monotonic()
deadline = t0 + a.seconds
while time.monotonic() < deadline:
    for s in socks:
        try:
            s.send(payload)
            sent += 1
        except OSError:
            pass
elapsed = time.monotonic() - t0
print(f"sent {sent} datagrams of {a.size} bytes in {elapsed:.2f}s ({sent/elapsed:.0f} pps)")
