# AF_XDP migration to current Caladan

## Source and branch

The `af-xdp-upstream` branch starts from `shenango/caladan` commit
`bdb4ddebc2198cd95991079ed20e44a5f210d6d3` and carries the standalone probe
from `cattermelon1234/caladan-ae` commit
`188f404563aa95f0f4502368f336d590cedaa164` (`af-xdp-impl`).

The branch preserves both histories through a merge. The existing GitHub
repository can keep its name and branches; no force push is required. The
original `main` and `af-xdp-impl` branches remain available. The separate
local checkout is `../caladan-current`, and the CloudLab checkouts are
`~/caladan-current` on hp077 and hp079.

Probe C sources, headers, and simulator tests are carried over unchanged.
Generated binaries and macOS `.dSYM` directories are ignored. CloudLab helper
defaults now use `caladan-current`. The compatibility fixes made to the older
`caladan-ae` checkout's build scripts and kernel module are not imported.

The probe remains a standalone receive program. It does not send packets or
deliver them to a Caladan runtime.

## Verified on 2026-11-07

Both nodes use Ubuntu 22.04, Linux `5.15.0-198-generic`, GCC 11.4,
ConnectX-4 Lx firmware `14.18.2030`, and experiment interface `ens1f1np1`.
Its PCI address is `0000:03:00.1`. Management traffic uses a different NIC.

| Check | Result |
| --- | --- |
| Probe simulator on macOS | 19 passed, 0 failed |
| Upstream dependencies, IOKernel, runtime test binaries, and ksched | Built on hp077 and hp079 |
| Native probe build and simulator on both nodes | Built; 19 passed, 0 failed on each |
| DPDK IOKernel on hp077's physical NIC | Initialized successfully with the verbs capability patch below |
| `tests/test_tcp_read server.config` on hp077 | Passed; exercises TCP through the runtime and local IOKernel delivery |
| hp079 ping to hp077's Caladan runtime at `192.168.1.3` | 3 sent, 3 received; verifies physical NIC RX/TX through the IOKernel and runtime |
| AF_XDP copy mode on hp079, queue 0 | 50,014 packets; 50,011 UDP and 3 ARP |
| AF_XDP zero-copy mode on hp079, queue 20 | 50,027 UDP packets; socket mode verified |

Both AF_XDP runs ended with `free=2048 kernel=2048 held=0`, zero ledger
errors, zero invalid descriptors, zero fill failures, and zero socket drops.
Traffic was sent from hp077 with `cloudlab/udp_blast.py`. The probe stops at
the packet limit while the sender can continue; sender and receiver totals
are not a packet-loss measurement. These are functional checks, not performance
benchmarks. RSS, temporary flow rules, and MTU were restored after testing.

Logs are in `/tmp/caladan-current-*.log` on the tested nodes. Important files
include `deps`, `build`, `ksched`, `iokernel`, `tcp-read`, `runtime-ping`,
`af-copy`, and `af-zero-copy` logs. `/tmp` files are lost when nodes reboot.

## Building the upstream baseline on these nodes

On a fresh Linux checkout, install upstream's prerequisites plus the linker
and probe dependencies needed by this Ubuntu image:

```sh
sudo apt-get update
sudo apt-get install -y \
  build-essential git cmake meson ninja-build pkg-config \
  libnl-3-dev libnl-route-3-dev libnuma-dev uuid-dev libssl-dev \
  libaio-dev libcunit1-dev libclang-dev libncurses-dev \
  python3-pyelftools libarchive-dev nettle-dev libacl1-dev \
  liblzma-dev libzstd-dev liblz4-dev libbz2-dev zlib1g-dev libxml2-dev \
  clang llvm libelf-dev libpcap-dev m4 ethtool
```

GCC 11 needs upstream's existing option to omit UINTR instructions:

```sh
cd ~/caladan-current
sed -i 's/^CONFIG_NO_UINTR=n$/CONFIG_NO_UINTR=y/' build/config
make submodules
make -j8 iokerneld tests/test_ping tests/test_tcp_read
(cd ksched && make -j8)
sudo ./scripts/setup_machine.sh nouintr
```

`make submodules` is the initial dependency setup command. Upstream applies
patches with `git am`; rerunning it in an already patched checkout is not
an incremental rebuild. The two prepared CloudLab checkouts already have
their dependencies built.

### Mellanox verbs compatibility patch

`build/patches/dpdk/0006-mlx5-verbs-wq-limit.patch` is applied by upstream's
existing dependency script on a fresh build. It fixes the DPDK 25.03 verbs
fallback on these NICs when DevX is unavailable.

Without it, an unpopulated `log_max_wq_sz` is interpreted as a maximum queue
size of one. DPDK clamps the requested rings to one descriptor and then fails
to create the RX completion queue. The patch derives a conservative
power-of-two limit from the verbs-reported `max_qp_wr`. It leaves the DevX
path unchanged and covers both descriptor adjustment and RX queue validation.

### Physical NIC baseline check

On hp077, keep the IOKernel running in one terminal:

```sh
cd ~/caladan-current
sudo ./iokerneld simple nobw nicpci 0000:03:00.1
```

In another hp077 terminal:

```sh
cd ~/caladan-current
sudo ./tests/test_tcp_read server.config
sudo ./tests/test_ping server.config
```

The supplied `server.config` uses runtime IP `192.168.1.3` and does not enable
directpath, so packets use the DPDK IOKernel path. While `test_ping` runs for
about ten seconds, run this on hp079:

```sh
ping -I ens1f1np1 -c3 -W2 192.168.1.3
```

Stop the IOKernel before running the AF_XDP tests on the same NIC.

## Standalone AF_XDP hardware checks

The prepared nodes have libxdp 1.4.2 installed. For a fresh node, run the
installer after installing the prerequisites above:

```sh
cd ~/caladan-current
./tools/af_xdp_probe/cloudlab/install_libxdp.sh
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig make -C tools/af_xdp_probe
make -C tools/af_xdp_probe test
```

For the following checks, hp079 receives at Linux IP `192.168.1.2` and hp077
sends. These nodes have twenty channels, a default round-robin RSS table,
and no pre-existing ntuple rule at location 100. Check `ethtool -l`, `-x`,
and `-n` before reusing these commands on another machine.

First ping `192.168.1.2` from hp077 to populate its neighbor entry. On hp079,
save the existing MTU, then use an MTU compatible with the probe's 4096-byte
UMEM frames. Leaving the NIC at MTU 9000 causes XDP attachment to fail here.

```sh
probe_saved_mtu=$(cat /sys/class/net/ens1f1np1/mtu)
sudo ip link set dev ens1f1np1 mtu 1500
sudo ethtool -X ens1f1np1 equal 1
sudo ./tools/af_xdp_probe/af_xdp_probe \
  --interface ens1f1np1 --queue 0 --mode copy --count 50000
```

While the probe polls, run on hp077:

```sh
python3 ~/caladan-current/tools/af_xdp_probe/cloudlab/udp_blast.py \
  192.168.1.2 --seconds 2
```

After copy mode exits, restore RSS on hp079:

```sh
sudo ethtool -X ens1f1np1 default
```

For zero-copy, keep MTU 1500. This mlx5 driver exposes channel 0's zero-copy
queue as `20 + 0`, so bind queue 20 and steer by the receiving interface's
MAC. Record the original ntuple feature state before changing it.

```sh
sudo ethtool -K ens1f1np1 ntuple on
sudo ethtool -N ens1f1np1 flow-type ether \
  dst 9c:dc:71:5b:22:71 action 20 loc 100
sudo ./tools/af_xdp_probe/af_xdp_probe \
  --interface ens1f1np1 --queue 20 --mode zero-copy --count 50000
```

Send the same UDP burst from hp077. After exit, remove the test rule and
restore the saved MTU and the original ntuple feature state:

```sh
sudo ethtool -N ens1f1np1 delete 100
sudo ethtool -X ens1f1np1 default
sudo ip link set dev ens1f1np1 mtu "$probe_saved_mtu"
# If ntuple was originally off: sudo ethtool -K ens1f1np1 ntuple off
ip -details link show ens1f1np1
```

The probe should report balanced frame ownership and zero errors. The final
interface output should show no attached XDP program. libxdp may print a
dispatcher loading warning on this kernel before successfully attaching its
redirect program directly; check the probe's final initialization result.

## Remaining integration work in current upstream

The original project's high-level architecture still applies, but several
details have changed. Use this checkout's source for the next implementation:

| Area | Current behavior and AF_XDP integration needed |
| --- | --- |
| `iokernel/main.c` | Select backend initialization and polling while retaining scheduler and runtime control handling. |
| `iokernel/rx.c` | DPDK uses a hardware flow mark or destination IP lookup to select a runtime; `rx_send_to_runtime()` selects a kthread through `flow_tbl` and the flow hash. Preserve this routing policy with backend-neutral packet access. |
| `inc/iokernel/queue.h` | RX metadata is packed in `union rxq_cmd`; the LRPC payload is a shared ingress offset. Do not transplant the older `rx_net_hdr` layout. |
| `iokernel/control.c` | Make the shared ingress region usable for AF_XDP UMEM or a staging allocator. A private probe UMEM address cannot be handed directly to another process. |
| `runtime/net/core.c` | `net_rx_alloc_mbuf()` copies bytes from the shared ingress offset and returns that same offset using `TXCMD_NET_COMPLETE`. TCP/UDP lookup remains in the runtime. |
| `iokernel/commands.c` | Validate ownership and return completed RX buffers to the selected backend. Existing completion handling reconstructs and frees a DPDK mbuf. |
| `iokernel/tx.c` | Drain runtime TX queues into AF_XDP frames, calculate required checksums, submit TX, reclaim Completion descriptors, and preserve runtime TX completion semantics. |
| `iokernel/dp_clients.c` | Retain runtime registration and disconnect handling; reclaim outstanding backend buffers when a runtime exits. |

Start with one queue and an explicit ownership model. The next packet-path
milestone is standalone AF_XDP TX and completion reclamation, followed by a
backend adapter for shared ingress delivery and runtime TX. None of that
runtime integration is implemented by this migration.
