#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dashboard.h"
#include "stats.h"

#define BAR_WIDTH   30
#define TOP_N        6
#define MAX_DISPLAY 10

/* ------------------------------------------------------------------ */
/* Sorting comparators                                                 */
/* ------------------------------------------------------------------ */

static int compare_latency(const void *a, const void *b)
{
    const struct syscall_stat *s1 = (const struct syscall_stat *)a;
    const struct syscall_stat *s2 = (const struct syscall_stat *)b;
    if (s2->max_latency > s1->max_latency) return  1;
    if (s2->max_latency < s1->max_latency) return -1;
    return 0;
}

static int compare_deviation(const void *a, const void *b)
{
    const struct syscall_stat *s1 = (const struct syscall_stat *)a;
    const struct syscall_stat *s2 = (const struct syscall_stat *)b;
    if (s2->deviation > s1->deviation) return  1;
    if (s2->deviation < s1->deviation) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Activity bar helper                                                 */
/* ------------------------------------------------------------------ */

static void draw_bar(double rate, double max_rate)
{
    int bars = (max_rate > 0) ? (int)((rate / max_rate) * BAR_WIDTH) : 0;
    for (int i = 0; i < BAR_WIDTH; i++)
        printf(i < bars ? "\xe2\x96\x88" : " ");
}

/* ------------------------------------------------------------------ */
/* Section 1 — Main table                                             */
/* Shows top MAX_DISPLAY entries sorted by arrival order (most active */
/* processes naturally appear first as they generate more slots).     */
/* Columns: PROCESS, PID, EVENT, RATE/s, AVG(us), MAX(us), CTXSW     */
/* ------------------------------------------------------------------ */

static void print_main_table(void)
{
    printf(COLOR_BOLD
           "\nPROCESS        PID    EVENT        RATE/s   AVG(us)   MAX(us)   CTXSW\n"
           COLOR_RESET);
    printf("--------------------------------------------------------------------------\n");

    for (int i = 0; i < stat_count && i < MAX_DISPLAY; i++) {

        long avg = stats[i].count
            ? (stats[i].total_latency / stats[i].count) / 1000
            : 0;

        /* Mark anomalous rows with a leading indicator */
        const char *marker = (stats[i].is_anomaly && stats[i].baseline_ready)
            ? COLOR_RED "!" COLOR_RESET " "
            : "  ";

        printf("%s%-14s %-6d %-12s %8.1f   %s%-8ld%s %-9ld %-6ld\n",
               marker,
               stats[i].process,
               stats[i].pid,
               stats[i].event,
               stats[i].rate,
               latency_color(avg), avg, COLOR_RESET,
               stats[i].max_latency / 1000,
               stats[i].ctx_switches);
    }
}

/* ------------------------------------------------------------------ */
/* Section 2 — Event summary (aggregated across all processes)        */
/* ------------------------------------------------------------------ */

static void print_event_summary(void)
{
    struct {
        char event[32];
        long count;
        long total_latency;
        long max_latency;
    } summary[32];

    int count = 0;

    for (int i = 0; i < stat_count; i++) {
        int found = -1;
        for (int j = 0; j < count; j++) {
            if (strcmp(summary[j].event, stats[i].event) == 0) {
                found = j;
                break;
            }
        }

        if (found == -1) {
            if (count >= 32) continue;
            strncpy(summary[count].event, stats[i].event,
                    sizeof(summary[count].event) - 1);
            summary[count].event[sizeof(summary[count].event) - 1] = '\0';
            summary[count].count         = stats[i].count;
            summary[count].total_latency = stats[i].total_latency;
            summary[count].max_latency   = stats[i].max_latency;
            count++;
        } else {
            summary[found].count         += stats[i].count;
            summary[found].total_latency += stats[i].total_latency;
            if (stats[i].max_latency > summary[found].max_latency)
                summary[found].max_latency = stats[i].max_latency;
        }
    }

    printf(COLOR_BOLD "\nEVENT SUMMARY\n" COLOR_RESET);
    printf("------------------------------------------------------------\n");

    for (int i = 0; i < count; i++) {
        long avg = summary[i].count
            ? (summary[i].total_latency / summary[i].count) / 1000
            : 0;

        printf("%-10s | avg:%6ld us | max:%6ld us | total:%-8ld\n",
               summary[i].event,
               avg,
               summary[i].max_latency / 1000,
               summary[i].count);
    }
}

/* ------------------------------------------------------------------ */
/* Section 3 — Top events by aggregated rate                          */
/* ------------------------------------------------------------------ */

static void print_top_events(void)
{
    struct {
        char   event[32];
        double rate;
    } agg[32];

    int count = 0;

    for (int i = 0; i < stat_count; i++) {
        int found = -1;
        for (int j = 0; j < count; j++) {
            if (strcmp(agg[j].event, stats[i].event) == 0) {
                found = j;
                break;
            }
        }

        if (found == -1) {
            if (count >= 32) continue;
            strncpy(agg[count].event, stats[i].event,
                    sizeof(agg[count].event) - 1);
            agg[count].event[sizeof(agg[count].event) - 1] = '\0';
            agg[count].rate = stats[i].rate;
            count++;
        } else {
            agg[found].rate += stats[i].rate;
        }
    }

    /* Sort descending by rate */
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            if (agg[j].rate > agg[i].rate) {
                __typeof__(agg[0]) tmp = agg[i];
                agg[i] = agg[j];
                agg[j] = tmp;
            }
        }
    }

    printf(COLOR_BOLD "\nTOP EVENTS\n" COLOR_RESET);
    printf("----------------------------------------\n");

    for (int i = 0; i < count && i < TOP_N; i++)
        printf("%-12s %8.1f/s\n", agg[i].event, agg[i].rate);
}

/* ------------------------------------------------------------------ */
/* Section 4 — Slowest events by max latency                          */
/* ------------------------------------------------------------------ */

static void print_slowest_events(void)
{
    struct syscall_stat tmp[MAX_STATS];
    memcpy(tmp, stats, stat_count * sizeof(struct syscall_stat));
    qsort(tmp, stat_count, sizeof(struct syscall_stat), compare_latency);

    printf(COLOR_BOLD "\nSLOWEST EVENTS\n" COLOR_RESET);
    printf("----------------------------------------\n");

    for (int i = 0; i < stat_count && i < TOP_N; i++)
        printf("%-14s %-12s max %s%ld us%s\n",
               tmp[i].process,
               tmp[i].event,
               latency_color(tmp[i].max_latency / 1000),
               tmp[i].max_latency / 1000,
               COLOR_RESET);
}

/* ------------------------------------------------------------------ */
/* Section 5 — Activity graph                                         */
/* Aggregated by event type (same fix as TOP EVENTS) so we never get  */
/* duplicate "sched" bars.                                            */
/* ------------------------------------------------------------------ */

static void print_activity_graph(void)
{
    if (stat_count == 0)
        return;

    /* Aggregate rate by event type */
    struct {
        char   event[32];
        double rate;
    } agg[32];

    int count = 0;

    for (int i = 0; i < stat_count; i++) {
        int found = -1;
        for (int j = 0; j < count; j++) {
            if (strcmp(agg[j].event, stats[i].event) == 0) {
                found = j;
                break;
            }
        }

        if (found == -1) {
            if (count >= 32) continue;
            strncpy(agg[count].event, stats[i].event,
                    sizeof(agg[count].event) - 1);
            agg[count].event[sizeof(agg[count].event) - 1] = '\0';
            agg[count].rate = stats[i].rate;
            count++;
        } else {
            agg[found].rate += stats[i].rate;
        }
    }

    /* Sort descending */
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            if (agg[j].rate > agg[i].rate) {
                __typeof__(agg[0]) tmp = agg[i];
                agg[i] = agg[j];
                agg[j] = tmp;
            }
        }
    }

    double max_rate = (count > 0) ? agg[0].rate : 1.0;

    printf(COLOR_BOLD "\nACTIVITY\n" COLOR_RESET);
    printf("----------------------------------------\n");

    for (int i = 0; i < count && i < TOP_N; i++) {
        printf("%-12s ", agg[i].event);
        printf(COLOR_GREEN);
        draw_bar(agg[i].rate, max_rate);
        printf(COLOR_RESET);
        printf("  %6.1f/s\n", agg[i].rate);
    }
}

/* ------------------------------------------------------------------ */
/* Section 6 — Adaptive baseline anomaly alerts                       */
/* Only shows entries where deviation > ANOMALY_THRESHOLD.            */
/* Sorted by deviation descending so worst offenders are on top.      */
/* ------------------------------------------------------------------ */

static void print_anomaly_alerts(void)
{
    struct syscall_stat tmp[MAX_STATS];
    memcpy(tmp, stats, stat_count * sizeof(struct syscall_stat));
    qsort(tmp, stat_count, sizeof(struct syscall_stat), compare_deviation);

    int n_anomalies = 0;
    for (int i = 0; i < stat_count; i++) {
        if (tmp[i].is_anomaly && tmp[i].baseline_ready)
            n_anomalies++;
    }

    printf(COLOR_BOLD "\nADAPTIVE BASELINE \xe2\x80\x94 ANOMALY ALERTS\n" COLOR_RESET);
    printf("------------------------------------------------------------\n");

    if (n_anomalies == 0) {
        printf(COLOR_GREEN "  All events within normal range.\n" COLOR_RESET);
        return;
    }

    printf(COLOR_BOLD
           "  %-14s %-12s  DEVIATION  BASELINE(us)  CURRENT(us)\n"
           COLOR_RESET,
           "PROCESS", "EVENT");

    int shown = 0;
    for (int i = 0; i < stat_count && shown < TOP_N; i++) {
        if (!tmp[i].is_anomaly || !tmp[i].baseline_ready)
            continue;

        long baseline_us = (long)(tmp[i].baseline_latency / 1000.0);
        long current_us  = tmp[i].count
            ? (tmp[i].total_latency / tmp[i].count) / 1000
            : 0;

        printf("  %-14s %-12s  %s%6.1f%%%s   %8ld        %8ld\n",
               tmp[i].process,
               tmp[i].event,
               deviation_color(tmp[i].deviation),
               tmp[i].deviation * 100.0,
               COLOR_RESET,
               baseline_us,
               current_us);
        shown++;
    }
}

/* ------------------------------------------------------------------ */
/* Main render entry point                                            */
/* ------------------------------------------------------------------ */

void dashboard_render(double elapsed)
{
    /* Clear screen, cursor to top-left */
    printf("\033[2J\033[H");

    /* Header */
    printf(COLOR_CYAN COLOR_BOLD "\xe2\x9a\xa1 eBPF Kernel Monitor" COLOR_RESET);
    printf(" | Runtime: %.0fs | Tracked: %d entries\n", elapsed, stat_count);
    printf("============================================================\n");
    printf(COLOR_DIM "  ! = active anomaly   latency: "
           COLOR_GREEN "green<10us " COLOR_RESET COLOR_DIM
           COLOR_YELLOW "yellow<100us " COLOR_RESET COLOR_DIM
           COLOR_RED "red>=100us\n" COLOR_RESET);

    print_main_table();
    print_event_summary();
    print_top_events();
    print_slowest_events();
    print_activity_graph();
    print_anomaly_alerts();

    printf("\nControls: " COLOR_BOLD "q" COLOR_RESET " = quit  |  "
           COLOR_BOLD "r" COLOR_RESET " = reset stats\n");
}