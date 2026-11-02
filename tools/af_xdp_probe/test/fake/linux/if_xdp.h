/* Subset of <linux/if_xdp.h> used by the probe. Values match the kernel UAPI. */
#pragma once
#include <stdint.h>

#define XDP_COPY		(1 << 1)
#define XDP_ZEROCOPY		(1 << 2)
#define XDP_USE_NEED_WAKEUP	(1 << 3)
#define XDP_STATISTICS		7
#define XDP_OPTIONS		8
#define XDP_OPTIONS_ZEROCOPY	(1 << 0)

struct xdp_options { uint32_t flags; };

struct xdp_statistics {
	uint64_t rx_dropped;
	uint64_t rx_invalid_descs;
	uint64_t tx_invalid_descs;
	uint64_t rx_ring_full;
	uint64_t rx_fill_ring_empty_descs;
	uint64_t tx_ring_empty_descs;
};

struct xdp_desc {
	unsigned long long addr;	/* __u64 on Linux, which is long long */
	uint32_t len;
	uint32_t options;
};
