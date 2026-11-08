CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra
CPPFLAGS += -D_GNU_SOURCE

SRCS := main.c af_xdp.c rx_loop.c
HDRS := af_xdp.h options.h rx_loop.h

.PHONY: all check-deps test clean

all: af_xdp_probe

check-deps:
	@test "$$(uname -s)" = Linux || { echo "AF_XDP requires Linux." >&2; exit 1; }
	@command -v $(PKG_CONFIG) >/dev/null || { echo "Install pkg-config and libxdp development files." >&2; exit 1; }
	@$(PKG_CONFIG) --exists libxdp || { echo "pkg-config cannot find libxdp development files." >&2; exit 1; }

af_xdp_probe: $(SRCS) $(HDRS) | check-deps
	$(CC) $(CPPFLAGS) $(CFLAGS) $(shell $(PKG_CONFIG) --cflags libxdp 2>/dev/null) $(SRCS) $(LDFLAGS) $(shell $(PKG_CONFIG) --libs libxdp 2>/dev/null) $(LDLIBS) -o $@

# Build the probe against a simulated kernel (test/sim_kernel.c) and run the
# behavioural tests. Works on any host; needs no libxdp and no root.
TEST_CPPFLAGS := -D_GNU_SOURCE -Itest/fake -include test/fake/compat.h

test/probe_sim: $(SRCS) $(HDRS) test/sim_kernel.c $(wildcard test/fake/*.h test/fake/*/*.h)
	$(CC) $(TEST_CPPFLAGS) $(CFLAGS) $(SRCS) test/sim_kernel.c -o $@

test: test/probe_sim
	test/run_tests.sh test/probe_sim

clean:
	$(RM) af_xdp_probe test/probe_sim
