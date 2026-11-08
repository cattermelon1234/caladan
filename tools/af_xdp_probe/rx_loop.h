/* Single-threaded receive loop with frame-ownership accounting. */

#pragma once

#include <signal.h>
#include <stdint.h>

struct af_xdp;
struct probe_options;

struct rx_stats {
	uint64_t packets;
	uint64_t bytes;
	uint64_t batches;
	uint64_t wakeups;		/* recvfrom calls made for the kernel */
	uint64_t wakeup_errors;
	uint64_t fill_retries;		/* Fill ring reservations that failed */
	uint64_t fill_failures;		/* batches whose frames could not be recycled */
	uint64_t ipv4, ipv6, arp, other_ethertype;
	uint64_t tcp, udp, icmp, other_ipproto;
	uint64_t runt;			/* shorter than an Ethernet header */
	uint64_t bad_desc;		/* descriptor outside UMEM or its frame */
	uint64_t ledger_errors;		/* ownership transitions that were wrong */
	uint32_t min_len, max_len;
};

/*
 * Publish the receive half of UMEM, then receive and recycle frames until
 * *stop becomes nonzero or opts->packet_limit packets have arrived. On return
 * the RX ring has been drained and the ledger reconciled. Returns 0 when the
 * ledger balanced and no descriptor or ownership errors were seen.
 */
int rx_loop_run(struct af_xdp *io, const struct probe_options *opts,
		const volatile sig_atomic_t *stop, struct rx_stats *stats);
void rx_print_summary(const struct rx_stats *stats);
