/*
 * rx_loop.c - receive packets, account for frame ownership, recycle frames
 *
 * Every UMEM frame is owned by exactly one party at a time. The ledger below
 * tracks that so a frame handed to the kernel twice, delivered while not
 * owned by the kernel, or never returned shows up as an error instead of as
 * silent corruption or starvation.
 *
 *   FREE   - owned by userspace, not published (the transmit half of UMEM)
 *   KERNEL - on the Fill ring or RX ring; the kernel may write into it
 *   HELD   - consumed from the RX ring, being inspected, not yet recycled
 *
 * This thread owns all four rings. Nothing else may touch them.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <net/ethernet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#include "af_xdp.h"
#include "options.h"
#include "rx_loop.h"

#define BATCH_SIZE	64U
#define MAX_REPORTED_ERRORS	10
#define FILL_RETRY_SECONDS	2.0

enum frame_state { FRAME_FREE = 0, FRAME_KERNEL, FRAME_HELD };

static const char *const state_names[] = {"free", "kernel", "held"};

struct ledger {
	uint8_t state[AF_XDP_FRAME_COUNT];
};

/* Returns false, and leaves the state alone, if the transition was invalid. */
static bool ledger_move(struct ledger *l, struct rx_stats *s, uint32_t frame,
			enum frame_state from, enum frame_state to,
			const char *event)
{
	if (l->state[frame] != from) {
		s->ledger_errors++;
		if (s->ledger_errors <= MAX_REPORTED_ERRORS)
			fprintf(stderr, "ledger: %s frame %" PRIu32
				" while %s (expected %s)\n", event, frame,
				state_names[l->state[frame]], state_names[from]);
		return false;
	}
	l->state[frame] = to;
	return true;
}

static double seconds_since(const struct timespec *then)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)(now.tv_sec - then->tv_sec) +
	       (double)(now.tv_nsec - then->tv_nsec) / 1e9;
}

static void kick_kernel(struct af_xdp *io, struct rx_stats *s)
{
	s->wakeups++;
	/*
	 * EBUSY, EAGAIN, ENOBUFS, and ENETDOWN are expected transient results
	 * per the kernel documentation; count anything else but keep going.
	 */
	if (recvfrom(af_xdp_fd(io), NULL, 0, MSG_DONTWAIT, NULL, NULL) &&
	    errno != EBUSY && errno != EAGAIN && errno != ENOBUFS &&
	    errno != ENETDOWN)
		s->wakeup_errors++;
}

/*
 * Hand @n frame base addresses to the kernel via the Fill ring. The ring has
 * as many slots as there are receive frames, so a reservation can only fail
 * transiently while the kernel is still consuming earlier entries.
 */
static int publish_frames(struct af_xdp *io, struct ledger *l,
			  struct rx_stats *s, const uint64_t *addrs,
			  uint32_t n, enum frame_state from,
			  const volatile sig_atomic_t *stop)
{
	struct timespec started = {0};
	uint32_t idx, i;

	while (xsk_ring_prod__reserve(&io->fill, n, &idx) != n) {
		s->fill_retries++;
		if (xsk_ring_prod__needs_wakeup(&io->fill))
			kick_kernel(io, s);
		if (!started.tv_sec)
			clock_gettime(CLOCK_MONOTONIC, &started);
		else if (*stop || seconds_since(&started) > FILL_RETRY_SECONDS)
			return -EAGAIN;
	}

	for (i = 0; i < n; i++) {
		*xsk_ring_prod__fill_addr(&io->fill, idx + i) = addrs[i];
		ledger_move(l, s, (uint32_t)(addrs[i] / AF_XDP_FRAME_SIZE),
			    from, FRAME_KERNEL, "published");
	}
	xsk_ring_prod__submit(&io->fill, n);
	return 0;
}

static int publish_initial_frames(struct af_xdp *io, struct ledger *l,
				  struct rx_stats *s,
				  const volatile sig_atomic_t *stop)
{
	uint64_t addrs[BATCH_SIZE];
	uint32_t frame = 0, i;
	int ret;

	while (frame < AF_XDP_RING_SIZE) {
		for (i = 0; i < BATCH_SIZE && frame < AF_XDP_RING_SIZE; i++)
			addrs[i] = (uint64_t)frame++ * AF_XDP_FRAME_SIZE;
		ret = publish_frames(io, l, s, addrs, i, FRAME_FREE, stop);
		if (ret)
			return ret;
	}
	return 0;
}

static void inspect_packet(const uint8_t *data, uint32_t len,
			   struct rx_stats *s)
{
	const struct ether_header *eth = (const struct ether_header *)data;
	const struct ip *ip4;
	uint16_t type;

	s->packets++;
	s->bytes += len;
	if (len < s->min_len)
		s->min_len = len;
	if (len > s->max_len)
		s->max_len = len;

	if (len < sizeof(*eth)) {
		s->runt++;
		return;
	}

	type = ntohs(eth->ether_type);
	if (type == ETHERTYPE_ARP) {
		s->arp++;
	} else if (type == ETHERTYPE_IPV6) {
		s->ipv6++;
	} else if (type == ETHERTYPE_IP) {
		s->ipv4++;
		if (len < sizeof(*eth) + sizeof(*ip4))
			return;
		ip4 = (const struct ip *)(data + sizeof(*eth));
		switch (ip4->ip_p) {
		case IPPROTO_TCP:
			s->tcp++;
			break;
		case IPPROTO_UDP:
			s->udp++;
			break;
		case IPPROTO_ICMP:
			s->icmp++;
			break;
		default:
			s->other_ipproto++;
		}
	} else {
		s->other_ethertype++;
	}
}

/*
 * Consume up to BATCH_SIZE descriptors, inspect each packet, and recycle
 * every frame. Returns the number of descriptors consumed.
 */
static uint32_t receive_batch(struct af_xdp *io, struct ledger *l,
			      struct rx_stats *s,
			      const volatile sig_atomic_t *stop)
{
	uint64_t recycle[BATCH_SIZE];
	uint32_t idx, received, i, n = 0;

	received = xsk_ring_cons__peek(&io->rx, BATCH_SIZE, &idx);
	if (!received) {
		if (xsk_ring_prod__needs_wakeup(&io->fill))
			kick_kernel(io, s);
		return 0;
	}
	s->batches++;

	for (i = 0; i < received; i++) {
		const struct xdp_desc *desc = xsk_ring_cons__rx_desc(&io->rx,
								     idx + i);
		uint64_t frame = desc->addr / AF_XDP_FRAME_SIZE;
		uint64_t offset = desc->addr % AF_XDP_FRAME_SIZE;

		if (frame >= AF_XDP_FRAME_COUNT) {
			/* Not ours; nothing we can safely recycle. */
			s->bad_desc++;
			if (s->bad_desc <= MAX_REPORTED_ERRORS)
				fprintf(stderr, "descriptor addr %" PRIu64
					" is outside UMEM\n", (uint64_t)desc->addr);
			continue;
		}

		/*
		 * A frame the kernel did not own (delivered twice, say) is not
		 * recycled: doing so would overfill the Fill ring and the
		 * ledger would lose track of it. It stays as it was and shows
		 * up at reconciliation.
		 */
		if (!ledger_move(l, s, (uint32_t)frame, FRAME_KERNEL, FRAME_HELD,
				 "received"))
			continue;

		if (offset + desc->len > AF_XDP_FRAME_SIZE) {
			s->bad_desc++;
			if (s->bad_desc <= MAX_REPORTED_ERRORS)
				fprintf(stderr, "descriptor addr %" PRIu64
					" len %" PRIu32 " overruns its frame\n",
					(uint64_t)desc->addr, desc->len);
		} else {
			inspect_packet(xsk_umem__get_data(io->buffer, desc->addr),
				       desc->len, s);
		}

		recycle[n++] = frame * AF_XDP_FRAME_SIZE;
	}
	xsk_ring_cons__release(&io->rx, received);

	if (n && publish_frames(io, l, s, recycle, n, FRAME_HELD, stop)) {
		s->fill_failures++;
		if (s->fill_failures <= MAX_REPORTED_ERRORS)
			fprintf(stderr, "Fill ring stayed full for %.0f s; %"
				PRIu32 " frames left unrecycled\n",
				FILL_RETRY_SECONDS, n);
	}
	return received;
}

static void print_interval(const struct rx_stats *s, uint64_t prev_packets,
			   uint64_t prev_bytes, double elapsed)
{
	printf("rx: %" PRIu64 " packets (+%" PRIu64 ", %.0f pps, %.1f Mbit/s)"
	       " wakeups=%" PRIu64 " ledger_errors=%" PRIu64
	       " bad_desc=%" PRIu64 "\n",
	       s->packets, s->packets - prev_packets,
	       (double)(s->packets - prev_packets) / elapsed,
	       (double)(s->bytes - prev_bytes) * 8.0 / elapsed / 1e6,
	       s->wakeups, s->ledger_errors, s->bad_desc);
	fflush(stdout);
}

static int reconcile(const struct ledger *l, struct rx_stats *s)
{
	uint64_t counts[3] = {0};
	uint32_t i;
	int ret = 0;

	for (i = 0; i < AF_XDP_FRAME_COUNT; i++)
		counts[l->state[i]]++;

	printf("Frame ownership at exit: free=%" PRIu64 " kernel=%" PRIu64
	       " held=%" PRIu64 " (expected free=%u kernel=%u held=0)\n",
	       counts[FRAME_FREE], counts[FRAME_KERNEL], counts[FRAME_HELD],
	       AF_XDP_FRAME_COUNT - AF_XDP_RING_SIZE, AF_XDP_RING_SIZE);

	if (counts[FRAME_HELD] || counts[FRAME_KERNEL] != AF_XDP_RING_SIZE) {
		fprintf(stderr, "Frame ownership did not reconcile\n");
		ret = -EIO;
	}
	if (s->ledger_errors || s->bad_desc || s->fill_failures) {
		fprintf(stderr, "%" PRIu64 " ledger errors, %" PRIu64
			" bad descriptors, %" PRIu64 " fill failures\n",
			s->ledger_errors, s->bad_desc, s->fill_failures);
		ret = -EIO;
	}
	return ret;
}

int rx_loop_run(struct af_xdp *io, const struct probe_options *opts,
		const volatile sig_atomic_t *stop, struct rx_stats *s)
{
	static struct ledger ledger;
	struct timespec last_report;
	uint64_t prev_packets = 0, prev_bytes = 0;
	double elapsed;
	uint32_t i;
	int ret;

	memset(s, 0, sizeof(*s));
	s->min_len = UINT32_MAX;
	memset(&ledger, 0, sizeof(ledger));

	ret = publish_initial_frames(io, &ledger, s, stop);
	if (ret) {
		fprintf(stderr, "Publish initial receive frames: %s\n",
			strerror(-ret));
		return ret;
	}
	printf("Published %u receive frames; polling (Ctrl+C to stop)\n",
	       AF_XDP_RING_SIZE);
	fflush(stdout);

	clock_gettime(CLOCK_MONOTONIC, &last_report);
	while (!*stop) {
		receive_batch(io, &ledger, s, stop);

		if (opts->packet_limit && s->packets >= opts->packet_limit) {
			printf("Reached packet limit\n");
			break;
		}
		if (opts->stats_interval) {
			elapsed = seconds_since(&last_report);
			if (elapsed >= opts->stats_interval) {
				print_interval(s, prev_packets, prev_bytes,
					       elapsed);
				prev_packets = s->packets;
				prev_bytes = s->bytes;
				clock_gettime(CLOCK_MONOTONIC, &last_report);
			}
		}
	}

	/*
	 * Drain what the kernel delivered before we stopped looking, bounded by
	 * one ring's worth since traffic may still be arriving. Frames left on
	 * the RX ring remain kernel-owned, so the ledger reconciles either way.
	 */
	for (i = 0; i < AF_XDP_RING_SIZE / BATCH_SIZE; i++)
		if (!receive_batch(io, &ledger, s, stop))
			break;

	return reconcile(&ledger, s);
}

void rx_print_summary(const struct rx_stats *s)
{
	printf("Receive summary\n"
	       "  packets: %" PRIu64 " in %" PRIu64 " batches, %" PRIu64 " bytes\n"
	       "  lengths: min %" PRIu32 " max %" PRIu32 "\n"
	       "  ethertype: ipv4=%" PRIu64 " ipv6=%" PRIu64 " arp=%" PRIu64
	       " other=%" PRIu64 " runt=%" PRIu64 "\n"
	       "  ipv4 protocol: tcp=%" PRIu64 " udp=%" PRIu64 " icmp=%" PRIu64
	       " other=%" PRIu64 "\n"
	       "  wakeups: %" PRIu64 " (%" PRIu64 " errors), fill retries: %"
	       PRIu64 "\n"
	       "  errors: ledger=%" PRIu64 " bad_desc=%" PRIu64
	       " fill_failures=%" PRIu64 "\n",
	       s->packets, s->batches, s->bytes,
	       s->packets ? s->min_len : 0, s->max_len,
	       s->ipv4, s->ipv6, s->arp, s->other_ethertype, s->runt,
	       s->tcp, s->udp, s->icmp, s->other_ipproto,
	       s->wakeups, s->wakeup_errors, s->fill_retries,
	       s->ledger_errors, s->bad_desc, s->fill_failures);
	fflush(stdout);
}
