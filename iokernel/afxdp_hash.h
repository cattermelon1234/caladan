/*
 * afxdp_hash.h - software flow hashing for the AF_XDP receive path
 *
 * AF_XDP RX descriptors carry no NIC RSS hash, so the iokernel computes one.
 * It must match the runtime's compute_flow_affinity() (runtime/net/core.c):
 * same Toeplitz function, same key (published in iokernel_info), and the same
 * argument order. Otherwise flows still stay on one kthread, but affinity-aware
 * dialing (tcp_dial_affinity()) picks ports for the wrong kthread.
 */

#pragma once

#include <netinet/in.h>
#include <string.h>

#include <base/stddef.h>
#include <base/byteorder.h>
#include <iokernel/control.h>
#include <net/ethernet.h>
#include <net/ip.h>
#include <net/toeplitz.h>

/* the widely used default RSS key (Microsoft RSS verification suite) */
static const uint8_t afxdp_default_rss_key[40] = {
	0x6d, 0x5a, 0x56, 0xda, 0x25, 0x5b, 0x0e, 0xc2,
	0x41, 0x67, 0x25, 0x3d, 0x43, 0xa3, 0x8f, 0xb0,
	0xd0, 0xca, 0x2b, 0xcb, 0xae, 0x7b, 0x30, 0xb4,
	0x77, 0xcb, 0x2d, 0xa3, 0x80, 0x30, 0xf2, 0x0c,
	0x6a, 0x42, 0xb7, 0x3b, 0xbe, 0xac, 0x01, 0xfa,
};

BUILD_ASSERT(sizeof(afxdp_default_rss_key) >= TOEPLITZ_MIN_KEY_LEN);
BUILD_ASSERT(sizeof(afxdp_default_rss_key) <=
	     sizeof(((struct iokernel_info *)0)->rss_key));

/**
 * afxdp_publish_rss_key - publishes the software hash key to runtimes
 * @info: the shared iokernel info page
 *
 * Call during AF_XDP init, before any runtime attaches (runtimes read the key
 * when they start). Replaces what the DPDK path reads from the NIC.
 */
static inline void afxdp_publish_rss_key(struct iokernel_info *info)
{
	memset(info->rss_key, 0, sizeof(info->rss_key));
	memcpy(info->rss_key, afxdp_default_rss_key,
	       sizeof(afxdp_default_rss_key));
	info->rss_key_len = sizeof(afxdp_default_rss_key);
}

/**
 * afxdp_flow_hash - computes the flow hash of a received Ethernet frame
 * @key: the published RSS key (iok_info->rss_key)
 * @frame: the start of the Ethernet header
 * @len: the frame length
 *
 * IPv4 TCP/UDP packets hash on (saddr, daddr, sport, dport); other IPv4
 * packets and fragments hash on the addresses only. Non-IPv4 frames (e.g.
 * ARP) and truncated or malformed headers return 0.
 */
static inline uint32_t afxdp_flow_hash(const uint8_t *key, const void *frame,
				       uint32_t len)
{
	const struct eth_hdr *eth = frame;
	const struct ip_hdr *ip;
	const uint16_t *ports;
	uint32_t ihl;
	uint16_t sport = 0, dport = 0;

	if (unlikely(len < sizeof(*eth) + sizeof(*ip)))
		return 0;
	if (ntoh16(eth->type) != ETHTYPE_IP)
		return 0;

	ip = (const struct ip_hdr *)(eth + 1);
	ihl = ip->header_len * sizeof(uint32_t);
	if (unlikely(ip->version != IPVERSION || ihl < sizeof(*ip)))
		return 0;

	/* only the first fragment has ports; hash the rest on addresses */
	if ((ip->proto == IPPROTO_TCP || ip->proto == IPPROTO_UDP) &&
	    !(ntoh16(ip->off) & (IP_MF | IP_OFFMASK)) &&
	    len >= sizeof(*eth) + ihl + 2 * sizeof(uint16_t)) {
		/* TCP and UDP headers both begin with sport, dport */
		ports = (const uint16_t *)((const uint8_t *)ip + ihl);
		sport = ntoh16(ports[0]);
		dport = ntoh16(ports[1]);
	}

	/* same argument order as the runtime: (remote, local) for ingress */
	return toeplitz_hash(key, ntoh32(ip->saddr), ntoh32(ip->daddr),
			     sport, dport);
}
