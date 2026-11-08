/*
 * main.c - standalone AF_XDP receive probe
 *
 * Sets up UMEM, rings, and one socket; attaches libxdp's default redirect
 * program; then receives and recycles frames on one queue until interrupted.
 * It does not transmit or communicate with a Caladan runtime.
 */

#include <errno.h>
#include <getopt.h>
#include <net/if.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "af_xdp.h"
#include "options.h"
#include "rx_loop.h"

static volatile sig_atomic_t stop_requested;

static void request_stop(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

static void usage(FILE *out, const char *program)
{
	fprintf(out,
		"Usage: %s --interface IFACE [--queue ID] [--mode copy|zero-copy]\n"
		"          [--count N] [--stats-interval SECONDS]\n"
		"\n"
		"  -i, --interface IFACE   Network interface (required)\n"
		"  -q, --queue ID          Hardware RX queue ID (default: 0)\n"
		"  -m, --mode MODE         Explicit transfer mode (default: copy)\n"
		"  -c, --count N           Exit after N packets (default: run until signal)\n"
		"  -s, --stats-interval S  Print a stats line every S seconds (default: 1; 0 off)\n"
		"  -h, --help              Show this help\n"
		"\n"
		"Receives on one queue, recycles every frame, and reports whether frame\n"
		"ownership reconciled at exit. Transmit is not implemented.\n",
		program);
}

static int parse_uint(const char *value, unsigned long long max,
		      unsigned long long *out)
{
	unsigned long long parsed;
	char *end;

	/* Reject signs and whitespace as well as overflow and trailing text. */
	if (!value[0] || strspn(value, "0123456789") != strlen(value))
		return -EINVAL;

	errno = 0;
	parsed = strtoull(value, &end, 10);
	if (errno || *end || parsed > max)
		return -EINVAL;

	*out = parsed;
	return 0;
}

/* Returns 1 for help, 0 for valid options, or a negative error. */
static int parse_options(int argc, char **argv, struct probe_options *opts)
{
	static const struct option long_options[] = {
		{"interface", required_argument, NULL, 'i'},
		{"queue", required_argument, NULL, 'q'},
		{"mode", required_argument, NULL, 'm'},
		{"count", required_argument, NULL, 'c'},
		{"stats-interval", required_argument, NULL, 's'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};
	unsigned long long value;
	int opt;

	opts->stats_interval = 1;

	opterr = 0;
	while ((opt = getopt_long(argc, argv, "i:q:m:c:s:h", long_options,
				  NULL)) != -1) {
		switch (opt) {
		case 'i':
			opts->ifname = optarg;
			break;
		case 'q':
			if (parse_uint(optarg, UINT32_MAX, &value)) {
				fprintf(stderr, "Invalid queue ID: %s\n", optarg);
				return -EINVAL;
			}
			opts->queue_id = (uint32_t)value;
			break;
		case 'm':
			if (!strcmp(optarg, "copy")) {
				opts->zero_copy = false;
			} else if (!strcmp(optarg, "zero-copy")) {
				opts->zero_copy = true;
			} else {
				fprintf(stderr, "Invalid mode: %s\n", optarg);
				return -EINVAL;
			}
			break;
		case 'c':
			if (parse_uint(optarg, UINT64_MAX, &value)) {
				fprintf(stderr, "Invalid count: %s\n", optarg);
				return -EINVAL;
			}
			opts->packet_limit = value;
			break;
		case 's':
			if (parse_uint(optarg, UINT32_MAX, &value)) {
				fprintf(stderr, "Invalid stats interval: %s\n",
					optarg);
				return -EINVAL;
			}
			opts->stats_interval = (unsigned int)value;
			break;
		case 'h':
			return 1;
		default:
			fprintf(stderr, "Unknown option or missing option argument\n");
			return -EINVAL;
		}
	}

	if (!opts->ifname || !opts->ifname[0] ||
	    strlen(opts->ifname) >= IFNAMSIZ || optind != argc) {
		fprintf(stderr, "Provide an interface name and no positional arguments\n");
		return -EINVAL;
	}

	return 0;
}

static int install_signal_handlers(void)
{
	struct sigaction action = {0};

	/* No SA_RESTART: a blocked syscall should return so the loop can exit. */
	action.sa_handler = request_stop;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL))
		return -errno;
	return 0;
}

int main(int argc, char **argv)
{
	struct probe_options opts = {0};
	struct af_xdp io = {0};
	struct rx_stats stats;
	int ret, cleanup_ret;

	ret = parse_options(argc, argv, &opts);
	if (ret) {
		usage(ret > 0 ? stdout : stderr, argv[0]);
		return ret > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	/* Install before setup so SIGINT/SIGTERM during startup still clean up. */
	ret = install_signal_handlers();
	if (ret) {
		fprintf(stderr, "Install signal handlers: %s\n", strerror(-ret));
		return EXIT_FAILURE;
	}

	ret = af_xdp_init(&io, &opts);
	if (ret)
		goto out;

	af_xdp_print_config(&opts);

	ret = rx_loop_run(&io, &opts, &stop_requested, &stats);
	if (stop_requested)
		printf("Stop requested; releasing AF_XDP resources\n");
	rx_print_summary(&stats);
	af_xdp_print_kernel_stats(&io);

out:
	cleanup_ret = af_xdp_cleanup(&io);
	if (cleanup_ret)
		fprintf(stderr, "Release AF_XDP resources: %s\n",
			strerror(-cleanup_ret));
	return ret || cleanup_ret ? EXIT_FAILURE : EXIT_SUCCESS;
}
