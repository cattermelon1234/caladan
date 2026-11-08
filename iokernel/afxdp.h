/*
 * afxdp.h - AF_XDP packet I/O for the iokernel (alternative to DPDK)
 *
 * Selected with the "afxdp IFACE QUEUE copy|zc" command line option and
 * built only when CONFIG_AF_XDP=y. This header has no libxdp dependency so
 * the rest of the iokernel compiles either way.
 *
 * Memory layout: the UMEM *is* the start of the shared ingress region that
 * runtimes already map read-only. A received packet is never copied by the
 * iokernel; its UMEM offset is the shmptr handed to the runtime, and the
 * runtime returns the same offset with TXCMD_NET_COMPLETE.
 */

#pragma once

#include <base/stddef.h>
#include <iokernel/shm.h>

struct proc;

struct afxdp_cfg {
	const char	*ifname;	/* experiment interface */
	uint32_t	queue_id;	/* hardware RX queue to bind */
	bool		zero_copy;	/* require zero-copy (else copy mode) */
};

extern struct afxdp_cfg afxdp_cfg;

/* UMEM geometry (aligned chunk mode) */
#define AFXDP_FRAME_SIZE	4096U
#define AFXDP_NR_FRAMES		32768U	/* 128 MiB of the ingress region */
#define AFXDP_NR_TX_FRAMES	4096U	/* reserved at the end for TX */
#define AFXDP_NR_RX_FRAMES	(AFXDP_NR_FRAMES - AFXDP_NR_TX_FRAMES)
#define AFXDP_UMEM_SIZE		((size_t)AFXDP_FRAME_SIZE * AFXDP_NR_FRAMES)

/* ring sizes; RX >= Fill so the RX ring can never overflow */
#define AFXDP_FILL_RING_SIZE	4096U
#define AFXDP_RX_RING_SIZE	4096U
#define AFXDP_TX_RING_SIZE	2048U
#define AFXDP_COMP_RING_SIZE	2048U

/* initialization (an init handler; does nothing unless cfg.af_xdp) */
extern int afxdp_init(void);

/* dataplane polling; only the dataplane thread may call these */
extern bool afxdp_rx_poll(void);
extern void afxdp_log_stats(void);

/* clean shutdown on SIGINT/SIGTERM (detaches the XDP program) */
extern bool afxdp_stop_requested(void);
extern void afxdp_shutdown(void);

/*
 * RX frame ownership, for returning frames from runtimes. Not wired into
 * commands.c or dp_clients.c yet (RX delivery is the next step).
 */
extern bool afxdp_frame_return(struct proc *p, shmptr_t off);
extern void afxdp_reclaim_proc_frames(struct proc *p);
