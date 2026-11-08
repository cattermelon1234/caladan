/*
 * Fake <xdp/xsk.h>: the libxdp AF_XDP API surface the probe uses, implemented
 * by sim_kernel.c. Signatures match libxdp; the ring structs do not, since the
 * simulator keeps ring state itself.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <linux/if_xdp.h>

#define XSK_LIBXDP_FLAGS__INHIBIT_PROG_LOAD (1U << 0)

struct xsk_umem;
struct xsk_socket;
struct xsk_ring_prod { int unused; };
struct xsk_ring_cons { int unused; };

struct xsk_umem_config {
	uint32_t fill_size, comp_size, frame_size, frame_headroom, flags;
};
struct xsk_socket_config {
	uint32_t rx_size, tx_size, libxdp_flags, xdp_flags;
	uint16_t bind_flags;
};

int xsk_umem__create(struct xsk_umem **umem, void *area, uint64_t size,
		     struct xsk_ring_prod *fill, struct xsk_ring_cons *comp,
		     const struct xsk_umem_config *config);
int xsk_umem__delete(struct xsk_umem *umem);
int xsk_socket__create(struct xsk_socket **xsk, const char *ifname,
		       uint32_t queue_id, struct xsk_umem *umem,
		       struct xsk_ring_cons *rx, struct xsk_ring_prod *tx,
		       const struct xsk_socket_config *config);
void xsk_socket__delete(struct xsk_socket *xsk);
int xsk_socket__fd(const struct xsk_socket *xsk);

size_t xsk_ring_prod__reserve(struct xsk_ring_prod *prod, size_t nb, uint32_t *idx);
uint64_t *xsk_ring_prod__fill_addr(struct xsk_ring_prod *fill, uint32_t idx);
void xsk_ring_prod__submit(struct xsk_ring_prod *prod, size_t nb);
int xsk_ring_prod__needs_wakeup(const struct xsk_ring_prod *prod);

size_t xsk_ring_cons__peek(struct xsk_ring_cons *cons, size_t nb, uint32_t *idx);
const struct xdp_desc *xsk_ring_cons__rx_desc(const struct xsk_ring_cons *rx, uint32_t idx);
void xsk_ring_cons__release(struct xsk_ring_cons *cons, size_t nb);

void *xsk_umem__get_data(void *umem_area, uint64_t addr);
