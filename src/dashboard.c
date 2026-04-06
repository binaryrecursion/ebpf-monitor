#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dashboard.h"
#include "stats.h"

#define BAR_WIDTH 30
#define TOP_N 6
#define MAX_DISPLAY 10

/* ---------- sorting ---------- */

static int compare_rate(const void *a, const void *b)
{
    struct syscall_stat *s1 = (struct syscall_stat *)a;
    struct syscall_stat *s2 = (struct syscall_stat *)b;

    if (s2->rate > s1->rate) return 1;
    if (s2->rate < s1->rate) return -1;
    return 0;
}

static int compare_latency(const void *a, const void *b)
{
    struct syscall_stat *s1 = (struct syscall_stat *)a;
    struct syscall_stat *s2 = (struct syscall_stat *)b;

    return s2->max_latency - s1->max_latency;
}

/* ---------- visuals ---------- */

static void draw_bar(double rate, double max_rate)
{
    int bars = 0;

    if (max_rate > 0)
        bars = (int)((rate / max_rate) * BAR_WIDTH);

    for (int i = 0; i < BAR_WIDTH; i++)
        printf(i < bars ? "█" : " ");
}

/* ---------- MAIN TABLE ---------- */

static void print_main_table()
{
    printf(COLOR_BOLD "\nPROCESS        EVENT        RATE/s   AVG(us)   MAX(us)   CTXSW\n" COLOR_RESET);
    printf("---------------------------------------------------------------------\n");

    for (int i = 0; i < stat_count && i < MAX_DISPLAY; i++) {

        long avg = stats[i].count ?
            (stats[i].total_latency / stats[i].count) / 1000 : 0;

        printf("%-14s %-12s %8.1f   %s%-8ld%s %-9ld %-6ld\n",
               stats[i].process,
               stats[i].event,
               stats[i].rate,
               latency_color(avg),
               avg,
               COLOR_RESET,
               stats[i].max_latency / 1000,
               stats[i].ctx_switches);
    }
}

/* ---------- EVENT SUMMARY ---------- */

static void print_event_summary()
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
            strcpy(summary[count].event, stats[i].event);
            summary[count].count = stats[i].count;
            summary[count].total_latency = stats[i].total_latency;
            summary[count].max_latency = stats[i].max_latency;
            count++;
        } else {
            summary[found].count += stats[i].count;
            summary[found].total_latency += stats[i].total_latency;

            if (stats[i].max_latency > summary[found].max_latency)
                summary[found].max_latency = stats[i].max_latency;
        }
    }

    printf(COLOR_BOLD "\nEVENT SUMMARY\n" COLOR_RESET);
    printf("------------------------------------------------------------\n");

    for (int i = 0; i < count; i++) {

        long avg = summary[i].count ?
            (summary[i].total_latency / summary[i].count) / 1000 : 0;

        printf("%-10s | avg:%6ld us | max:%6ld us | total:%-8ld\n",
               summary[i].event,
               avg,
               summary[i].max_latency / 1000,
               summary[i].count);
    }
}

/* ---------- TOP EVENTS (aggregated) ---------- */

static void print_top_events()
{
    struct {
        char event[32];
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
            strcpy(agg[count].event, stats[i].event);
            agg[count].rate = stats[i].rate;
            count++;
        } else {
            agg[found].rate += stats[i].rate;
        }
    }

    // sort aggregated
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            if (agg[j].rate > agg[i].rate) {
                typeof(agg[0]) tmp = agg[i];
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

/* ---------- SLOWEST ---------- */

static void print_slowest_events()
{
    struct syscall_stat tmp[MAX_STATS];
    memcpy(tmp, stats, sizeof(stats));

    qsort(tmp, stat_count, sizeof(struct syscall_stat), compare_latency);

    printf(COLOR_BOLD "\nSLOWEST EVENTS\n" COLOR_RESET);
    printf("----------------------------------------\n");

    for (int i = 0; i < stat_count && i < TOP_N; i++)
        printf("%-12s max %ld us\n",
               tmp[i].event,
               tmp[i].max_latency / 1000);
}

/* ---------- ACTIVITY GRAPH (aggregated) ---------- */

static void print_activity_graph()
{
    if (stat_count == 0)
        return;

    struct syscall_stat tmp[MAX_STATS];
    memcpy(tmp, stats, sizeof(stats));

    qsort(tmp, stat_count, sizeof(struct syscall_stat), compare_rate);

    double max_rate = tmp[0].rate;

    printf(COLOR_BOLD "\nACTIVITY\n" COLOR_RESET);
    printf("----------------------------------------\n");

    for (int i = 0; i < stat_count && i < TOP_N; i++) {

        printf("%-12s ", tmp[i].event);

        printf(COLOR_GREEN);
        draw_bar(tmp[i].rate, max_rate);
        printf(COLOR_RESET);

        printf("  %6.1f/s\n", tmp[i].rate);
    }
}

/* ---------- MAIN RENDER ---------- */

void dashboard_render(double elapsed)
{
    printf("\033[2J");
    printf("\033[H");

    printf(COLOR_CYAN COLOR_BOLD);
    printf("⚡ eBPF Kernel Monitor");
    printf(COLOR_RESET);

    printf(" | Runtime: %.0fs | Events: %d\n", elapsed, stat_count);

    printf("============================================================\n");

    print_main_table();
    print_event_summary();
    print_top_events();
    print_slowest_events();
    print_activity_graph();

    printf("\nControls: q = quit | r = reset\n");
}