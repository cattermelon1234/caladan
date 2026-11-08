# AF_XDP receive probe

A standalone tool that receives packets on one NIC queue through AF_XDP and
proves that frame ownership is tracked correctly. It is the first milestone of
the Caladan AF_XDP backend: it does not transmit and does not talk to a Caladan
runtime, but it exercises the exact receive and recycle path the IOKernel will
need.

What it does:

1. Allocates 16 MiB of UMEM (4,096 frames of 4,096 bytes, 128 bytes headroom)
   and registers it with the kernel.
2. Creates one AF_XDP socket bound to the chosen interface and queue, with
   Fill, RX, TX, and Completion rings of 2,048 descriptors each.
3. Lets libxdp attach its default XDP redirect program in native driver mode
   and add the socket to the XSKMAP for that queue. Packets on other queues
   pass to the kernel as usual.
4. Verifies the socket ended up in the requested copy or zero-copy mode.
5. Publishes 2,048 frames on the Fill ring, then polls the RX ring, inspects
   each packet, and recycles its frame. A per-frame ownership ledger catches a
   frame delivered while not owned by the kernel, published twice, or lost.
6. On SIGINT, SIGTERM, or a packet count limit, reports whether ownership
   reconciled and releases everything, including the XDP program.

## Files

| File | Responsibility |
| --- | --- |
| `main.c` | Option parsing, signal handling, top-level flow |
| `af_xdp.c`, `af_xdp.h` | UMEM, socket, rings, program attach, cleanup, kernel stats |
| `rx_loop.c`, `rx_loop.h` | Fill ring publication, RX polling, recycling, ledger |
| `options.h` | Parsed options shared by the above |

The receive loop owns all four rings. Nothing else touches them.

## Building

Linux only. Needs a C compiler, `make`, `pkg-config`, and libxdp with its
headers. Ubuntu 23.04 and later package it as `libxdp-dev`. Ubuntu 22.04 (the
CloudLab xl170 image) does not, so build it from xdp-tools, which bundles a
compatible libbpf:

```sh
cloudlab/cl.sh deps node0                       # clang, llvm, libelf-dev, ...
cloudlab/cl.sh run node0 caladan-ae/tools/af_xdp_probe/cloudlab/install_libxdp.sh
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig make -C tools/af_xdp_probe
tools/af_xdp_probe/af_xdp_probe --help
```

The Makefile refuses to build on macOS and when pkg-config cannot find libxdp.

## Running

```sh
sudo tools/af_xdp_probe/af_xdp_probe --interface IFACE --queue 0 --mode zero-copy
```

| Option | Meaning |
| --- | --- |
| `-i`, `--interface` | Experiment interface (required). Not the management interface. |
| `-q`, `--queue` | Hardware RX queue to bind (default 0) |
| `-m`, `--mode` | `copy` (default) or `zero-copy`; requested without fallback and verified after bind |
| `-c`, `--count` | Exit successfully after this many packets (default: run until signal) |
| `-s`, `--stats-interval` | Seconds between stats lines (default 1; 0 disables) |

Do not run this while Caladan's DPDK IOKernel is up on the same NIC. On
Mellanox NICs DPDK installs flow steering rules that pull traffic away from
the kernel queues this probe binds to.

### Steering traffic to the socket

The socket only sees packets that the NIC delivers to its queue. How that
happens depends on the driver and kernel. On the xl170 (mlx5_core, Linux
5.15) two things are needed, both verified on 2026-11-02:

**Zero-copy queue IDs are offset by the channel count.** Before Linux 5.19,
mlx5 exposes a separate receive queue for zero-copy on each channel, with
queue ID `num_channels + channel`. With 20 channels, channel 0's zero-copy
socket binds to queue 20. Binding zero-copy to queue 0 fails with EINVAL.
Copy mode binds to the regular queue ID.

**RSS never feeds the zero-copy queue.** Hashed traffic lands on the regular
queue, where the redirect program finds no socket and passes it to the
kernel. An ntuple rule is required to steer traffic into the zero-copy queue:

```sh
sudo ethtool -K IFACE ntuple on
sudo ethtool -N IFACE flow-type ether dst <IFACE's MAC> action 20
sudo ethtool -n IFACE                 # list rules; delete with -N IFACE delete <id>
```

This is hardware steering by destination MAC to a queue, the same mechanism
the per-runtime demultiplexing design needs, and it works on this NIC.

Hashed traffic can also be collapsed to one channel for copy-mode tests:

```sh
sudo ethtool -X IFACE equal 1         # RSS indirection -> channel 0 only
sudo ethtool -X IFACE default         # restore
```

### Other things observed on the xl170

- libxdp 1.4.2's multi-program dispatcher fails to load on 5.15 ("prog
  'xdp_pass': BPF program load failed: Invalid argument"). libxdp falls back
  to attaching the redirect program directly; the warnings are harmless and
  the program is attached and detached correctly.
- Sending SIGINT to a `sudo` wrapper does not reach the probe. Signal the
  probe's own PID, or run it as `sudo timeout -s INT N ./af_xdp_probe ...`.
- `tx_ring_empty_descs` in the kernel statistics counts the kernel checking
  the TX ring on each poll; it is not an error.

## Validation on the Linux host

Verified on CloudLab xl170 node0 (ConnectX-4 Lx, mlx5_core, Ubuntu 22.04,
Linux 5.15.0-198, libxdp 1.4.2) on 2026-11-02:

- Bad options, bad interface, and the wrong queue for zero-copy exit 1 with
  a stage-specific message. Nothing is left attached after a failed bind.
- Copy mode binds on queue 0; zero-copy binds on queue 20 and the mode check
  confirms it. `ip link show` reports `xdp` while live and nothing after exit.
- With a MAC steering rule and a UDP sender on node1 (`cloudlab/udp_blast.py`):
  2,717,580 datagrams sent, 2,717,580 received in zero-copy mode at up to
  544k pps, every one classified IPv4/UDP, `rx_dropped` 0, `rx_ring_full` 0,
  ownership balanced at exit (free=2048 kernel=2048 held=0). That is 660x the
  frame count with no starvation, and traffic started several seconds after
  the probe did.
- SIGINT during polling and during idle both exit 0 with everything released.
  An immediate rebind on the same queue succeeds.

Not yet exercised on hardware: the wakeup path under load (the Fill ring
never ran dry, so only one kick was needed), transmit, and the TCP ethertype
counters (only UDP was generated).

## Tests

```sh
make -C tools/af_xdp_probe test
```

Builds the probe sources against a simulated kernel (`test/sim_kernel.c`
behind a fake `xdp/xsk.h`) and runs `test/run_tests.sh`. Works on macOS and
Linux, needs no libxdp and no root, and finishes in a few seconds. The code
under test is the shipping probe; only the kernel boundary is faked.

The suite covers option validation, mode verification against the socket,
100k packets through 4,096 frames with the ledger balanced, packet
classification, the need-wakeup kick, SIGINT cleanup, periodic stats, and
fault detection: a frame delivered twice, a descriptor outside UMEM, and a
kernel that stops delivering. Fault injection knobs are documented at the top
of `test/sim_kernel.c`.

The simulator models the ring ownership protocol only. It cannot tell you
whether the driver binds, whether zero-copy works, or whether cleanup detaches
the XDP program. Those need the Linux host.

## Next milestones

Standalone transmit and Completion ring reclamation, then connecting the
working packet path to the IOKernel's shared ingress region and LRPC queues.
See the backend design doc in the repository history for the integration plan.

References: the [libxdp AF_XDP API](https://github.com/xdp-project/xdp-tools/blob/main/headers/xdp/xsk.h)
and the [Linux AF_XDP documentation](https://docs.kernel.org/networking/af_xdp.html).
