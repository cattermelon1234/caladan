/* AF_XDP resources and their initialization/cleanup interface. */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <xdp/xsk.h>

/*
 * UMEM geometry. Half of the frames are published for receive; the other
 * half are reserved for future transmit work.
 */
#define AF_XDP_RING_SIZE	2048U
#define AF_XDP_FRAME_SIZE	4096U
#define AF_XDP_FRAME_COUNT	(2U * AF_XDP_RING_SIZE)
#define AF_XDP_FRAME_HEADROOM	128U
#define AF_XDP_UMEM_SIZE	((size_t)AF_XDP_FRAME_SIZE * AF_XDP_FRAME_COUNT)

struct probe_options;

struct af_xdp {
	void *buffer;
	struct xsk_umem *umem;
	struct xsk_socket *socket;
	struct xsk_ring_prod fill;
	struct xsk_ring_cons completion;
	struct xsk_ring_cons rx;
	struct xsk_ring_prod tx;
};

/*
 * Pass zero-initialized state. Cleanup is required even if init fails.
 * Init registers UMEM, binds the socket, and attaches libxdp's default
 * redirect program. It does not touch the Fill ring; see rx_loop.h.
 */
int af_xdp_init(struct af_xdp *io, const struct probe_options *opts);
int af_xdp_cleanup(struct af_xdp *io);
int af_xdp_fd(const struct af_xdp *io);
void af_xdp_print_config(const struct probe_options *opts);
void af_xdp_print_kernel_stats(const struct af_xdp *io);
