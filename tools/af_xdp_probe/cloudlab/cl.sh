#!/bin/sh
# Thin wrapper for working with the CloudLab nodes in hosts.env.
#
#   cl.sh run NODE CMD...     run a command on NODE (node0, node1, or a hostname)
#   cl.sh ssh NODE            interactive shell
#   cl.sh sync NODE           rsync tools/af_xdp_probe to the node
#   cl.sh iface NODE          print the interface holding the node's experiment IP
#   cl.sh info NODE           kernel, NIC, driver, queues, libxdp availability
#   cl.sh deps NODE           install distro prerequisites (libxdp itself: see
#                             install_libxdp.sh; jammy has no libxdp-dev)
#   cl.sh client NODE         install traffic-generation tools on a client node
#   cl.sh build NODE          sync, then make the probe and run its tests

set -eu
here=$(cd "$(dirname "$0")" && pwd)
env_file=$here/hosts.env
[ -f "$env_file" ] || { echo "Copy $here/hosts.env.example to hosts.env and fill it in." >&2; exit 1; }
. "$env_file"

resolve() {
	case "$1" in
	node0) echo "$NODE0" ;;
	node1) echo "$NODE1" ;;
	*) echo "$1" ;;
	esac
}
node_ip() {
	case "$1" in
	node0) echo "$NODE0_IP" ;;
	node1) echo "$NODE1_IP" ;;
	*) echo "" ;;
	esac
}

SSH="ssh -i $SSH_KEY -o StrictHostKeyChecking=accept-new -o ServerAliveInterval=30"

cmd=${1:?usage: cl.sh run|ssh|sync|iface|info|deps|build NODE [CMD...]}
node=${2:?missing NODE}
host=$(resolve "$node")
shift 2

case "$cmd" in
run)
	exec $SSH "$SSH_USER@$host" "$@"
	;;
ssh)
	exec $SSH "$SSH_USER@$host"
	;;
sync)
	# tar over ssh rather than rsync: macOS ships openrsync, which cannot
	# talk to the Ubuntu rsync. Replaces the remote copy wholesale.
	src=$(cd "$here/.." && pwd)
	COPYFILE_DISABLE=1 tar -C "$(dirname "$src")" --no-xattrs -czf - \
		--exclude af_xdp_probe/af_xdp_probe --exclude af_xdp_probe/test/probe_sim \
		--exclude af_xdp_probe/cloudlab/hosts.env --exclude '*.o' \
		"$(basename "$src")" |
	$SSH "$SSH_USER@$host" "mkdir -p $REMOTE_DIR/tools && rm -rf $REMOTE_DIR/tools/af_xdp_probe && tar -xzf - -C $REMOTE_DIR/tools"
	;;
iface)
	ip=$(node_ip "$node")
	[ -n "$EXP_IFACE" ] && { echo "$EXP_IFACE"; exit 0; }
	[ -n "$ip" ] || { echo "No experiment IP known for $node" >&2; exit 1; }
	exec $SSH "$SSH_USER@$host" "ip -br -4 addr | awk '\$3 ~ /^$ip\\// {print \$1}'"
	;;
info)
	ip=$(node_ip "$node")
	exec $SSH "$SSH_USER@$host" "
		echo '== kernel =='; uname -r
		echo '== interfaces =='; ip -br -4 addr
		iface=\$(ip -br -4 addr | awk '\$3 ~ /^$ip\\// {print \$1}')
		echo \"== experiment iface: \$iface ==\"
		[ -n \"\$iface\" ] && {
			echo '-- driver --'; sudo ethtool -i \$iface | egrep 'driver|version|bus-info'
			echo '-- queues --'; sudo ethtool -l \$iface | tail -n +6
			echo '-- ntuple/rxhash --'; sudo ethtool -k \$iface | egrep 'ntuple|rx-hashing'
			echo '-- xdp attached --'; ip link show \$iface | grep -o 'xdp[a-z]*' || echo none
			echo '-- pci --'; lspci | grep -i ethernet
		}
		echo '== libxdp =='; pkg-config --modversion libxdp 2>/dev/null || echo 'not installed'
		dpkg -l libxdp-dev libbpf-dev 2>/dev/null | grep '^ii' || echo 'dev packages not installed'
		echo '== memlock =='; ulimit -l
	"
	;;
deps)
	exec $SSH "$SSH_USER@$host" "sudo apt-get update -qq && sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq build-essential pkg-config git clang llvm libelf-dev zlib1g-dev libpcap-dev m4 ethtool"
	;;
client)
	exec $SSH "$SSH_USER@$host" "sudo apt-get update -qq && sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq iperf3 ethtool"
	;;
build)
	"$0" sync "$node"
	exec $SSH "$SSH_USER@$host" "cd $REMOTE_DIR/tools/af_xdp_probe && PKG_CONFIG_PATH=/usr/local/lib/pkgconfig make && make test"
	;;
*)
	echo "unknown command: $cmd" >&2; exit 1
	;;
esac
