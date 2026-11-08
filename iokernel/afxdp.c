/*
 * afxdp.c - AF_XDP packet I/O for the iokernel (alternative to DPDK)
 *
 * The UMEM is registered directly on the shared ingress region, so packets
 * land in memory that runtimes can already read. One socket is bound to one
 * NIC queue; libxdp's default XDP program redirects that queue to it.
 *
 * Every UMEM frame is in exactly one state. The ledger catches a frame the
 * kernel returns twice, a runtime returning a frame it doesn't own, and leaks.
 *
 *   FREE     - on the iokernel's free list
 *   KERNEL   - on the Fill or RX ring; the kernel/NIC may write into it
 *   IOKERNEL - taken off the RX ring, being handled by the dataplane
 *   RUNTIME  - lent to a runtime, returned with TXCMD_NET_COMPLETE
 *   TX       - reserved for transmit (unused until the TX path exists)
 *
 * Only the dataplane thread touches the rings and the ledger.
 */

#ifdef AF_XDP
#include <errno.h>
#include <inttypes.h>
#include <net/if.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/if_link.h>
#include <linux/if_xdp.h>
#include <xdp/xsk.h>
#endif

#include <base/log.h>
#include <base/time.h>

#include "defs.h"
#include "afxdp.h"

struct afxdp_cfg afxdp_cfg;

#ifdef AF_XDP

#include "afxdp_hash.h"

#ifndef SOL_XDP
#define SOL_XDP 283
#endif

#define AFXDP_FILL_BATCH	256U

BUILD_ASSERT(AFXDP_UMEM_SIZE <= INGRESS_MBUF_SHM_SIZE);
BUILD_ASSERT(AFXDP_FILL_RING_SIZE <= AFXDP_RX_RING_SIZE);
BUILD_ASSERT(AFXDP_FILL_RING_SIZE <= AFXDP_NR_RX_FRAMES);
BUILD_ASSERT(AFXDP_NR_TX_FRAMES < AFXDP_NR_FRAMES);

enum {
	FRAME_FREE = 0,
	FRAME_KERNEL,
	FRAME_IOKERNEL,
	FRAME_RUNTIME,
	FRAME_TX,
	FRAME_NR_STATES,
};

static const char *const frame_state_names[] = {
	"free", "kernel", "iokernel", "runtime", "tx",
};

struct afxdp_frame {
	struct proc	*owner;		/* set while FRAME_RUNTIME */
	uint8_t		state;
};

struct afxdp_stats {
	uint64_t	rx_packets;
	uint64_t	rx_bytes;
	uint64_t	wakeups;
	uint64_t	wakeup_errors;
	uint64_t	bad_desc;	/* descriptor outside UMEM or its frame */
	uint64_t	ledger_errors;	/* invalid ownership transition */
	uint64_t	bad_returns;	/* runtime returned a frame it didn't own */
};

static struct {
	struct xsk_umem		*umem;
	struct xsk_socket	*xsk;
	struct xsk_ring_prod	fill;
	struct xsk_ring_cons	comp;
	struct xsk_ring_cons	rx;
	struct xsk_ring_prod	tx;
	int			fd;
	void			*base;

	uint32_t		nr_free;
	uint32_t		free_list[AFXDP_NR_RX_FRAMES];
	uint32_t		count[FRAME_NR_STATES];
	struct afxdp_frame	frames[AFXDP_NR_FRAMES];

	struct afxdp_stats	stats;
	struct afxdp_stats	logged;
	uint64_t		next_log_us;
} ax;

static volatile sig_atomic_t stop_requested;

/*
 * Frame ownership ledger
 */

static bool frame_move(uint32_t idx, uint8_t from, uint8_t to)
{
	struct afxdp_frame *f = &ax.frames[idx];

	if (unlikely(f->state != from)) {
		ax.stats.ledger_errors++;
		log_err_ratelimited("afxdp: frame %u is %s, expected %s",
				    idx, frame_state_names[f->state],
				    frame_state_names[from]);
		return false;
	}

	ax.count[from]--;
	ax.count[to]++;
	f->state = to;
	return true;
}

/* returns the RX frame index for a UMEM offset, or false if out of range */
static inline bool rx_frame_idx(uint64_t off, uint32_t *idx)
{
	uint64_t i = off / AFXDP_FRAME_SIZE;

	if (unlikely(i >= AFXDP_NR_RX_FRAMES))
		return false;
	*idx = (uint32_t)i;
	return true;
}

/* gives a frame the iokernel holds back to the free list */
static void frame_recycle(uint32_t idx)
{
	if (frame_move(idx, FRAME_IOKERNEL, FRAME_FREE))
		ax.free_list[ax.nr_free++] = idx;
}

bool afxdp_frame_return(struct proc *p, shmptr_t off)
{
	uint32_t idx;

	if (unlikely(!rx_frame_idx(off, &idx) ||
		     ax.frames[idx].state != FRAME_RUNTIME ||
		     ax.frames[idx].owner != p)) {
		ax.stats.bad_returns++;
		return false;
	}

	ax.frames[idx].owner = NULL;
	frame_move(idx, FRAME_RUNTIME, FRAME_FREE);
	ax.free_list[ax.nr_free++] = idx;
	return true;
}

void afxdp_reclaim_proc_frames(struct proc *p)
{
	uint32_t idx, n = 0;

	for (idx = 0; idx < AFXDP_NR_RX_FRAMES; idx++) {
		if (ax.frames[idx].state != FRAME_RUNTIME ||
		    ax.frames[idx].owner != p)
			continue;
		ax.frames[idx].owner = NULL;
		frame_move(idx, FRAME_RUNTIME, FRAME_FREE);
		ax.free_list[ax.nr_free++] = idx;
		n++;
	}

	if (n)
		log_info("afxdp: reclaimed %u frames from exiting runtime", n);
}

/*
 * Ring handling
 */

static void kick_kernel(void)
{
	ax.stats.wakeups++;
	/* these are expected transient results per the kernel docs */
	if (recvfrom(ax.fd, NULL, 0, MSG_DONTWAIT, NULL, NULL) < 0 &&
	    errno != EBUSY && errno != EAGAIN && errno != ENOBUFS &&
	    errno != ENETDOWN)
		ax.stats.wakeup_errors++;
}

/*
 * Moves free frames onto the Fill ring. Every frame on the Fill ring is in
 * the KERNEL state, so the ring has at least (size - KERNEL count) free slots
 * and the reservation below cannot fail.
 */
static void fill_refill(void)
{
	uint32_t n, i, pos, idx;

	n = MIN(ax.nr_free, AFXDP_FILL_RING_SIZE - ax.count[FRAME_KERNEL]);
	n = MIN(n, AFXDP_FILL_BATCH);
	if (!n)
		return;

	if (unlikely(xsk_ring_prod__reserve(&ax.fill, n, &pos) != n)) {
		log_warn_ratelimited("afxdp: unexpected Fill ring reservation failure");
		return;
	}

	for (i = 0; i < n; i++) {
		idx = ax.free_list[--ax.nr_free];
		WARN_ON(!frame_move(idx, FRAME_FREE, FRAME_KERNEL));
		*xsk_ring_prod__fill_addr(&ax.fill, pos + i) =
			(uint64_t)idx * AFXDP_FRAME_SIZE;
	}
	xsk_ring_prod__submit(&ax.fill, n);
}

/*
 * Handles one received packet. Returns true if the frame was lent to a
 * runtime (it comes back through afxdp_frame_return()), or false if the
 * caller should recycle it now.
 *
 * For now packets are only counted. Delivering them to runtimes (ip_to_proc
 * lookup, afxdp_flow_hash(), rx_send_to_runtime()) is the next step.
 */
static bool rx_one(uint32_t idx, uint64_t off, const void *data, uint32_t len)
{
	ax.stats.rx_packets++;
	ax.stats.rx_bytes += len;
	return false;
}

bool afxdp_rx_poll(void)
{
	const struct xdp_desc *desc;
	uint32_t pos, n, i, idx, len;
	uint64_t off;

	n = xsk_ring_cons__peek(&ax.rx, IOKERNEL_RX_BURST_SIZE, &pos);

	for (i = 0; i < n; i++) {
		desc = xsk_ring_cons__rx_desc(&ax.rx, pos + i);
		off = desc->addr;
		len = desc->len;

		if (unlikely(!rx_frame_idx(off, &idx))) {
			/* not one of ours; nothing safe to recycle */
			ax.stats.bad_desc++;
			log_err_ratelimited("afxdp: RX descriptor %#" PRIx64
					    " is outside UMEM", off);
			continue;
		}

		/* a frame the kernel didn't own is left alone (ledger error) */
		if (unlikely(!frame_move(idx, FRAME_KERNEL, FRAME_IOKERNEL)))
			continue;

		if (unlikely(!len || off % AFXDP_FRAME_SIZE + len >
			     AFXDP_FRAME_SIZE)) {
			ax.stats.bad_desc++;
			frame_recycle(idx);
			continue;
		}

		if (!rx_one(idx, off, xsk_umem__get_data(ax.base, off), len))
			frame_recycle(idx);
	}

	if (n)
		xsk_ring_cons__release(&ax.rx, n);

	fill_refill();
	if (!n && xsk_ring_prod__needs_wakeup(&ax.fill))
		kick_kernel();

	STAT_INC(RX_PULLED, n);
	return n > 0;
}

/*
 * Statistics
 */

static void log_counts(const char *prefix)
{
	struct xdp_statistics ks = {0};
	socklen_t len = sizeof(ks);

	if (getsockopt(ax.fd, SOL_XDP, XDP_STATISTICS, &ks, &len))
		memset(&ks, 0, sizeof(ks));

	log_info("afxdp: %s rx=%" PRIu64 " bytes=%" PRIu64
		 " frames free=%u kernel=%u iokernel=%u runtime=%u"
		 " | wakeups=%" PRIu64 " bad_desc=%" PRIu64
		 " ledger_errors=%" PRIu64 " bad_returns=%" PRIu64
		 " | kernel rx_dropped=%" PRIu64 " rx_ring_full=%" PRIu64
		 " fill_empty=%" PRIu64,
		 prefix, ax.stats.rx_packets, ax.stats.rx_bytes,
		 ax.count[FRAME_FREE], ax.count[FRAME_KERNEL],
		 ax.count[FRAME_IOKERNEL], ax.count[FRAME_RUNTIME],
		 ax.stats.wakeups, ax.stats.bad_desc, ax.stats.ledger_errors,
		 ax.stats.bad_returns, (uint64_t)ks.rx_dropped,
		 (uint64_t)ks.rx_ring_full,
		 (uint64_t)ks.rx_fill_ring_empty_descs);
}

/* logs once a second while counters are changing */
void afxdp_log_stats(void)
{
	uint64_t now = microtime();

	if (now < ax.next_log_us)
		return;
	ax.next_log_us = now + ONE_SECOND;

	if (!memcmp(&ax.stats, &ax.logged, sizeof(ax.stats)))
		return;
	ax.logged = ax.stats;
	log_counts("");
}

/*
 * Setup and teardown
 */

static void afxdp_signal_handler(int sig)
{
	/* a second signal means shutdown is stuck; exit immediately */
	if (stop_requested)
		_exit(1);
	stop_requested = 1;
}

bool afxdp_stop_requested(void)
{
	return stop_requested;
}

static void afxdp_teardown(void)
{
	int ret;

	/*
	 * Deleting the socket removes it from the XSKMAP and detaches the XDP
	 * program libxdp loaded for it. It must go before the UMEM.
	 */
	if (ax.xsk) {
		xsk_socket__delete(ax.xsk);
		ax.xsk = NULL;
	}
	if (ax.umem) {
		ret = xsk_umem__delete(ax.umem);
		if (ret)
			log_err("afxdp: failed to delete UMEM: %s", strerror(-ret));
		ax.umem = NULL;
	}
}

void afxdp_shutdown(void)
{
	log_info("afxdp: shutting down");
	log_counts("final");
	afxdp_teardown();
}

static int verify_mode(void)
{
	struct xdp_options opts = {0};
	socklen_t len = sizeof(opts);
	bool zero_copy;

	if (getsockopt(ax.fd, SOL_XDP, XDP_OPTIONS, &opts, &len))
		return -errno;
	if (len != sizeof(opts))
		return -EIO;

	zero_copy = (opts.flags & XDP_OPTIONS_ZEROCOPY) != 0;
	if (zero_copy != afxdp_cfg.zero_copy) {
		log_err("afxdp: socket is in %s mode, %s was requested",
			zero_copy ? "zero-copy" : "copy",
			afxdp_cfg.zero_copy ? "zero-copy" : "copy");
		return -EIO;
	}
	return 0;
}

static void init_frames(void)
{
	uint32_t idx;

	memset(ax.count, 0, sizeof(ax.count));

	/* push in reverse so the lowest frames are handed out first */
	ax.nr_free = 0;
	for (idx = AFXDP_NR_RX_FRAMES; idx-- > 0;) {
		ax.frames[idx].state = FRAME_FREE;
		ax.frames[idx].owner = NULL;
		ax.free_list[ax.nr_free++] = idx;
	}
	ax.count[FRAME_FREE] = AFXDP_NR_RX_FRAMES;

	for (idx = AFXDP_NR_RX_FRAMES; idx < AFXDP_NR_FRAMES; idx++)
		ax.frames[idx].state = FRAME_TX;
	ax.count[FRAME_TX] = AFXDP_NR_TX_FRAMES;
}

static int setup_socket(void)
{
	const struct xsk_umem_config ucfg = {
		.fill_size = AFXDP_FILL_RING_SIZE,
		.comp_size = AFXDP_COMP_RING_SIZE,
		.frame_size = AFXDP_FRAME_SIZE,
		.frame_headroom = 0,
		.flags = 0,
	};
	const struct xsk_socket_config scfg = {
		.rx_size = AFXDP_RX_RING_SIZE,
		.tx_size = AFXDP_TX_RING_SIZE,
		/* libxdp attaches its default redirect program for us */
		.libxdp_flags = 0,
		/* native (driver) mode only; no fallback to generic XDP */
		.xdp_flags = XDP_FLAGS_DRV_MODE,
		.bind_flags = XDP_USE_NEED_WAKEUP |
			(afxdp_cfg.zero_copy ? XDP_ZEROCOPY : XDP_COPY),
	};
	const struct rlimit unlimited = {RLIM_INFINITY, RLIM_INFINITY};
	int ret;

	/* older kernels charge UMEM against this limit */
	if (setrlimit(RLIMIT_MEMLOCK, &unlimited))
		log_warn("afxdp: could not raise RLIMIT_MEMLOCK: %s",
			 strerror(errno));

	ret = xsk_umem__create(&ax.umem, ax.base, AFXDP_UMEM_SIZE, &ax.fill,
			       &ax.comp, &ucfg);
	if (ret) {
		log_err("afxdp: failed to register the ingress region as UMEM: %s",
			strerror(-ret));
		return ret;
	}

	ret = xsk_socket__create(&ax.xsk, afxdp_cfg.ifname, afxdp_cfg.queue_id,
				 ax.umem, &ax.rx, &ax.tx, &scfg);
	if (ret) {
		log_err("afxdp: failed to create socket on %s queue %u: %s",
			afxdp_cfg.ifname, afxdp_cfg.queue_id, strerror(-ret));
		log_err("afxdp: check the queue exists (mlx5 zero-copy uses "
			"num_channels + channel), the MTU fits a %u-byte frame, "
			"and no other XDP program is attached "
			"(ip link set dev %s xdp off)", AFXDP_FRAME_SIZE,
			afxdp_cfg.ifname);
		return ret;
	}
	ax.fd = xsk_socket__fd(ax.xsk);

	return verify_mode();
}

int afxdp_init(void)
{
	struct sigaction sa = {0};
	int ret;

	if (!cfg.af_xdp)
		return 0;

	if (!if_nametoindex(afxdp_cfg.ifname)) {
		log_err("afxdp: no interface named %s", afxdp_cfg.ifname);
		return -ENODEV;
	}

	/* the UMEM is the start of the shared ingress region (control_init) */
	ax.base = dp.ingress_mbuf_region.base;
	if (!ax.base) {
		log_err("afxdp: ingress region is not mapped");
		return -EINVAL;
	}
	dp.ingress_mbuf_region.len = AFXDP_UMEM_SIZE;

	init_frames();

	ret = setup_socket();
	if (ret) {
		afxdp_teardown();
		return ret;
	}

	/* hand the kernel its first batch of receive frames */
	while (ax.count[FRAME_KERNEL] < AFXDP_FILL_RING_SIZE)
		fill_refill();

	/* software flow hash key; runtimes read it when they attach */
	afxdp_publish_rss_key(iok_info);

	/* there is no NIC checksum offload; runtimes compute checksums */
	cfg.tx_offloads_disabled = iok_info->no_tx_offloads = true;

	dp.dataplane_mode = IOK_NET_MODE_AF_XDP;

	/* shut down cleanly so the XDP program is detached */
	sa.sa_handler = afxdp_signal_handler;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	log_info("afxdp: %s queue %u, %s mode, UMEM %zu MiB (%u RX + %u TX "
		 "frames of %u bytes), Fill/RX rings %u",
		 afxdp_cfg.ifname, afxdp_cfg.queue_id,
		 afxdp_cfg.zero_copy ? "zero-copy" : "copy",
		 AFXDP_UMEM_SIZE >> 20, AFXDP_NR_RX_FRAMES, AFXDP_NR_TX_FRAMES,
		 AFXDP_FRAME_SIZE, AFXDP_FILL_RING_SIZE);
	return 0;
}

#else /* AF_XDP */

/* main() rejects the afxdp option when built without CONFIG_AF_XDP */
int afxdp_init(void) { return 0; }
bool afxdp_rx_poll(void) { return false; }
void afxdp_log_stats(void) { }
bool afxdp_stop_requested(void) { return false; }
void afxdp_shutdown(void) { }
bool afxdp_frame_return(struct proc *p, shmptr_t off) { return false; }
void afxdp_reclaim_proc_frames(struct proc *p) { }

#endif /* AF_XDP */
