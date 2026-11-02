/*
 * Force-included (via -include) when building the probe against the
 * simulated kernel. Fills in Linux-only definitions so the test build works
 * on macOS, and routes getsockopt to the simulator.
 */
#pragma once

#include <sys/mman.h>
#include <sys/socket.h>

#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif
#ifndef SOL_XDP
#define SOL_XDP 283
#endif

int sim_getsockopt(int fd, int level, int name, void *val, socklen_t *len);
#define getsockopt sim_getsockopt
