/*
 * af_xdp.c - UMEM allocation, socket/ring setup, redirect program attachment,
 * and resource cleanup
 */

#include <errno.h>
#include <inttypes.h>
#include <net/if.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>

#include <linux/if_link.h>
#include <linux/if_xdp.h>

#include "af_xdp.h"
#include "options.h"

#ifndef SOL_XDP
#define SOL_XDP 283
#endif

static void raise_memlock_limit(void)
{
	const struct rlimit limit = {RLIM_INFINITY, RLIM_INFINITY};

	/* Older kernels charge UMEM against this limit; newer ones may not. */
	if (setrlimit(RLIMIT_MEMLOCK, &limit))
		fprintf(stderr, "Could not raise locked-memory limit: %s; "
			"continuing with the current limit\n", strerror(errno));
}

static int allocate_umem(struct af_xdp *p)
{
	const struct xsk_umem_config config = {
		.fill_size = AF_XDP_RING_SIZE,
		.comp_size = AF_XDP_RING_SIZE,
		.frame_size = AF_XDP_FRAME_SIZE,
		.frame_headroom = AF_XDP_FRAME_HEADROOM,
		.flags = 0,
	};

	p->buffer = mmap(NULL, AF_XDP_UMEM_SIZE, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	if (p->buffer == MAP_FAILED) {
		p->buffer = NULL;
		return -errno;
	}

	return xsk_umem__create(&p->umem, p->buffer, AF_XDP_UMEM_SIZE,
				&p->fill, &p->completion, &config);
}

static int create_socket(struct af_xdp *p, const struct probe_options *opts)
{
	const struct xsk_socket_config config = {
		.rx_size = AF_XDP_RING_SIZE,
		.tx_size = AF_XDP_RING_SIZE,
		/*
		 * Let libxdp attach its default program, which redirects every
		 * packet on a queue that has a socket in the XSKMAP and passes
		 * the rest to the kernel. The socket is added to the map for
		 * @queue_id automatically and removed again on delete.
		 */
		.libxdp_flags = 0,
		/* Native (driver) mode only; no fallback to generic XDP. */
		.xdp_flags = XDP_FLAGS_DRV_MODE,
		.bind_flags = XDP_USE_NEED_WAKEUP |
			(opts->zero_copy ? XDP_ZEROCOPY : XDP_COPY),
	};

	return xsk_socket__create(&p->socket, opts->ifname, opts->queue_id,
				  p->umem, &p->rx, &p->tx, &config);
}

static int verify_mode(struct af_xdp *p, const struct probe_options *opts)
{
	struct xdp_options actual = {0};
	socklen_t len = sizeof(actual);
	bool zero_copy;

	if (getsockopt(xsk_socket__fd(p->socket), SOL_XDP, XDP_OPTIONS,
		       &actual, &len))
		return -errno;
	if (len != sizeof(actual))
		return -EIO;

	zero_copy = (actual.flags & XDP_OPTIONS_ZEROCOPY) != 0;
	return zero_copy == opts->zero_copy ? 0 : -EIO;
}

int af_xdp_cleanup(struct af_xdp *p)
{
	int ret;

	/*
	 * Deleting the socket removes its XSKMAP entry and, since this socket
	 * caused the program load, detaches the redirect program. The socket
	 * must release its UMEM reference before deleting the UMEM.
	 */
	if (p->socket) {
		xsk_socket__delete(p->socket);
		p->socket = NULL;
	}
	if (p->umem) {
		ret = xsk_umem__delete(p->umem);
		if (ret)
			return ret;
		p->umem = NULL;
	}
	if (p->buffer) {
		if (munmap(p->buffer, AF_XDP_UMEM_SIZE))
			return -errno;
		p->buffer = NULL;
	}
	return 0;
}

int af_xdp_init(struct af_xdp *p, const struct probe_options *opts)
{
	const char *stage;
	int ret;

	if (!if_nametoindex(opts->ifname)) {
		ret = -errno;
		fprintf(stderr, "Cannot find interface %s: %s\n", opts->ifname,
			strerror(-ret));
		return ret;
	}

	raise_memlock_limit();
	stage = "Allocate/register UMEM and configure Fill/Completion rings";
	ret = allocate_umem(p);
	if (ret)
		goto fail;

	stage = "Create/bind socket, configure RX/TX rings, attach redirect program";
	ret = create_socket(p, opts);
	if (ret)
		goto fail;

	stage = "Verify selected AF_XDP mode";
	ret = verify_mode(p, opts);
	if (ret)
		goto fail;

	return 0;

fail:
	fprintf(stderr, "%s: %s\n", stage, strerror(-ret));
	return ret;
}

int af_xdp_fd(const struct af_xdp *p)
{
	return xsk_socket__fd(p->socket);
}

void af_xdp_print_config(const struct probe_options *opts)
{
	printf("AF_XDP initialization succeeded\n"
	       "  interface: %s\n"
	       "  queue: %" PRIu32 "\n"
	       "  verified mode: %s\n"
	       "  UMEM: %zu bytes, %u frames of %u bytes\n"
	       "  frame headroom: %u bytes\n"
	       "  rings: Fill/RX/TX/Completion = %u descriptors each\n"
	       "  XDP program: libxdp default redirect, native mode\n",
	       opts->ifname, opts->queue_id, opts->zero_copy ? "zero-copy" : "copy",
	       AF_XDP_UMEM_SIZE, AF_XDP_FRAME_COUNT, AF_XDP_FRAME_SIZE,
	       AF_XDP_FRAME_HEADROOM, AF_XDP_RING_SIZE);
	fflush(stdout);
}

void af_xdp_print_kernel_stats(const struct af_xdp *p)
{
	struct xdp_statistics stats = {0};
	socklen_t len = sizeof(stats);

	if (!p->socket)
		return;
	if (getsockopt(xsk_socket__fd(p->socket), SOL_XDP, XDP_STATISTICS,
		       &stats, &len)) {
		fprintf(stderr, "Kernel socket statistics unavailable: %s\n",
			strerror(errno));
		return;
	}

	printf("Kernel socket statistics\n"
	       "  rx_dropped: %" PRIu64 "\n"
	       "  rx_invalid_descs: %" PRIu64 "\n"
	       "  tx_invalid_descs: %" PRIu64 "\n",
	       (uint64_t)stats.rx_dropped, (uint64_t)stats.rx_invalid_descs,
	       (uint64_t)stats.tx_invalid_descs);
	/* The remaining counters exist on Linux 5.9 and later. */
	if (len >= sizeof(stats))
		printf("  rx_ring_full: %" PRIu64 "\n"
		       "  rx_fill_ring_empty_descs: %" PRIu64 "\n"
		       "  tx_ring_empty_descs: %" PRIu64 "\n",
		       (uint64_t)stats.rx_ring_full,
		       (uint64_t)stats.rx_fill_ring_empty_descs,
		       (uint64_t)stats.tx_ring_empty_descs);
	fflush(stdout);
}
