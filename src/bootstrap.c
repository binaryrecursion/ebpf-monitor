// SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)

#include <argp.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <bpf/libbpf.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>

#include "bootstrap.h"
#include "bootstrap.skel.h"
#include "stats.h"
#include "dashboard.h"
#include "export.h"

/* ------------------------------------------------------------------ */
/* CLI argument state                                                  */
/* ------------------------------------------------------------------ */

static struct env {
    bool  verbose;
    long  min_duration_ms;
    int   target_pid;                 /* 0 = all */
    char  target_comm[TASK_COMM_LEN]; /* "" = all */
    char *export_json;
    char *export_csv;
    char *log_anomalies;
} env = {
    .verbose         = false,
    .min_duration_ms = 0,
    .target_pid      = 0,
    .target_comm     = "",
    .export_json     = NULL,
    .export_csv      = NULL,
    .log_anomalies   = NULL,
};

const char *argp_program_version     = "eBPF monitor 7.0";
const char *argp_program_bug_address = "<group11@example.com>";

static const char argp_doc[] =
    "Kernel-level system performance monitor using eBPF (Group 11).\n"
    "\n"
    "Traces syscalls, scheduling, and process lifecycle events\n"
    "with adaptive baseline anomaly detection.\n"
    "\n"
    "Controls while running:\n"
    "  q  quit (also triggers final JSON/CSV export)\n"
    "  r  reset all stats and baselines\n"
    "  e  export snapshot (JSON + CSV) immediately\n";

static const struct argp_option argp_opts[] = {
    { "verbose",       'v', NULL,   0, "Enable libbpf debug output",              0 },
    { "min-dur",       'd', "MS",   0, "Minimum event duration to report (ms)",   0 },
    { "pid",           'p', "PID",  0, "Trace only this PID (0 = all)",           0 },
    { "comm",          'c', "NAME", 0, "Trace only processes matching NAME",      0 },
    { "export-json",   'j', "FILE", 0, "Write JSON snapshot to FILE on exit",     0 },
    { "export-csv",    'C', "FILE", 0, "Write CSV snapshot to FILE on exit",      0 },
    { "log-anomalies", 'l', "FILE", 0, "Append anomaly log to FILE continuously", 0 },
    { 0 }
};

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
    switch (key) {
    case 'v': env.verbose = true;                                   break;
    case 'd': env.min_duration_ms = strtol(arg, NULL, 10);         break;
    case 'p': env.target_pid = (int)strtol(arg, NULL, 10);         break;
    case 'c': strncpy(env.target_comm, arg, TASK_COMM_LEN - 1);    break;
    case 'j': env.export_json   = arg;                              break;
    case 'C': env.export_csv    = arg;                              break;
    case 'l': env.log_anomalies = arg;                              break;
    case ARGP_KEY_ARG: argp_usage(state);                           break;
    default:  return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static const struct argp argp = {
    .options = argp_opts,
    .parser  = parse_opt,
    .doc     = argp_doc,
};

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

static volatile bool exiting = false;
static time_t        start_time;

/* Active filter values — displayed in dashboard header */
static int  active_pid          = 0;
static char active_comm[TASK_COMM_LEN] = "";
static long active_min_dur_ms   = 0;

/* ------------------------------------------------------------------ */
/* CPU overhead measurement                                            */
/*                                                                     */
/* Baseline is taken AFTER BPF load/attach so libbpf's heavy startup  */
/* cost is excluded.  On 'r' (reset) the baseline is refreshed.       */
/*                                                                     */
/* Formula:                                                            */
/*   cpu_used = (utime_now - utime_base) + (stime_now - stime_base)   */
/*   overhead = (cpu_used / wall_elapsed) * 100                       */
/* ------------------------------------------------------------------ */

static struct rusage cpu_baseline;
static time_t        cpu_wall_start;

static void cpu_baseline_reset(void)
{
    getrusage(RUSAGE_SELF, &cpu_baseline);
    cpu_wall_start = time(NULL);
}

static double cpu_overhead_pct(void)
{
    struct rusage ru_now;
    if (getrusage(RUSAGE_SELF, &ru_now) != 0)
        return 0.0;

    double wall_s = difftime(time(NULL), cpu_wall_start);
    if (wall_s < 1.0)
        return 0.0;

    double used_u =
        (double)(ru_now.ru_utime.tv_sec  - cpu_baseline.ru_utime.tv_sec)
      + (double)(ru_now.ru_utime.tv_usec - cpu_baseline.ru_utime.tv_usec) / 1e6;

    double used_s =
        (double)(ru_now.ru_stime.tv_sec  - cpu_baseline.ru_stime.tv_sec)
      + (double)(ru_now.ru_stime.tv_usec - cpu_baseline.ru_stime.tv_usec) / 1e6;

    double pct = ((used_u + used_s) / wall_s) * 100.0;
    if (pct < 0.0)   pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    return pct;
}

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
    return (read(STDIN_FILENO, &c, 1) == 1) ? (unsigned char)c : -1;
}

/* ------------------------------------------------------------------ */
/* libbpf logging                                                      */
/* ------------------------------------------------------------------ */

static int libbpf_print_fn(enum libbpf_print_level level,
                            const char *format, va_list args)
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
    (void)ctx; (void)data_sz;
    stats_update((const struct event *)data);
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    struct ring_buffer   *rb   = NULL;
    struct bootstrap_bpf *skel = NULL;
    int    err;
    time_t last_print;

    err = argp_parse(&argp, argc, argv, 0, NULL, NULL);
    if (err) return err;

    /* Stash active filter values for dashboard display */
    active_pid        = env.target_pid;
    active_min_dur_ms = env.min_duration_ms;
    strncpy(active_comm, env.target_comm, TASK_COMM_LEN - 1);

    libbpf_set_print(libbpf_print_fn);
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    setup_terminal();
    atexit(reset_terminal);

    struct rlimit rlim = { .rlim_cur = RLIM_INFINITY,
                           .rlim_max = RLIM_INFINITY };
    setrlimit(RLIMIT_MEMLOCK, &rlim);

    /* ---- Open / Load / Attach ---- */
    skel = bootstrap_bpf__open();
    if (!skel) {
        fprintf(stderr, "Failed to open BPF skeleton\n");
        return 1;
    }

    skel->rodata->min_duration_ns =
        (unsigned long long)env.min_duration_ms * 1000000ULL;
    skel->rodata->target_pid = env.target_pid;
    if (env.target_comm[0] != '\0')
        memcpy((void *)skel->rodata->target_comm,
               env.target_comm, TASK_COMM_LEN);

    err = bootstrap_bpf__load(skel);
    if (err) {
        fprintf(stderr, "Failed to load BPF programs\n");
        goto cleanup;
    }

    err = bootstrap_bpf__attach(skel);
    if (err) {
        fprintf(stderr, "Failed to attach BPF programs\n");
        goto cleanup;
    }

    rb = ring_buffer__new(bpf_map__fd(skel->maps.rb),
                          handle_event, NULL, NULL);
    if (!rb) {
        fprintf(stderr, "Failed to create ring buffer\n");
        goto cleanup;
    }

    /*
     * Take the CPU/wall baseline NOW — after expensive BPF setup —
     * so overhead reflects only the monitor's steady-state cost.
     */
    cpu_baseline_reset();
    start_time = time(NULL);
    last_print  = start_time;

    export_open_anomaly_log(env.log_anomalies);

    /* Startup splash */
    printf("\033[2J\033[H");
    printf("\xe2\x9a\xa1 eBPF Kernel Monitor starting");
    if (active_pid)         printf(" | PID filter: %d",   active_pid);
    if (active_comm[0])     printf(" | comm filter: %s",  active_comm);
    if (active_min_dur_ms > 0)
        printf(" | min-dur: %ld ms", active_min_dur_ms);
    printf("\nWaiting for events...\n");

    /* ---- Main event loop ---- */
    while (!exiting) {
        err = ring_buffer__poll(rb, 100 /* ms */);

        if (err == -EINTR) break;
        if (err < 0) {
            fprintf(stderr, "Error polling ring buffer: %d\n", err);
            break;
        }

        int key = read_key();

        if (key == 'q') {
            exiting = true;
        }

        if (key == 'r') {
            stats_reset();
            start_time = time(NULL);
            last_print  = start_time;
            cpu_baseline_reset();
            dashboard_render(0.0, 0.0,
                             active_pid, active_comm, active_min_dur_ms);
        }

        if (key == 'e') {
            double elapsed = difftime(time(NULL), start_time);
            stats_compute_rates(elapsed);
            export_json(env.export_json, elapsed);
            export_csv(env.export_csv,   elapsed);
        }

        time_t now     = time(NULL);
        double elapsed = difftime(now, start_time);

        if (now - last_print >= 2) {
            stats_compute_rates(elapsed);
            export_log_anomalies(elapsed);
            dashboard_render(elapsed, cpu_overhead_pct(),
                             active_pid, active_comm, active_min_dur_ms);
            last_print = now;
        }
    }

    /* Final export on exit */
    {
        double elapsed = difftime(time(NULL), start_time);
        stats_compute_rates(elapsed);
        export_json(env.export_json, elapsed);
        export_csv(env.export_csv,   elapsed);
        export_close();
    }

cleanup:
    ring_buffer__free(rb);
    bootstrap_bpf__destroy(skel);
    return err < 0 ? -err : 0;
}
