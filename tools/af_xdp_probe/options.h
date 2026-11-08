/* Command-line options shared by the probe's main program and AF_XDP setup. */

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct probe_options {
	const char *ifname;
	uint32_t queue_id;
	bool zero_copy;
	uint64_t packet_limit;		/* stop after this many packets; 0 = never */
	unsigned int stats_interval;	/* seconds between stats lines; 0 = off */
};
