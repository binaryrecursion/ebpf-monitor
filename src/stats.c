#include <string.h>
#include <stdio.h>
#include <math.h>

#include "stats.h"

struct syscall_stat stats[MAX_STATS];
int stat_count = 0;

/* ------------------------------------------------------------------ */
/* Internal: find or create a stat slot for (process, event) pair     */
/* ------------------------------------------------------------------ */

static struct syscall_stat *get_stat(const char *proc, const char *event, int pid)
{
    for (int i = 0; i < stat_count; i++) {
        if (strcmp(stats[i].process, proc) == 0 &&
            strcmp(stats[i].event,   event) == 0)
            return &stats[i];
    }

    if (stat_count >= MAX_STATS)
        return NULL;

    struct syscall_stat *s = &stats[stat_count];

    /* Zero the entire slot first so no field is ever uninitialised */
    memset(s, 0, sizeof(*s));

    snprintf(s->process, sizeof(s->process), "%s", proc);
    snprintf(s->event,   sizeof(s->event),   "%s", event);
    s->pid = pid;

    stat_count++;
    return s;
}

/* ------------------------------------------------------------------ */
/* Internal: EMA baseline update + anomaly classification             */
/*                                                                     */
/*   On first sample  : seed baseline = current_avg                   */
/*   On later samples : baseline = a*current + (1-a)*baseline        */
/*   deviation        = |current_avg - baseline| / baseline           */
/*   anomaly          : deviation > ANOMALY_THRESHOLD                 */
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

    /* Exponential moving average */
    s->baseline_latency = BASELINE_ALPHA * current_avg
                        + (1.0 - BASELINE_ALPHA) * s->baseline_latency;

    /* Relative deviation */
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
    /* Drop events with no meaningful name */
    if (e->filename[0] == '\0')
        return;

    /* Ignore the monitor itself */
    if (strcmp(e->comm, "bootstrap") == 0)
        return;

    /* ---------- PROCESS LIFECYCLE (exit) ---------- */
    if (e->type == EVENT_EXIT) {
        struct syscall_stat *s = get_stat(e->comm, "lifecycle", e->pid);
        if (!s)
            return;

        s->pid = e->pid;
        s->count++;
        s->total_latency += (long)e->duration_ns;

        if ((long)e->duration_ns > s->max_latency)
            s->max_latency = (long)e->duration_ns;

        update_baseline(s);
        return;
    }

    /* ---------- SYSCALL + SCHED EVENTS ---------- */
    if (e->type != EVENT_SYSCALL && e->type != EVENT_SCHED)
        return;

    struct syscall_stat *s = get_stat(e->comm, e->filename, e->pid);
    if (!s)
        return;

    s->pid = e->pid;
    s->count++;
    s->total_latency += (long)e->duration_ns;

    if ((long)e->duration_ns > s->max_latency)
        s->max_latency = (long)e->duration_ns;

    if (e->type == EVENT_SCHED)
        s->ctx_switches++;

    update_baseline(s);
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
    /* Wipe all data including baselines so a reset is truly clean */
    memset(stats, 0, sizeof(stats));
    stat_count = 0;
}