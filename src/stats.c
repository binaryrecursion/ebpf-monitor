#include <string.h>
#include <stdio.h>
#include <math.h>

#include "stats.h"

struct syscall_stat stats[MAX_STATS];
int  stat_count          = 0;
long total_events_dropped = 0;

/* ------------------------------------------------------------------ */
/* Internal: find or create a stat slot for (process, event) pair     */
/* Returns NULL if the table is full (event is counted as dropped).   */
/* ------------------------------------------------------------------ */

static struct syscall_stat *get_stat(const char *proc,
                                     const char *ev,
                                     int         pid)
{
    for (int i = 0; i < stat_count; i++) {
        if (strcmp(stats[i].process, proc) == 0 &&
            strcmp(stats[i].event,   ev)   == 0)
            return &stats[i];
    }

    if (stat_count >= MAX_STATS) {
        total_events_dropped++;
        return NULL;
    }

    struct syscall_stat *s = &stats[stat_count];
    memset(s, 0, sizeof(*s));
    snprintf(s->process, sizeof(s->process), "%s", proc);
    snprintf(s->event,   sizeof(s->event),   "%s", ev);
    s->pid = pid;
    stat_count++;
    return s;
}

/* ------------------------------------------------------------------ */
/* Internal: EMA baseline update + anomaly classification             */
/*                                                                     */
/*  First sample  → seed baseline = current_avg                       */
/*  Later samples → baseline = α·current + (1-α)·baseline            */
/*  deviation     = |current_avg − baseline| / baseline               */
/*  anomaly       → deviation > ANOMALY_THRESHOLD                     */
/* ------------------------------------------------------------------ */

static void update_baseline(struct syscall_stat *s)
{
    if (s->count == 0)
        return;

    double current_avg = (double)s->total_latency / (double)s->count;

    if (!s->baseline_ready) {
        s->baseline_latency = current_avg;
        s->baseline_ready   = 1;
        s->deviation        = 0.0;
        s->is_anomaly       = 0;
        return;
    }

    s->baseline_latency = BASELINE_ALPHA * current_avg
                        + (1.0 - BASELINE_ALPHA) * s->baseline_latency;

    if (s->baseline_latency > 0.0)
        s->deviation = fabs(current_avg - s->baseline_latency)
                       / s->baseline_latency;
    else
        s->deviation = 0.0;

    s->is_anomaly = (s->deviation > ANOMALY_THRESHOLD) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void stats_update(const struct event *e)
{
    /* Drop events with no meaningful label */
    if (e->filename[0] == '\0')
        return;

    /* Ignore the monitor binary itself */
    if (strcmp(e->comm, "bootstrap") == 0)
        return;

    /* ---------- PROCESS EXEC ---------- */
    if (e->type == EVENT_EXEC) {
        struct syscall_stat *s = get_stat(e->comm, "exec", e->pid);
        if (!s) return;
        s->pid = e->pid;
        s->exec_count++;
        s->count++;
        /* exec has duration_ns = 0; don't skew latency averages */
        update_baseline(s);
        return;
    }

    /* ---------- PROCESS EXIT (lifecycle duration) ---------- */
    if (e->type == EVENT_EXIT) {
        struct syscall_stat *s = get_stat(e->comm, "lifecycle", e->pid);
        if (!s) return;
        s->pid = e->pid;
        s->count++;
        s->total_latency += (long)e->duration_ns;
        if ((long)e->duration_ns > s->max_latency)
            s->max_latency = (long)e->duration_ns;
        update_baseline(s);
        return;
    }

    /* ---------- SYSCALL EVENTS ---------- */
    if (e->type == EVENT_SYSCALL) {
        struct syscall_stat *s = get_stat(e->comm, e->filename, e->pid);
        if (!s) return;
        s->pid = e->pid;
        s->count++;
        s->total_latency += (long)e->duration_ns;
        if ((long)e->duration_ns > s->max_latency)
            s->max_latency = (long)e->duration_ns;
        update_baseline(s);
        return;
    }

    /* ---------- SCHED EVENTS ---------- */
    if (e->type == EVENT_SCHED) {
        struct syscall_stat *s = get_stat(e->comm, "sched", e->pid);
        if (!s) return;
        s->pid = e->pid;
        s->ctx_switches++;

        /* Filter out noise: very long off-CPU times are usually sleep,
           not scheduling latency.  Count them separately but don't
           include them in the latency average or baseline.           */
        if (e->duration_ns > SCHED_NOISE_NS) {
            s->drop_count++;
            /* Still update count so rate is accurate */
            s->count++;
            return;
        }

        s->count++;
        s->total_latency += (long)e->duration_ns;
        if ((long)e->duration_ns > s->max_latency)
            s->max_latency = (long)e->duration_ns;
        update_baseline(s);
        return;
    }
}

void stats_compute_rates(double elapsed)
{
    if (elapsed <= 0.0)
        return;
    for (int i = 0; i < stat_count; i++)
        stats[i].rate = (double)stats[i].count / elapsed;
}

void stats_reset(void)
{
    memset(stats, 0, sizeof(stats));
    stat_count           = 0;
    total_events_dropped = 0;
}