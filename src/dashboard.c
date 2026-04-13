#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dashboard.h"
#include "stats.h"

#define BAR_WIDTH   30
#define TOP_N        6
#define MAX_DISPLAY 12

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int compare_latency(const void *a, const void *b)
{
    const struct syscall_stat *s1 = a, *s2 = b;
    if (s2->max_latency > s1->max_latency) return  1;
    if (s2->max_latency < s1->max_latency) return -1;
    return 0;
}

static int compare_deviation(const void *a, const void *b)
{
    const struct syscall_stat *s1 = a, *s2 = b;
    if (s2->deviation > s1->deviation) return  1;
    if (s2->deviation < s1->deviation) return -1;
    return 0;
}

static int compare_rate(const void *a, const void *b)
{
    const struct syscall_stat *s1 = a, *s2 = b;
    if (s2->rate > s1->rate) return  1;
    if (s2->rate < s1->rate) return -1;
    return 0;
}

static void draw_bar(double rate, double max_rate)
{
    int bars = (max_rate > 0) ? (int)((rate / max_rate) * BAR_WIDTH) : 0;
    if (bars > BAR_WIDTH) bars = BAR_WIDTH;
    for (int i = 0; i < BAR_WIDTH; i++)
        printf(i < bars ? "\xe2\x96\x88" : " ");
}

/* ------------------------------------------------------------------ */
/* Aggregation helper: merge stats[] by event name                    */
/* ------------------------------------------------------------------ */

typedef struct {
    char   event[32];
    long   count;
    long   total_latency;
    long   max_latency;
    long   valid_count;   /* count excluding sched noise drops */
    double rate;
} agg_entry;

static int build_event_agg(agg_entry *agg, int max_agg)
{
    int count = 0;
    for (int i = 0; i < stat_count; i++) {
        int found = -1;
        for (int j = 0; j < count; j++) {
            if (strcmp(agg[j].event, stats[i].event) == 0) {
                found = j; break;
            }
        }
        if (found == -1) {
            if (count >= max_agg) continue;
            strncpy(agg[count].event, stats[i].event,
                    sizeof(agg[count].event) - 1);
            agg[count].event[sizeof(agg[count].event)-1] = '\0';
            agg[count].count         = stats[i].count;
            agg[count].total_latency = stats[i].total_latency;
            agg[count].max_latency   = stats[i].max_latency;
            agg[count].valid_count   = stats[i].count - stats[i].drop_count;
            agg[count].rate          = stats[i].rate;
            count++;
        } else {
            agg[found].count         += stats[i].count;
            agg[found].total_latency += stats[i].total_latency;
            agg[found].valid_count   += (stats[i].count - stats[i].drop_count);
            agg[found].rate          += stats[i].rate;
            if (stats[i].max_latency > agg[found].max_latency)
                agg[found].max_latency = stats[i].max_latency;
        }
    }
    return count;
}

static void sort_agg_by_rate(agg_entry *agg, int count)
{
    for (int i = 0; i < count - 1; i++)
        for (int j = i + 1; j < count; j++)
            if (agg[j].rate > agg[i].rate) {
                agg_entry tmp = agg[i]; agg[i] = agg[j]; agg[j] = tmp;
            }
}

/* ------------------------------------------------------------------ */
/* Section 1 — Main table (sorted by rate, top MAX_DISPLAY rows)      */
/*                                                                     */
/* Columns: PROCESS PID EVENT RATE/s AVG(us) P95(us) MAX(us)         */
/*          CTXSW EXECS                                                */
/* ! marker for active anomalies.                                      */
/* ------------------------------------------------------------------ */

static void print_main_table(void)
{
    struct syscall_stat tmp[MAX_STATS];
    memcpy(tmp, stats, stat_count * sizeof(*tmp));
    qsort(tmp, stat_count, sizeof(*tmp), compare_rate);

    printf(COLOR_BOLD
           "\n%-14s %-6s %-12s %8s %10s %10s %10s %6s %6s\n"
           COLOR_RESET,
           "PROCESS", "PID", "EVENT", "RATE/s",
           "AVG(us)", "P95(us)", "MAX(us)", "CTXSW", "EXECS");
    printf("------------------------------------------------------------------------------------\n");

    int shown = 0;
    for (int i = 0; i < stat_count && shown < MAX_DISPLAY; i++) {
        struct syscall_stat *s = &tmp[i];

        long valid = s->count - s->drop_count;
        long avg   = (valid > 0 && s->total_latency > 0)
                     ? (s->total_latency / valid) / 1000
                     : 0;
        long p95   = stats_p95_us(s);  /* -1 if too few samples */
        long maxus = s->max_latency / 1000;

        const char *marker = (s->is_anomaly && s->baseline_ready)
            ? COLOR_RED "!" COLOR_RESET " "
            : "  ";

        /* P95 column: show "--" when not enough samples */
        if (p95 < 0) {
            printf("%s%-14s %-6d %-12s %8.1f %s%10ld%s %10s %10ld %6ld %6ld\n",
                   marker,
                   s->process, s->pid, s->event, s->rate,
                   latency_color(avg), avg, COLOR_RESET,
                   "--",
                   maxus,
                   s->ctx_switches, s->exec_count);
        } else {
            printf("%s%-14s %-6d %-12s %8.1f %s%10ld%s %s%10ld%s %10ld %6ld %6ld\n",
                   marker,
                   s->process, s->pid, s->event, s->rate,
                   latency_color(avg), avg, COLOR_RESET,
                   latency_color(p95), p95, COLOR_RESET,
                   maxus,
                   s->ctx_switches, s->exec_count);
        }
        shown++;
    }

    if (stat_count > MAX_DISPLAY)
        printf(COLOR_DIM "  ... %d more entries (showing top %d by rate)\n"
               COLOR_RESET, stat_count - MAX_DISPLAY, MAX_DISPLAY);

    if (total_events_dropped > 0)
        printf(COLOR_RED
               "  WARNING: %ld events dropped — MAX_STATS (%d) reached. "
               "Increase MAX_STATS in stats.h or press 'r' to reset.\n"
               COLOR_RESET,
               total_events_dropped, MAX_STATS);
}

/* ------------------------------------------------------------------ */
/* Section 2 — Event summary (aggregated across all processes)        */
/*                                                                     */
/* AVG is computed only from valid (non-noise) samples.  A note       */
/* warns when the max is >= 10× the average (outlier present).        */
/* ------------------------------------------------------------------ */

static void print_event_summary(void)
{
    agg_entry agg[32];
    int count = build_event_agg(agg, 32);

    printf(COLOR_BOLD "\nEVENT SUMMARY\n" COLOR_RESET);
    printf("----------------------------------------------------------------------\n");
    printf(COLOR_BOLD "%-10s  %10s  %10s  %10s  %10s\n" COLOR_RESET,
           "EVENT", "AVG(us)", "MAX(us)", "TOTAL", "RATE/s");

    for (int i = 0; i < count; i++) {
        long avg = (agg[i].valid_count > 0 && agg[i].total_latency > 0)
                   ? (agg[i].total_latency / agg[i].valid_count) / 1000
                   : 0;
        long maxus = agg[i].max_latency / 1000;

        /* Outlier warning: max is 10x+ the average — one bad sample is
           skewing the avg for this event type across all processes.   */
        const char *outlier = (avg > 0 && maxus > avg * 10) ? " *" : "";

        printf("%-10s  %s%10ld%s  %10ld  %10ld  %10.1f%s\n",
               agg[i].event,
               latency_color(avg), avg, COLOR_RESET,
               maxus,
               agg[i].count,
               agg[i].rate,
               outlier);
    }

    /* Print footnote only if any outliers exist */
    for (int i = 0; i < count; i++) {
        long avg   = (agg[i].valid_count > 0 && agg[i].total_latency > 0)
                     ? (agg[i].total_latency / agg[i].valid_count) / 1000 : 0;
        long maxus = agg[i].max_latency / 1000;
        if (avg > 0 && maxus > avg * 10) {
            printf(COLOR_DIM
                   "  * MAX >> AVG: a single outlier is skewing this row."
                   " Per-process breakdown above is more accurate.\n"
                   COLOR_RESET);
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Section 3 — Top events by rate                                     */
/* ------------------------------------------------------------------ */

static void print_top_events(void)
{
    agg_entry agg[32];
    int count = build_event_agg(agg, 32);
    sort_agg_by_rate(agg, count);

    printf(COLOR_BOLD "\nTOP EVENTS BY RATE\n" COLOR_RESET);
    printf("----------------------------------------\n");

    for (int i = 0; i < count && i < TOP_N; i++)
        printf("  %-12s %8.1f/s\n", agg[i].event, agg[i].rate);
}

/* ------------------------------------------------------------------ */
/* Section 4 — Slowest SYSCALL events (peak latency)                  */
/*                                                                     */
/* Deliberately excludes sched, exec, and lifecycle entries because:  */
/*   - sched MAX clusters at the noise boundary (199ms) and is not    */
/*     comparable to syscall latency.                                  */
/*   - lifecycle is a whole-process duration, not a per-call cost.    */
/*   - exec latency is always 0 (no duration recorded at exec entry). */
/* ------------------------------------------------------------------ */

static void print_slowest_syscalls(void)
{
    struct syscall_stat tmp[MAX_STATS];
    int n = 0;

    /* Copy only genuine syscall entries */
    for (int i = 0; i < stat_count; i++) {
        const char *ev = stats[i].event;
        if (strcmp(ev, "sched")     == 0) continue;
        if (strcmp(ev, "exec")      == 0) continue;
        if (strcmp(ev, "lifecycle") == 0) continue;
        if (n < MAX_STATS)
            tmp[n++] = stats[i];
    }

    qsort(tmp, n, sizeof(*tmp), compare_latency);

    printf(COLOR_BOLD "\nSLOWEST SYSCALLS (peak latency)\n" COLOR_RESET);
    printf("----------------------------------------\n");

    if (n == 0) {
        printf(COLOR_DIM "  No syscall data yet.\n" COLOR_RESET);
        return;
    }

    for (int i = 0; i < n && i < TOP_N; i++) {
        long us  = tmp[i].max_latency / 1000;
        long p95 = stats_p95_us(&tmp[i]);
        if (p95 >= 0) {
            printf("  %-14s %-12s  max %s%ld us%s  p95 %ld us\n",
                   tmp[i].process, tmp[i].event,
                   latency_color(us), us, COLOR_RESET, p95);
        } else {
            printf("  %-14s %-12s  max %s%ld us%s\n",
                   tmp[i].process, tmp[i].event,
                   latency_color(us), us, COLOR_RESET);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Section 5 — Activity graph                                         */
/* ------------------------------------------------------------------ */

static void print_activity_graph(void)
{
    if (stat_count == 0) return;

    agg_entry agg[32];
    int count = build_event_agg(agg, 32);
    sort_agg_by_rate(agg, count);

    double max_rate = (count > 0 && agg[0].rate > 0) ? agg[0].rate : 1.0;

    printf(COLOR_BOLD "\nACTIVITY\n" COLOR_RESET);
    printf("----------------------------------------\n");

    for (int i = 0; i < count && i < TOP_N; i++) {
        printf("  %-12s ", agg[i].event);
        printf(COLOR_GREEN);
        draw_bar(agg[i].rate, max_rate);
        printf(COLOR_RESET "  %6.1f/s\n", agg[i].rate);
    }
}

/* ------------------------------------------------------------------ */
/* Section 6 — Anomaly alerts                                         */
/* ------------------------------------------------------------------ */

static void print_anomaly_alerts(void)
{
    struct syscall_stat tmp[MAX_STATS];
    memcpy(tmp, stats, stat_count * sizeof(*tmp));
    qsort(tmp, stat_count, sizeof(*tmp), compare_deviation);

    int n_anomalies = 0;
    for (int i = 0; i < stat_count; i++)
        if (tmp[i].is_anomaly && tmp[i].baseline_ready)
            n_anomalies++;

    printf(COLOR_BOLD "\nADAPTIVE BASELINE \xe2\x80\x94 ANOMALY ALERTS\n" COLOR_RESET);
    printf("----------------------------------------------------------------------\n");

    if (n_anomalies == 0) {
        printf(COLOR_GREEN "  All events within normal range.\n" COLOR_RESET);
        return;
    }

    printf(COLOR_BOLD
           "  %-14s %-12s  %8s  %12s  %12s\n"
           COLOR_RESET,
           "PROCESS", "EVENT", "DEVIATION", "BASELINE(us)", "CURRENT(us)");

    int shown = 0;
    for (int i = 0; i < stat_count && shown < TOP_N; i++) {
        if (!tmp[i].is_anomaly || !tmp[i].baseline_ready)
            continue;

        long valid       = tmp[i].count - tmp[i].drop_count;
        long current_us  = (valid > 0 && tmp[i].total_latency > 0)
                           ? (tmp[i].total_latency / valid) / 1000
                           : 0;
        long baseline_us = (long)(tmp[i].baseline_latency / 1000.0);

        printf("  %-14s %-12s  %s%7.1f%%%s  %12ld  %12ld\n",
               tmp[i].process, tmp[i].event,
               deviation_color(tmp[i].deviation),
               tmp[i].deviation * 100.0, COLOR_RESET,
               baseline_us, current_us);
        shown++;
    }

    if (n_anomalies > TOP_N)
        printf(COLOR_DIM "  ... %d more anomalies not shown\n" COLOR_RESET,
               n_anomalies - TOP_N);
}

/* ------------------------------------------------------------------ */
/* Section 7 — Process lifecycle (exec/exit summary)                  */
/* ------------------------------------------------------------------ */

static void print_process_summary(void)
{
    int lc_count = 0;
    struct {
        char process[16];
        int  pid;
        long execs;
        long exits;
        long avg_life_us;
    } procs[64];

    for (int i = 0; i < stat_count; i++) {
        const struct syscall_stat *s = &stats[i];
        if (strcmp(s->event, "exec") != 0 &&
            strcmp(s->event, "lifecycle") != 0)
            continue;

        int idx = -1;
        for (int j = 0; j < lc_count; j++) {
            if (strcmp(procs[j].process, s->process) == 0) {
                idx = j; break;
            }
        }
        if (idx == -1) {
            if (lc_count >= 64) continue;
            idx = lc_count++;
            memset(&procs[idx], 0, sizeof(procs[0]));
            strncpy(procs[idx].process, s->process,
                    sizeof(procs[0].process) - 1);
            procs[idx].pid = s->pid;
        }

        if (strcmp(s->event, "exec") == 0)
            procs[idx].execs = s->exec_count + s->count;
        else if (strcmp(s->event, "lifecycle") == 0) {
            procs[idx].exits = s->count;
            procs[idx].avg_life_us = s->count
                ? (s->total_latency / s->count) / 1000 : 0;
        }
    }

    if (lc_count == 0) return;

    printf(COLOR_BOLD "\nPROCESS LIFECYCLE\n" COLOR_RESET);
    printf("----------------------------------------------------------------------\n");
    printf(COLOR_BOLD "  %-14s %-6s  %8s  %8s  %12s\n" COLOR_RESET,
           "PROCESS", "PID", "EXECS", "EXITS", "AVG LIFE(us)");

    for (int i = 0; i < lc_count; i++) {
        printf("  %-14s %-6d  %8ld  %8ld  %12ld\n",
               procs[i].process, procs[i].pid,
               procs[i].execs, procs[i].exits,
               procs[i].avg_life_us);
    }
}

/* ------------------------------------------------------------------ */
/* Main render entry point                                            */
/* ------------------------------------------------------------------ */

void dashboard_render(double elapsed, double cpu_pct,
                      int active_pid, const char *active_comm,
                      long active_min_ms)
{
    /* Clear screen, cursor home */
    printf("\033[2J\033[H");

    /* ---- Header line 1: core stats ---- */
    printf(COLOR_CYAN COLOR_BOLD "\xe2\x9a\xa1 eBPF Kernel Monitor" COLOR_RESET);
    printf(" | Runtime: %.0fs | CPU: %.2f%% | Tracked: %d/%d slots",
           elapsed, cpu_pct, stat_count, MAX_STATS);
    if (total_events_dropped > 0)
        printf(COLOR_RED " | DROPPED: %ld" COLOR_RESET, total_events_dropped);
    printf("\n");

    /* ---- Header line 2: active filters (shown only when set) ---- */
    int any_filter = (active_pid != 0 || active_comm[0] != '\0' || active_min_ms > 0);
    if (any_filter) {
        printf(COLOR_MAGENTA COLOR_BOLD "  FILTERS:" COLOR_RESET COLOR_MAGENTA);
        if (active_pid)       printf("  pid=%d",       active_pid);
        if (active_comm[0])   printf("  comm=%s",      active_comm);
        if (active_min_ms > 0) printf("  min-dur=%ldms", active_min_ms);
        printf(COLOR_RESET "  (kernel-side, zero overhead for excluded processes)\n");
    }

    printf("======================================================================\n");
    printf(COLOR_DIM
           "  ! = active anomaly   latency: "
           COLOR_GREEN "green<10us " COLOR_RESET COLOR_DIM
           COLOR_YELLOW "yellow<100us " COLOR_RESET COLOR_DIM
           COLOR_RED "red>=100us"
           COLOR_RESET "\n");
    printf(COLOR_DIM
           "  sched avg/max/p95 exclude sleep noise (>200ms off-CPU)\n"
           COLOR_RESET);

    print_main_table();
    print_event_summary();
    print_top_events();
    print_slowest_syscalls();
    print_activity_graph();
    print_anomaly_alerts();
    print_process_summary();

    printf("\nControls: " COLOR_BOLD "q" COLOR_RESET " = quit  |  "
           COLOR_BOLD "r" COLOR_RESET " = reset stats  |  "
           COLOR_BOLD "e" COLOR_RESET " = export snapshot\n");
}
