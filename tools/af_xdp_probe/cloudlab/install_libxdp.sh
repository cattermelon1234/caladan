#!/bin/sh
# Build and install libxdp from xdp-tools on Ubuntu 22.04, which ships no
# libxdp-dev package. Installs to /usr/local. Run on the node as a user with
# sudo. Prerequisites (clang, llvm, libelf-dev, zlib1g-dev, m4, pkg-config)
# are installed by `cl.sh deps`.
#
# xdp-tools bundles a libbpf submodule; its configure script uses that when
# the system libbpf (0.5 on jammy) is too old. The default redirect program
# xsk_def_xdp_prog.o is built with clang and installed alongside the library.

set -eu
VERSION=${XDP_TOOLS_VERSION:-v1.4.2}
SRC=${XDP_TOOLS_SRC:-$HOME/xdp-tools}

if [ ! -d "$SRC" ]; then
	git clone --recurse-submodules --depth 1 -b "$VERSION" \
		https://github.com/xdp-project/xdp-tools "$SRC"
fi
cd "$SRC"
./configure
make -j"$(nproc)" libxdp
sudo make libxdp_install

# libxdp.pc declares Requires.private: libbpf, so pkg-config refuses to
# resolve libxdp unless a libbpf.pc exists. The bundled libbpf is linked into
# libxdp statically and never installed; install its headers, shared library,
# and .pc file next to libxdp so pkg-config and <bpf/libbpf.h> both resolve.
sudo make -C lib/libbpf/src PREFIX=/usr/local LIBDIR=/usr/local/lib install
sudo ldconfig

echo
echo "Installed:"
pkg-config --modversion libxdp libbpf
pkg-config --cflags --libs libxdp
find /usr/local/lib -name 'xsk_def_xdp_prog*.o'
echo
echo "Build the probe with:"
echo "  make -C tools/af_xdp_probe"
