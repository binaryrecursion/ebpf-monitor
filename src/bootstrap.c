// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)

#include <argp.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <sys/resource.h>
#include <bpf/libbpf.h>

#include <termios.h>
#include <unistd.h>
#include <fcntl.h>

#include "bootstrap.h"
#include "bootstrap.skel.h"
#include "stats.h"
#include "dashboard.h"

static struct env {
	bool verbose;
	long min_duration_ms;
} env;

static volatile bool exiting = false;
static time_t start_time;

const char *argp_program_version = "eBPF monitor 5.0";

/* terminal state */
static struct termios orig_term;

/* ---------------- terminal helpers ---------------- */

static void reset_terminal()
{
	// printf("\033[?1049l");   /* leave alternate screen */
	tcsetattr(STDIN_FILENO, TCSANOW, &orig_term);
}

static void setup_terminal()
{
	struct termios newt;

	tcgetattr(STDIN_FILENO, &orig_term);
	newt = orig_term;

	newt.c_lflag &= ~(ICANON | ECHO);

	tcsetattr(STDIN_FILENO, TCSANOW, &newt);
	fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);

	// printf("\033[?1049h");   /* enter alternate screen */
}

static int read_key()
{
	char c;

	if (read(STDIN_FILENO, &c, 1) == 1)
		return c;

	return -1;
}

/* ---------------- libbpf logging ---------------- */

static int libbpf_print_fn(enum libbpf_print_level level,
                           const char *format,
                           va_list args)
{
	if (level == LIBBPF_DEBUG && !env.verbose)
		return 0;

	return vfprintf(stderr, format, args);
}

/* ---------------- signal handling ---------------- */

static void sig_handler(int sig)
{
	exiting = true;
}

/* ---------------- ring buffer callback ---------------- */

static int handle_event(void *ctx, void *data, size_t data_sz)
{
	const struct event *e = data;

	stats_update(e);

	return 0;
}

/* ---------------- main ---------------- */

int main(int argc, char **argv)
{
	struct ring_buffer *rb = NULL;
	struct bootstrap_bpf *skel;
	int err;

	time_t last_print = time(NULL);
	start_time = time(NULL);

	libbpf_set_print(libbpf_print_fn);

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	setup_terminal();
	atexit(reset_terminal);

	/* increase memlock limit */
	struct rlimit rlim_new = {
		.rlim_cur = RLIM_INFINITY,
		.rlim_max = RLIM_INFINITY,
	};
	setrlimit(RLIMIT_MEMLOCK, &rlim_new);

	/* open BPF program */
	skel = bootstrap_bpf__open();
	if (!skel) {
		fprintf(stderr, "Failed to open BPF skeleton\n");
		return 1;
	}

	skel->rodata->min_duration_ns =
	    env.min_duration_ms * 1000000ULL;

	/* load BPF */
	err = bootstrap_bpf__load(skel);
	if (err) {
		fprintf(stderr, "Failed to load BPF programs\n");
		goto cleanup;
	}

	/* attach BPF */
	err = bootstrap_bpf__attach(skel);
	if (err) {
		fprintf(stderr, "Failed to attach BPF programs\n");
		goto cleanup;
	}

	/* create ring buffer */
	rb = ring_buffer__new(
	    bpf_map__fd(skel->maps.rb),
	    handle_event,
	    NULL,
	    NULL);

	if (!rb) {
		fprintf(stderr, "Failed to create ring buffer\n");
		goto cleanup;
	}

	while (!exiting) {

		err = ring_buffer__poll(rb, 100);

		if (err == -EINTR)
			break;

		if (err < 0) {
			fprintf(stderr,
			        "Error polling ring buffer: %d\n",
			        err);
			break;
		}

		/* keyboard input */
		int key = read_key();

		if (key == 'q')
			exiting = true;

		if (key == 'r') {
			stats_reset();
			start_time = time(NULL);
		}

		time_t now = time(NULL);

		if (now - last_print >= 2) {

			double elapsed = difftime(now, start_time);

			stats_compute_rates(elapsed);

			dashboard_render(elapsed);

			last_print = now;
		}
	}

cleanup:
	ring_buffer__free(rb);
	bootstrap_bpf__destroy(skel);

	return err < 0 ? -err : 0;
}