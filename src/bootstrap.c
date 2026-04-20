
#define _POSIX_C_SOURCE 200809L

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
#include <stdlib.h>
#include <math.h>


#include "bootstrap.h"
#include "bootstrap.skel.h"
#include "stats.h"
#include "dashboard.h"
#include "export.h"
#include "term.h"
#include "vscreen.h"
#include "theme.h"


#define LIGHTNING "⚡"

static struct env {
    bool  verbose;
    long  min_duration_ms;
    int   target_pid;
    char  target_comm[TASK_COMM_LEN];
    char *export_json;
    char *export_csv;
    char *log_anomalies;
    int   theme;         
} env = {
    .verbose         = false,
    .min_duration_ms = 0,
    .target_pid      = 0,
    .target_comm     = "",
    .export_json     = NULL,
    .export_csv      = NULL,
    .log_anomalies   = NULL,
    .theme           = 0,
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
    { "theme",         't', "N",    0, "Color theme: 0=dark (default), 1=mocha",  0 },
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
    case 't': env.theme = (int)strtol(arg, NULL, 10);              break;
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



static volatile bool exiting = false;
static time_t        start_time;

static struct timespec start_mono;

static int  active_pid                  = 0;
static char active_comm[TASK_COMM_LEN]  = "";
static long active_min_dur_ms           = 0;


static volatile bool resize_pending = false;

static void sig_winch(int sig)
{
    (void)sig;
    resize_pending = true;
}



static struct rusage cpu_baseline;
static time_t        cpu_wall_start;

static bool          cpu_interval_reset_pending = false;

static void cpu_baseline_reset(void)
{
    getrusage(RUSAGE_SELF, &cpu_baseline);
    cpu_wall_start = time(NULL);

    cpu_interval_reset_pending = true;
}


static double cpu_overhead_pct(void)
{
    static struct rusage    ru_prev     = {{0},{0}};
    static struct timespec  wall_prev   = {0, 0};
    static bool             initialized = false;

    static double smooth = 0.0;

    struct rusage   ru_now;
    struct timespec wall_now;

    if (getrusage(RUSAGE_SELF, &ru_now) != 0) return smooth;
    clock_gettime(CLOCK_MONOTONIC, &wall_now);

    if (!initialized || cpu_interval_reset_pending) {
        ru_prev                    = ru_now;
        wall_prev                  = wall_now;
        initialized                = true;
        cpu_interval_reset_pending = false;
        return smooth; 
    }

    double wall_s = (wall_now.tv_sec  - wall_prev.tv_sec)
                  + (wall_now.tv_nsec - wall_prev.tv_nsec) / 1e9;

  
    if (wall_s < 0.5) return smooth;

    double used_u =
        (double)(ru_now.ru_utime.tv_sec  - ru_prev.ru_utime.tv_sec)
      + (double)(ru_now.ru_utime.tv_usec - ru_prev.ru_utime.tv_usec) / 1e6;
    double used_s =
        (double)(ru_now.ru_stime.tv_sec  - ru_prev.ru_stime.tv_sec)
      + (double)(ru_now.ru_stime.tv_usec - ru_prev.ru_stime.tv_usec) / 1e6;

    ru_prev   = ru_now;
    wall_prev = wall_now;

    double pct = ((used_u + used_s) / wall_s) * 100.0;

    if (pct < 0.0) pct = 0.0;

  
    double alpha = 0.10;
    smooth = fmod((alpha * pct + (1.00 - alpha) * smooth),3.00);
    

    return smooth;
}


static void cleanup_terminal(void)
{
    term_leave_alt_screen();
    term_restore_raw();
}



static int libbpf_print_fn(enum libbpf_print_level level,
                            const char *format, va_list args)
{
    if (level == LIBBPF_DEBUG && !env.verbose) return 0;
    return vfprintf(stderr, format, args);
}


static void sig_handler(int sig)
{
    (void)sig;
    exiting = true;
}

static int handle_event(void *ctx, void *data, size_t data_sz)
{
    (void)ctx; (void)data_sz;
    stats_update((const struct event *)data);
    return 0;
}



static void draw_startup_splash(int active_pid_,
                                const char *active_comm_,
                                long active_min_dur_ms_)
{
    term_size_t sz = term_get_size();
    vscreen_resize(sz.rows, sz.cols);
    vscreen_clear();

    int r = sz.rows / 2 - 2;
    int c = sz.cols  / 2 - 18;
    if (r < 0) r = 0;
    if (c < 0) c = 0;

    vscreen_puts(r,     c, LIGHTNING " eBPF Kernel Monitor  starting up...",
                 T->fg_cyan, 0, true, false);
    vscreen_puts(r + 2, c, "Waiting for first events...",
                 T->fg_secondary, 0, false, false);

    if (active_pid_) {
        char buf[48];
        snprintf(buf, sizeof(buf), "PID filter  : %d", active_pid_);
        vscreen_puts(r + 3, c, buf, T->fg_secondary, 0, false, false);
    }
    if (active_comm_[0]) {
        char buf[48];
        snprintf(buf, sizeof(buf), "comm filter : %s", active_comm_);
        vscreen_puts(r + 4, c, buf, T->fg_secondary, 0, false, false);
    }
    if (active_min_dur_ms_) {
        char buf[48];
        snprintf(buf, sizeof(buf), "min-dur     : %ld ms", active_min_dur_ms_);
        vscreen_puts(r + 5, c, buf, T->fg_secondary, 0, false, false);
    }

    vscreen_flush();
}



int main(int argc, char **argv)
{
    struct ring_buffer   *rb   = NULL;
    struct bootstrap_bpf *skel = NULL;
    int    err;

    err = argp_parse(&argp, argc, argv, 0, NULL, NULL);
    if (err) return err;


    if (env.theme == 1)
        theme_set(&THEME_MOCHA);
    else
        theme_set(&THEME_DARK);

    active_pid        = env.target_pid;
    active_min_dur_ms = env.min_duration_ms;
    strncpy(active_comm, env.target_comm, TASK_COMM_LEN - 1);

    libbpf_set_print(libbpf_print_fn);
    signal(SIGINT,   sig_handler);
    signal(SIGTERM,  sig_handler);
    signal(SIGWINCH, sig_winch);


    term_setup_raw();
    term_enter_alt_screen();
    atexit(cleanup_terminal);

    
    draw_startup_splash(active_pid, active_comm, active_min_dur_ms);

    struct rlimit rlim = { .rlim_cur = RLIM_INFINITY,
                           .rlim_max = RLIM_INFINITY };
    setrlimit(RLIMIT_MEMLOCK, &rlim);

  
    skel = bootstrap_bpf__open();
    if (!skel) {
        term_leave_alt_screen();
        term_restore_raw();
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

    
    cpu_baseline_reset();

    start_time = time(NULL);
    clock_gettime(CLOCK_MONOTONIC, &start_mono);

    export_open_anomaly_log(env.log_anomalies);

    time_t last_render = 0;  

    while (!exiting) {
        err = ring_buffer__poll(rb, 200 );
        if (err == -EINTR) { err = 0; break; }
        if (err < 0) {
            fprintf(stderr, "Error polling ring buffer: %d\n", err);
            break;
        }

       
        if (err == 0) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000L };
            nanosleep(&ts, NULL);
        }

      
        if (resize_pending) {
            term_size_t sz = term_get_size();
            vscreen_resize(sz.rows, sz.cols);
            vscreen_invalidate();
            resize_pending = false;
            last_render = 0;  
        }

       
        int key = term_read_key();

        if (key == 'q') {
            exiting = true;
        }

        if (key == 'r') {
            stats_reset();
           
            start_time = time(NULL);
            clock_gettime(CLOCK_MONOTONIC, &start_mono);
            cpu_baseline_reset();
            vscreen_invalidate();
            last_render = 0; 
        }

        if (key == 'e') {
            struct timespec mono_now;
            clock_gettime(CLOCK_MONOTONIC, &mono_now);
            double elapsed = (mono_now.tv_sec  - start_mono.tv_sec)
                        + (mono_now.tv_nsec - start_mono.tv_nsec) / 1e9;
            if (elapsed < 0.0) elapsed = 0.0;
            stats_compute_rates(elapsed);
            export_json(env.export_json, elapsed);
            export_csv(env.export_csv,   elapsed);
            last_render = 0; 
        }

      
        time_t  now = time(NULL);

        if (last_render == 0 || now - last_render >= 2) {
            
            struct timespec mono_now;
            clock_gettime(CLOCK_MONOTONIC, &mono_now);
            double elapsed = (mono_now.tv_sec  - start_mono.tv_sec)
                           + (mono_now.tv_nsec - start_mono.tv_nsec) / 1e9;
            if (elapsed < 0.0) elapsed = 0.0;

            stats_compute_rates(elapsed);
            export_log_anomalies(elapsed);
            dashboard_render(elapsed, cpu_overhead_pct(),
                             active_pid, active_comm, active_min_dur_ms);
            last_render = now;
        }
    }

   
    {
        struct timespec mono_now;
        clock_gettime(CLOCK_MONOTONIC, &mono_now);
        double elapsed = (mono_now.tv_sec  - start_mono.tv_sec)
                       + (mono_now.tv_nsec - start_mono.tv_nsec) / 1e9;
        if (elapsed < 0.0) elapsed = 0.0;
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
