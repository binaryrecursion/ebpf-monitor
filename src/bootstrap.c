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

/* ------------------------------------------------------------------ */
/* CLI argument state                                                  */
/* ------------------------------------------------------------------ */

static struct env {
    bool verbose;
    long min_duration_ms;
} env = {
    .verbose         = false,
    .min_duration_ms = 0,
};

const char *argp_program_version     = "eBPF monitor 5.0";
const char *argp_program_bug_address = "<your-email@example.com>";

static const char argp_doc[] =
    "Kernel-level system performance monitor using eBPF.\n"
    "\n"
    "Traces syscalls, scheduling, and process lifecycle events\n"
    "with adaptive baseline anomaly detection.\n";

static const struct argp_option argp_opts[] = {
    { "verbose",     'v', NULL,   0, "Enable libbpf debug output",          0 },
    { "min-dur",     'd', "MS",   0, "Minimum event duration to report (ms)", 0 },
    { 0 }
};

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
    switch (key) {
    case 'v':
        env.verbose = true;
        break;
    case 'd':
        env.min_duration_ms = strtol(arg, NULL, 10);
        break;
    case ARGP_KEY_ARG:
        argp_usage(state);
        break;
    default:
        return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static const struct argp argp = {
    .options  = argp_opts,
    .parser   = parse_opt,
    .doc      = argp_doc,
};

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

static volatile bool exiting    = false;
static time_t        start_time;

/* ------------------------------------------------------------------ */
/* Terminal helpers                                                    */
/* ------------------------------------------------------------------ */

static struct termios orig_term;

static void reset_terminal(void)
{
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_term);
}

static void setup_terminal(void)
{
    struct termios newt;

    tcgetattr(STDIN_FILENO, &orig_term);
    newt         = orig_term;
    newt.c_lflag &= ~(ICANON | ECHO);

    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
}

static int read_key(void)
{
    char c;

    if (read(STDIN_FILENO, &c, 1) == 1)
        return (unsigned char)c;

    return -1;
}

/* ------------------------------------------------------------------ */
/* libbpf logging                                                      */
/* ------------------------------------------------------------------ */

static int libbpf_print_fn(enum libbpf_print_level level,
                            const char *format,
                            va_list args)
{
    if (level == LIBBPF_DEBUG && !env.verbose)
        return 0;

    return vfprintf(stderr, format, args);
}

/* ------------------------------------------------------------------ */
/* Signal handler                                                      */
/* ------------------------------------------------------------------ */

static void sig_handler(int sig)
{
    (void)sig;
    exiting = true;
}

/* ------------------------------------------------------------------ */
/* Ring buffer callback                                                */
/* ------------------------------------------------------------------ */

static int handle_event(void *ctx, void *data, size_t data_sz)
{
    (void)ctx;
    (void)data_sz;

    const struct event *e = data;

    stats_update(e);

    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    struct ring_buffer  *rb   = NULL;
    struct bootstrap_bpf *skel;
    int err;

    time_t last_print;
    start_time = time(NULL);
    last_print  = start_time;

    /* Parse CLI args */
    err = argp_parse(&argp, argc, argv, 0, NULL, NULL);
    if (err)
        return err;

    libbpf_set_print(libbpf_print_fn);

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    setup_terminal();
    atexit(reset_terminal);

    /* Increase memlock limit so BPF maps can be allocated */
    struct rlimit rlim_new = {
        .rlim_cur = RLIM_INFINITY,
        .rlim_max = RLIM_INFINITY,
    };
    setrlimit(RLIMIT_MEMLOCK, &rlim_new);

    /* Open BPF skeleton */
    skel = bootstrap_bpf__open();
    if (!skel) {
        fprintf(stderr, "Failed to open BPF skeleton\n");
        return 1;
    }

    skel->rodata->min_duration_ns =
        (unsigned long long)env.min_duration_ms * 1000000ULL;

    /* Load BPF programs into kernel */
    err = bootstrap_bpf__load(skel);
    if (err) {
        fprintf(stderr, "Failed to load BPF programs\n");
        goto cleanup;
    }

    /* Attach BPF programs to tracepoints */
    err = bootstrap_bpf__attach(skel);
    if (err) {
        fprintf(stderr, "Failed to attach BPF programs\n");
        goto cleanup;
    }

    /* Create ring buffer consumer */
    rb = ring_buffer__new(
        bpf_map__fd(skel->maps.rb),
        handle_event,
        NULL,
        NULL);

    if (!rb) {
        fprintf(stderr, "Failed to create ring buffer\n");
        goto cleanup;
    }

    /* ---- main event loop ---- */
    while (!exiting) {

        err = ring_buffer__poll(rb, 100 /* timeout ms */);

        if (err == -EINTR)
            break;

        if (err < 0) {
            fprintf(stderr, "Error polling ring buffer: %d\n", err);
            break;
        }

        /* Handle keyboard input */
        int key = read_key();

        if (key == 'q')
            exiting = true;

        if (key == 'r') {
            stats_reset();
            start_time = time(NULL);
            last_print  = start_time;
        }

        /* Re-render every 2 seconds */
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