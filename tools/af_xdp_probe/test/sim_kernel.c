/*
 * sim_kernel.c - a simulated AF_XDP kernel behind the fake libxdp API
 *
 * Models the ownership protocol only: frames the application publishes on
 * the Fill ring are consumed, a fabricated IPv4 packet is written into each,
 * and a descriptor is delivered on the RX ring. There is no real device.
 *
 * Fault injection via the environment, all off by default:
 *   SIM_MODE=copy|zero-copy  mode the socket reports (default zero-copy)
 *   SIM_IDLE=k               every k-th RX peek delivers nothing
 *   SIM_WAKE=k               every k-th needs_wakeup check answers yes
 *   SIM_DUP=k                every k-th packet's frame is delivered twice
 *   SIM_BAD=k                after every k-th packet, deliver an address
 *                            outside UMEM
 *   SIM_HOLD=n               stop consuming the Fill ring after n packets,
 *                            so it fills up and reservations fail
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <xdp/xsk.h>

#define RING		2048U
#define MASK		(RING - 1)
#define DATA_OFFSET	(256 + 128)	/* XDP_PACKET_HEADROOM + frame headroom */

static struct {
	uint64_t fill[RING];
	uint32_t fill_prod, fill_cons;
	struct xdp_desc rx[RING];
	uint32_t rx_prod, rx_cons;
	void *umem;
	uint64_t delivered;
	int zero_copy, idle_every, wake_every, dup_every, bad_every;
	long hold_after;
} S;

static int env_int(const char *name)
{
	const char *v = getenv(name);

	return v ? atoi(v) : 0;
}

static int dummy;

int xsk_umem__create(struct xsk_umem **umem, void *area, uint64_t size,
		     struct xsk_ring_prod *fill, struct xsk_ring_cons *comp,
		     const struct xsk_umem_config *config)
{
	const char *mode = getenv("SIM_MODE");

	(void)size; (void)fill; (void)comp; (void)config;
	memset(&S, 0, sizeof(S));
	S.umem = area;
	S.zero_copy = !mode || strcmp(mode, "copy") != 0;
	S.idle_every = env_int("SIM_IDLE");
	S.wake_every = env_int("SIM_WAKE");
	S.dup_every = env_int("SIM_DUP");
	S.bad_every = env_int("SIM_BAD");
	S.hold_after = getenv("SIM_HOLD") ? atol(getenv("SIM_HOLD")) : -1;
	*umem = (struct xsk_umem *)&dummy;
	return 0;
}

int xsk_umem__delete(struct xsk_umem *umem)
{
	(void)umem;
	return 0;
}

int xsk_socket__create(struct xsk_socket **xsk, const char *ifname,
		       uint32_t queue_id, struct xsk_umem *umem,
		       struct xsk_ring_cons *rx, struct xsk_ring_prod *tx,
		       const struct xsk_socket_config *config)
{
	(void)ifname; (void)queue_id; (void)umem; (void)rx; (void)tx;
	(void)config;
	*xsk = (struct xsk_socket *)&dummy;
	return 0;
}

void xsk_socket__delete(struct xsk_socket *xsk)
{
	(void)xsk;
}

int xsk_socket__fd(const struct xsk_socket *xsk)
{
	(void)xsk;
	return 99;	/* not a real descriptor; recvfrom on it fails */
}

/* Fill ring: application is the producer. */

size_t xsk_ring_prod__reserve(struct xsk_ring_prod *prod, size_t nb,
			      uint32_t *idx)
{
	(void)prod;
	if (S.fill_prod - S.fill_cons + nb > RING)
		return 0;
	*idx = S.fill_prod;
	return nb;
}

uint64_t *xsk_ring_prod__fill_addr(struct xsk_ring_prod *fill, uint32_t idx)
{
	(void)fill;
	return &S.fill[idx & MASK];
}

void xsk_ring_prod__submit(struct xsk_ring_prod *prod, size_t nb)
{
	(void)prod;
	S.fill_prod += nb;
}

int xsk_ring_prod__needs_wakeup(const struct xsk_ring_prod *prod)
{
	static unsigned int checks;

	(void)prod;
	return S.wake_every && ++checks % S.wake_every == 0;
}

/* RX ring: the simulated kernel is the producer. */

static void fabricate_packet(uint64_t addr, uint32_t len, uint64_t seq)
{
	uint8_t *d = (uint8_t *)S.umem + addr;

	memset(d, 0, len);
	d[12] = 0x08; d[13] = 0x00;		/* ethertype IPv4 */
	d[14] = 0x45;				/* IPv4, IHL 5 */
	d[23] = seq % 3 == 0 ? 17 : 6;		/* every third UDP, else TCP */
}

static void deliver(uint64_t addr, uint32_t len)
{
	struct xdp_desc *d = &S.rx[S.rx_prod & MASK];

	d->addr = addr;
	d->len = len;
	d->options = 0;
	S.rx_prod++;
}

static int rx_has_room(void)
{
	return S.rx_prod - S.rx_cons < RING;
}

size_t xsk_ring_cons__peek(struct xsk_ring_cons *cons, size_t nb,
			   uint32_t *idx)
{
	static unsigned int peeks;
	size_t moved = 0, avail;

	(void)cons;
	if (S.idle_every && ++peeks % S.idle_every == 0) {
		*idx = S.rx_cons;
		return 0;
	}

	while (S.fill_cons != S.fill_prod && moved < nb && rx_has_room()) {
		uint64_t base;
		uint32_t len;

		if (S.hold_after >= 0 && S.delivered >= (uint64_t)S.hold_after)
			break;

		base = S.fill[S.fill_cons++ & MASK];
		S.delivered++;
		len = 60 + (uint32_t)(S.delivered * 37 % 1441);
		fabricate_packet(base + DATA_OFFSET, len, S.delivered);
		deliver(base + DATA_OFFSET, len);
		moved++;

		if (S.dup_every && S.delivered % S.dup_every == 0 &&
		    rx_has_room()) {
			deliver(base + DATA_OFFSET, len);
			moved++;
		}
		if (S.bad_every && S.delivered % S.bad_every == 0 &&
		    rx_has_room()) {
			deliver((uint64_t)1 << 40, 64);
			moved++;
		}
	}

	avail = S.rx_prod - S.rx_cons;
	if (avail > nb)
		avail = nb;
	*idx = S.rx_cons;
	return avail;
}

const struct xdp_desc *xsk_ring_cons__rx_desc(const struct xsk_ring_cons *rx,
					      uint32_t idx)
{
	(void)rx;
	return &S.rx[idx & MASK];
}

void xsk_ring_cons__release(struct xsk_ring_cons *cons, size_t nb)
{
	(void)cons;
	S.rx_cons += nb;
}

void *xsk_umem__get_data(void *umem_area, uint64_t addr)
{
	return (uint8_t *)umem_area + addr;
}

/* Socket options the probe queries. */

int sim_getsockopt(int fd, int level, int name, void *val, socklen_t *len)
{
	(void)fd; (void)level;
	if (name == XDP_OPTIONS) {
		struct xdp_options *o = val;

		o->flags = S.zero_copy ? XDP_OPTIONS_ZEROCOPY : 0;
		*len = sizeof(*o);
		return 0;
	}
	if (name == XDP_STATISTICS) {
		memset(val, 0, *len);
		return 0;
	}
	errno = ENOPROTOOPT;
	return -1;
}
