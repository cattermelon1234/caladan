/*
 * toeplitz.h - software Toeplitz (RSS) hash shared by the iokernel and runtime
 *
 * The runtime uses this to predict which kthread a flow will be steered to
 * (see compute_flow_affinity()). Any iokernel path that computes the flow hash
 * in software must use the same function and key, or those predictions break.
 */

#pragma once

#include <base/stddef.h>
#include <base/byteorder.h>

/* the key must hold at least this many bytes (3 input words + 1) */
#define TOEPLITZ_MIN_KEY_LEN	16

/**
 * toeplitz_hash - computes the Toeplitz hash of an IPv4 4-tuple
 * @key: the RSS key (at least TOEPLITZ_MIN_KEY_LEN bytes)
 * @saddr: the source IP address (host byte order)
 * @daddr: the destination IP address (host byte order)
 * @sport: the source port (host byte order), 0 if none
 * @dport: the destination port (host byte order), 0 if none
 *
 * Matches the standard RSS hash over (saddr, daddr, sport, dport). With both
 * ports zero, it equals the IPv4-only RSS hash.
 */
static inline uint32_t toeplitz_hash(const uint8_t *key, uint32_t saddr,
				     uint32_t daddr, uint16_t sport,
				     uint16_t dport)
{
	uint32_t i, j, map, ret = 0, input_tuple[] = {
		saddr, daddr, dport | sport << 16
	};

	for (j = 0; j < ARRAY_SIZE(input_tuple); j++) {
		for (map = input_tuple[j]; map;	map &= (map - 1)) {
			i = (uint32_t)__builtin_ctz(map);
			ret ^= hton32(((const uint32_t *)key)[j]) << (31 - i) |
					(uint32_t)((uint64_t)(hton32(((const uint32_t *)key)[j + 1])) >>
					(i + 1));
		}
	}

	return ret;
}
