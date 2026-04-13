#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dashboard.h"
#include "stats.h"

/* ================================================================== */
/*  DESIGN CONSTANTS                                                   */
/* ================================================================== */

#define BAR_WIDTH     28
#define TOP_N          6
#define MAX_DISPLAY   12
#define TERM_WIDTH    104   /* assumed terminal width */

/* ================================================================== */
/*  EXTENDED COLOR / STYLE PALETTE                                     */
/* ================================================================== */

/* Base */
#define C_RESET     "\033[0m"
#define C_BOLD      "\033[1m"
#define C_DIM       "\033[2m"
#define C_ITALIC    "\033[3m"

/* Foreground */
#define C_WHITE     "\033[97m"
#define C_BLACK     "\033[30m"
#define C_RED       "\033[91m"
#define C_GREEN     "\033[92m"
#define C_YELLOW    "\033[93m"
#define C_BLUE      "\033[94m"
#define C_MAGENTA   "\033[95m"
#define C_CYAN      "\033[96m"
#define C_ORANGE    "\033[38;5;208m"
#define C_TEAL      "\033[38;5;43m"
#define C_PURPLE    "\033[38;5;141m"
#define C_PINK      "\033[38;5;213m"
#define C_LGRAY     "\033[38;5;245m"
#define C_DGRAY     "\033[38;5;238m"

/* Background */
#define BG_HEADER   "\033[48;5;235m"
#define BG_ROW_ALT  "\033[48;5;233m"
#define BG_RED      "\033[41m"
#define BG_GREEN    "\033[42m"

/* Box-drawing characters (UTF-8) */
#define BOX_TL      "\xe2\x94\x8c"   /* ┌ */
#define BOX_TR      "\xe2\x94\x90"   /* ┐ */
#define BOX_BL      "\xe2\x94\x94"   /* └ */
#define BOX_BR      "\xe2\x94\x98"   /* ┘ */
#define BOX_H       "\xe2\x94\x80"   /* ─ */
#define BOX_V       "\xe2\x94\x82"   /* │ */
#define BOX_TM      "\xe2\x94\xac"   /* ┬ */
#define BOX_BM      "\xe2\x94\xb4"   /* ┴ */
#define BOX_ML      "\xe2\x94\x9c"   /* ├ */
#define BOX_MR      "\xe2\x94\xa4"   /* ┤ */
#define BOX_CR      "\xe2\x94\xbc"   /* ┼ */
#define BOX_DH      "\xe2\x95\x90"   /* ═ */
#define BOX_DTL     "\xe2\x95\x94"   /* ╔ */
#define BOX_DTR     "\xe2\x95\x97"   /* ╗ */
#define BOX_DBL     "\xe2\x95\x9a"   /* ╚ */
#define BOX_DBR     "\xe2\x95\x9d"   /* ╝ */
#define BOX_DV      "\xe2\x95\x91"   /* ║ */

/* Block / bar chars */
#define BLOCK_FULL  "\xe2\x96\x88"   /* █ */
#define BLOCK_75    "\xe2\x96\x93"   /* ▓ */
#define BLOCK_50    "\xe2\x96\x92"   /* ▒ */
#define BLOCK_25    "\xe2\x96\x91"   /* ░ */
#define BLOCK_L     "\xe2\x96\x8c"   /* ▌ left-half */
#define TRIANGLE    "\xe2\x96\xb6"   /* ▶ */
#define BULLET      "\xe2\x80\xa2"   /* • */
#define DIAMOND     "\xe2\x97\x86"   /* ◆ */
#define ARROW_UP    "\xe2\x86\x91"   /* ↑ */
#define ARROW_R     "\xe2\x86\x92"   /* → */
#define SPARK_CHARS "\xe2\x96\x81\xe2\x96\x82\xe2\x96\x83\xe2\x96\x84\xe2\x96\x85\xe2\x96\x86\xe2\x96\x87\xe2\x96\x88"

/* Event-type badge colors */
static const char *event_color(const char *ev)
{
    if (strcmp(ev, "write")     == 0) return C_CYAN;
    if (strcmp(ev, "read")      == 0) return C_TEAL;
    if (strcmp(ev, "openat")    == 0) return C_BLUE;
    if (strcmp(ev, "close")     == 0) return C_PURPLE;
    if (strcmp(ev, "mmap")      == 0) return C_ORANGE;
    if (strcmp(ev, "sched")     == 0) return C_YELLOW;
    if (strcmp(ev, "exec")      == 0) return C_GREEN;
    if (strcmp(ev, "lifecycle") == 0) return C_MAGENTA;
    return C_LGRAY;
}

/* ================================================================== */
/*  HELPERS                                                            */
/* ================================================================== */

static void hline(int width)
{
    for (int i = 0; i < width; i++) printf(BOX_H);
}

/* Print a titled box header  ┌─── TITLE ──────────┐  */
static void box_header(const char *title, int width, const char *title_color)
{
    int tlen = (int)strlen(title);
    int inner = width - 2;          /* subtract left/right corners */
    int left_dashes = 2;
    int right_dashes = inner - left_dashes - 1 - tlen - 1; /* spaces */
    if (right_dashes < 0) right_dashes = 0;

    printf(C_DGRAY BOX_TL);
    for (int i = 0; i < left_dashes; i++) printf(BOX_H);
    printf(C_RESET " %s%s%s%s " C_DGRAY, C_BOLD, title_color, title, C_RESET C_DGRAY);
    for (int i = 0; i < right_dashes; i++) printf(BOX_H);
    printf(BOX_TR C_RESET "\n");
}

static void box_footer(int width)
{
    printf(C_DGRAY BOX_BL);
    hline(width - 2);
    printf(BOX_BR C_RESET "\n");
}

static void box_divider(int width)
{
    printf(C_DGRAY BOX_ML);
    hline(width - 2);
    printf(BOX_MR C_RESET "\n");
}

/* Left-padded number with K/M suffix */
static void fmt_rate(char *buf, int len, double r)
{
    if (r >= 1e6)       snprintf(buf, len, "%6.1fM", r / 1e6);
    else if (r >= 1e3)  snprintf(buf, len, "%6.1fK", r / 1e3);
    else                snprintf(buf, len, "%6.0f ", r);
}

static void fmt_us(char *buf, int len, long us)
{
    if (us < 0)             snprintf(buf, len, "   --");
    else if (us >= 1000000) snprintf(buf, len, "%4.1fs", us / 1e6);
    else if (us >= 1000)    snprintf(buf, len, "%4.1fm", us / 1e3);
    else                    snprintf(buf, len, "%4ldµ", us);
}

static const char *lat_color(long us)
{
    if (us < 0)               return C_LGRAY;
    if (us < LATENCY_GREEN_US)  return C_GREEN;
    if (us < LATENCY_YELLOW_US) return C_YELLOW;
    if (us < 1000)              return C_ORANGE;
    return C_RED;
}

static const char *dev_color(double d)
{
    if (d < 0.25)              return C_GREEN;
    if (d < ANOMALY_THRESHOLD) return C_YELLOW;
    if (d < 1.5)               return C_ORANGE;
    return C_RED;
}

/* ================================================================== */
/*  AGGREGATION (unchanged logic, same as original)                   */
/* ================================================================== */

typedef struct {
    char   event[32];
    long   count;
    long   total_latency;
    long   max_latency;
    long   valid_count;
    double rate;
} agg_entry;

static int build_event_agg(agg_entry *agg, int max_agg)
{
    int count = 0;
    for (int i = 0; i < stat_count; i++) {
        int found = -1;
        for (int j = 0; j < count; j++)
            if (strcmp(agg[j].event, stats[i].event) == 0) { found = j; break; }
        if (found == -1) {
            if (count >= max_agg) continue;
            strncpy(agg[count].event, stats[i].event, sizeof(agg[0].event) - 1);
            agg[count].event[sizeof(agg[0].event)-1] = '\0';
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
                agg_entry t = agg[i]; agg[i] = agg[j]; agg[j] = t;
            }
}

static int cmp_latency(const void *a, const void *b)
{
    const struct syscall_stat *s1 = a, *s2 = b;
    return (s2->max_latency > s1->max_latency) ? 1 : -1;
}
static int cmp_deviation(const void *a, const void *b)
{
    const struct syscall_stat *s1 = a, *s2 = b;
    return (s2->deviation > s1->deviation) ? 1 : -1;
}
static int cmp_rate(const void *a, const void *b)
{
    const struct syscall_stat *s1 = a, *s2 = b;
    return (s2->rate > s1->rate) ? 1 : -1;
}

/* ================================================================== */
/*  SECTION 0 — HEADER BANNER                                         */
/* ================================================================== */

static void print_header(double elapsed, double cpu_pct,
                         int active_pid, const char *active_comm,
                         long active_min_ms)
{
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm);

    int h = (int)(elapsed / 3600);
    int m = ((int)elapsed % 3600) / 60;
    int s = (int)elapsed % 60;

    /* ╔══ title bar ══╗ */
    printf(C_DGRAY BOX_DTL);
    for (int i = 0; i < TERM_WIDTH - 2; i++) printf(BOX_DH);
    printf(BOX_DTR C_RESET "\n");

    /* ║  ⚡ title  |  stats  |  time  ║ */
    printf(C_DGRAY BOX_DV C_RESET);
    printf("  " C_CYAN C_BOLD "\xe2\x9a\xa1 eBPF Kernel Monitor" C_RESET
           C_LGRAY "  v7.0" C_RESET);

    /* runtime */
    printf("    " C_DGRAY BULLET C_RESET
           "  " C_WHITE "Runtime" C_RESET
           " " C_YELLOW "%02d:%02d:%02d" C_RESET, h, m, s);

    /* cpu */
    const char *cpu_col = cpu_pct < 5.0 ? C_GREEN :
                          cpu_pct < 30.0 ? C_YELLOW :
                          cpu_pct < 70.0 ? C_ORANGE : C_RED;
    printf("    " C_DGRAY BULLET C_RESET
           "  " C_WHITE "CPU" C_RESET " %s%.2f%%%s", cpu_col, cpu_pct, C_RESET);

    /* slots */
    double slot_pct = (double)stat_count / MAX_STATS * 100.0;
    const char *slot_col = slot_pct < 50 ? C_GREEN :
                           slot_pct < 80 ? C_YELLOW : C_RED;
    printf("    " C_DGRAY BULLET C_RESET
           "  " C_WHITE "Slots" C_RESET " %s%d/%d%s",
           slot_col, stat_count, MAX_STATS, C_RESET);

    if (total_events_dropped > 0)
        printf("  " C_RED C_BOLD " DROPPED:%ld" C_RESET, total_events_dropped);

    /* time right-aligned roughly */
    printf("    " C_DGRAY BULLET C_RESET
           "  " C_LGRAY "%s" C_RESET, timebuf);

    printf("\n");

    /* ╠══ filter bar (only if filters active) ══╣ */
    int any_filter = (active_pid || active_comm[0] || active_min_ms);
    if (any_filter) {
        printf(C_DGRAY BOX_ML);
        for (int i = 0; i < TERM_WIDTH - 2; i++) printf(BOX_H);
        printf(BOX_MR C_RESET "\n");
        printf(C_DGRAY BOX_DV C_RESET);
        printf("  " C_BOLD C_MAGENTA DIAMOND " ACTIVE FILTERS:" C_RESET);
        if (active_pid)    printf("  " C_MAGENTA "pid" C_RESET "=%d", active_pid);
        if (active_comm[0]) printf("  " C_MAGENTA "comm" C_RESET "=%s", active_comm);
        if (active_min_ms) printf("  " C_MAGENTA "min-dur" C_RESET "=%ldms", active_min_ms);
        printf(C_LGRAY "  (kernel-side — zero overhead for excluded processes)" C_RESET "\n");
    }

    /* ╠══ legend ══╣ */
    printf(C_DGRAY BOX_ML);
    for (int i = 0; i < TERM_WIDTH - 2; i++) printf(BOX_H);
    printf(BOX_MR C_RESET "\n");

    printf(C_DGRAY BOX_DV C_RESET);
    printf("  " C_LGRAY "Latency: "
           C_GREEN BLOCK_FULL " <10µs  "
           C_YELLOW BLOCK_FULL " <100µs  "
           C_ORANGE BLOCK_FULL " <1ms  "
           C_RED BLOCK_FULL " ≥1ms" C_RESET
           C_LGRAY "     "
           C_RED "!" C_RESET C_LGRAY " = anomaly     "
           "sched avg/p95 exclude sleep noise >200ms"
           C_RESET "\n");

    /* ╚══╝ */
    printf(C_DGRAY BOX_DBL);
    for (int i = 0; i < TERM_WIDTH - 2; i++) printf(BOX_DH);
    printf(BOX_DBR C_RESET "\n");
}

/* ================================================================== */
/*  SECTION 1 — MAIN PROCESS TABLE                                    */
/* ================================================================== */

static void print_main_table(void)
{
    struct syscall_stat tmp[MAX_STATS];
    memcpy(tmp, stats, stat_count * sizeof(*tmp));
    qsort(tmp, stat_count, sizeof(*tmp), cmp_rate);

    int W = TERM_WIDTH;
    box_header("PROCESSES", W, C_CYAN);

    /* Column header row */
    printf(C_DGRAY BOX_V C_RESET);
    printf(BG_HEADER C_BOLD C_WHITE
           "  %-15s %-7s %-11s %8s  %6s  %6s  %8s  %6s  %6s  "
           C_RESET "\n",
           "PROCESS", "PID", "EVENT",
           "RATE/s", "AVG", "P95", "MAX", "CTXSW", "EXECS");

    box_divider(W);

    int shown = 0;
    for (int i = 0; i < stat_count && shown < MAX_DISPLAY; i++) {
        struct syscall_stat *s = &tmp[i];

        long valid = s->count - s->drop_count;
        long avg   = (valid > 0 && s->total_latency > 0)
                     ? (s->total_latency / valid) / 1000 : 0;
        long p95   = stats_p95_us(s);
        long maxus = s->max_latency / 1000;

        char rate_s[16], avg_s[12], p95_s[12], max_s[12];
        fmt_rate(rate_s, sizeof(rate_s), s->rate);
        fmt_us(avg_s,  sizeof(avg_s),  avg);
        fmt_us(p95_s,  sizeof(p95_s),  p95);
        fmt_us(max_s,  sizeof(max_s),  maxus);

        const char *anom = (s->is_anomaly && s->baseline_ready)
            ? C_RED C_BOLD "!" C_RESET : " ";

        /* Alternate row bg for readability */
        const char *row_bg = (shown % 2 == 1) ? BG_ROW_ALT : "";
        const char *row_reset = (shown % 2 == 1) ? C_RESET : "";

        printf(C_DGRAY BOX_V C_RESET);
        printf("%s%s %-15s " C_LGRAY "%-7d" C_RESET "%s ",
               row_bg, anom, s->process, s->pid, row_bg);

        /* Event badge */
        printf("%s%-11s" C_RESET "%s", event_color(s->event), s->event, row_bg);

        /* Rate */
        printf(C_BOLD C_WHITE " %8s" C_RESET "%s  ", rate_s, row_bg);

        /* AVG latency */
        printf("%s%6s" C_RESET "%s  ", lat_color(avg), avg_s, row_bg);

        /* P95 latency */
        printf("%s%6s" C_RESET "%s  ", lat_color(p95), p95_s, row_bg);

        /* MAX latency */
        printf("%s%8s" C_RESET "%s  ", lat_color(maxus), max_s, row_bg);

        /* CTXSW / EXECS */
        if (s->ctx_switches > 0)
            printf(C_YELLOW "%6ld" C_RESET "%s  ", s->ctx_switches, row_bg);
        else
            printf(C_LGRAY "%6ld" C_RESET "%s  ", s->ctx_switches, row_bg);

        if (s->exec_count > 0)
            printf(C_GREEN "%6ld" C_RESET "%s", s->exec_count, row_bg);
        else
            printf(C_LGRAY "%6ld" C_RESET "%s", s->exec_count, row_bg);

        printf("%s\n", row_reset);
        shown++;
    }

    if (stat_count > MAX_DISPLAY) {
        printf(C_DGRAY BOX_V C_RESET);
        printf(C_DIM "  " TRIANGLE " %d more entries hidden  "
               C_LGRAY "(showing top %d by rate)" C_RESET "\n",
               stat_count - MAX_DISPLAY, MAX_DISPLAY);
    }

    if (total_events_dropped > 0) {
        box_divider(W);
        printf(C_DGRAY BOX_V C_RESET);
        printf(C_RED C_BOLD "  ⚠  %ld events dropped — slot table full (%d). "
               "Press " C_WHITE "r" C_RED " to reset.\n" C_RESET,
               total_events_dropped, MAX_STATS);
    }

    box_footer(W);
}

/* ================================================================== */
/*  SECTION 2 — EVENT SUMMARY                                         */
/* ================================================================== */

static void print_event_summary(void)
{
    agg_entry agg[32];
    int count = build_event_agg(agg, 32);

    int W = TERM_WIDTH;
    box_header("EVENT SUMMARY", W, C_TEAL);

    printf(C_DGRAY BOX_V C_RESET);
    printf(BG_HEADER C_BOLD C_WHITE
           "  %-11s  %8s  %8s  %12s  %10s  "
           C_RESET "\n",
           "EVENT", "AVG", "MAX", "TOTAL", "RATE/s");

    box_divider(W);

    for (int i = 0; i < count; i++) {
        long avg   = (agg[i].valid_count > 0 && agg[i].total_latency > 0)
                     ? (agg[i].total_latency / agg[i].valid_count) / 1000 : 0;
        long maxus = agg[i].max_latency / 1000;

        int is_outlier = (avg > 0 && maxus > avg * 10);

        char avg_s[12], max_s[12], rate_s[16];
        fmt_us(avg_s,  sizeof(avg_s),  avg);
        fmt_us(max_s,  sizeof(max_s),  maxus);
        fmt_rate(rate_s, sizeof(rate_s), agg[i].rate);

        printf(C_DGRAY BOX_V C_RESET);
        printf("  %s%-11s" C_RESET "  ", event_color(agg[i].event), agg[i].event);
        printf("%s%8s" C_RESET "  ", lat_color(avg),   avg_s);
        printf("%s%8s" C_RESET "  ", lat_color(maxus), max_s);
        printf(C_WHITE "%12ld" C_RESET "  ", agg[i].count);
        printf(C_BOLD C_WHITE "%10s" C_RESET, rate_s);
        if (is_outlier)
            printf("  " C_YELLOW C_DIM "⚠ outlier" C_RESET);
        printf("\n");
    }

    /* footnote */
    int has_outlier = 0;
    for (int i = 0; i < count; i++) {
        long avg   = (agg[i].valid_count > 0 && agg[i].total_latency > 0)
                     ? (agg[i].total_latency / agg[i].valid_count) / 1000 : 0;
        long maxus = agg[i].max_latency / 1000;
        if (avg > 0 && maxus > avg * 10) { has_outlier = 1; break; }
    }
    if (has_outlier) {
        printf(C_DGRAY BOX_V C_RESET);
        printf(C_DIM C_YELLOW "  ⚠ outlier" C_RESET
               C_DIM " = MAX >> AVG: single spike skewing aggregate. "
               "Per-process table above is more accurate." C_RESET "\n");
    }

    box_footer(W);
}

/* ================================================================== */
/*  SECTION 3 + 5 — ACTIVITY GRAPH  (merged with top-events)          */
/* ================================================================== */

static void print_activity_graph(void)
{
    if (stat_count == 0) return;

    agg_entry agg[32];
    int count = build_event_agg(agg, 32);
    sort_agg_by_rate(agg, count);

    double max_rate = (count > 0 && agg[0].rate > 0) ? agg[0].rate : 1.0;

    int W = TERM_WIDTH;
    box_header("ACTIVITY  " ARROW_UP " rate/s", W, C_ORANGE);

    /* Gradient bar colors by fill level */
    static const char *bar_colors[] = {
        C_GREEN, C_GREEN, C_TEAL, C_CYAN,
        C_BLUE, C_YELLOW, C_ORANGE, C_RED
    };

    for (int i = 0; i < count && i < TOP_N; i++) {
        int bars = (int)((agg[i].rate / max_rate) * BAR_WIDTH);
        if (bars > BAR_WIDTH) bars = BAR_WIDTH;

        char rate_s[16];
        fmt_rate(rate_s, sizeof(rate_s), agg[i].rate);

        /* Colored fill — full blocks for filled, dim for empty */
        int seg = (bars * 8) / (BAR_WIDTH > 0 ? BAR_WIDTH : 1);
        if (seg > 7) seg = 7;
        const char *bcol = bar_colors[seg];

        printf(C_DGRAY BOX_V C_RESET);
        printf("  %s%-11s" C_RESET "  ", event_color(agg[i].event), agg[i].event);

        printf("%s", bcol);
        for (int b = 0; b < BAR_WIDTH; b++) {
            if (b < bars)
                printf(BLOCK_FULL);
            else
                printf(C_DGRAY BLOCK_25 "%s", bcol);
        }
        printf(C_RESET);

        printf("  " C_BOLD C_WHITE "%s/s" C_RESET "\n", rate_s);
    }

    box_footer(W);
}

/* ================================================================== */
/*  SECTION 4 — SLOWEST SYSCALLS                                      */
/* ================================================================== */

static void print_slowest_syscalls(void)
{
    struct syscall_stat tmp[MAX_STATS];
    int n = 0;

    for (int i = 0; i < stat_count; i++) {
        const char *ev = stats[i].event;
        if (strcmp(ev, "sched")     == 0) continue;
        if (strcmp(ev, "exec")      == 0) continue;
        if (strcmp(ev, "lifecycle") == 0) continue;
        if (n < MAX_STATS) tmp[n++] = stats[i];
    }

    if (n == 0) return;

    qsort(tmp, n, sizeof(*tmp), cmp_latency);

    int W = TERM_WIDTH;
    box_header("SLOWEST SYSCALLS", W, C_RED);

    printf(C_DGRAY BOX_V C_RESET);
    printf(BG_HEADER C_BOLD C_WHITE
           "  %-15s  %-10s  %12s  %10s  %10s  "
           C_RESET "\n",
           "PROCESS", "SYSCALL", "MAX", "P95", "AVG");
    box_divider(W);

    for (int i = 0; i < n && i < TOP_N; i++) {
        long maxus = tmp[i].max_latency / 1000;
        long p95   = stats_p95_us(&tmp[i]);
        long valid = tmp[i].count - tmp[i].drop_count;
        long avg   = (valid > 0 && tmp[i].total_latency > 0)
                     ? (tmp[i].total_latency / valid) / 1000 : 0;

        char max_s[12], p95_s[12], avg_s[12];
        fmt_us(max_s, sizeof(max_s), maxus);
        fmt_us(p95_s, sizeof(p95_s), p95);
        fmt_us(avg_s, sizeof(avg_s), avg);

        printf(C_DGRAY BOX_V C_RESET);
        printf("  " C_WHITE "%-15s" C_RESET "  ", tmp[i].process);
        printf("%s%-10s" C_RESET "  ", event_color(tmp[i].event), tmp[i].event);
        printf("%s%12s" C_RESET "  ", lat_color(maxus), max_s);
        printf("%s%10s" C_RESET "  ", lat_color(p95),   p95_s);
        printf("%s%10s" C_RESET "\n", lat_color(avg),   avg_s);
    }

    box_footer(W);
}

/* ================================================================== */
/*  SECTION 6 — ANOMALY ALERTS                                        */
/* ================================================================== */

static void print_anomaly_alerts(void)
{
    struct syscall_stat tmp[MAX_STATS];
    memcpy(tmp, stats, stat_count * sizeof(*tmp));
    qsort(tmp, stat_count, sizeof(*tmp), cmp_deviation);

    int n_anom = 0;
    for (int i = 0; i < stat_count; i++)
        if (tmp[i].is_anomaly && tmp[i].baseline_ready) n_anom++;

    int W = TERM_WIDTH;
    box_header("ANOMALY DETECTION  \xe2\x80\x94  EMA BASELINE", W, C_RED);

    if (n_anom == 0) {
        printf(C_DGRAY BOX_V C_RESET);
        printf("  " C_GREEN C_BOLD "\xe2\x9c\x94" C_RESET
               C_GREEN "  All events within normal baseline range.\n" C_RESET);
        box_footer(W);
        return;
    }

    printf(C_DGRAY BOX_V C_RESET);
    printf(BG_HEADER C_BOLD C_WHITE
           "  %-15s  %-11s  %10s  %12s  %12s  "
           C_RESET "\n",
           "PROCESS", "EVENT", "DEVIATION", "BASELINE", "CURRENT");
    box_divider(W);

    int shown = 0;
    for (int i = 0; i < stat_count && shown < TOP_N; i++) {
        if (!tmp[i].is_anomaly || !tmp[i].baseline_ready) continue;

        long valid      = tmp[i].count - tmp[i].drop_count;
        long current_us = (valid > 0 && tmp[i].total_latency > 0)
                          ? (tmp[i].total_latency / valid) / 1000 : 0;
        long baseline_us = (long)(tmp[i].baseline_latency / 1000.0);

        char cur_s[12], bas_s[12];
        fmt_us(cur_s, sizeof(cur_s), current_us);
        fmt_us(bas_s, sizeof(bas_s), baseline_us);

        const char *dc = dev_color(tmp[i].deviation);

        /* deviation bar (mini, 10 chars) */
        int dbar = (int)(tmp[i].deviation * 5);
        if (dbar > 10) dbar = 10;

        printf(C_DGRAY BOX_V C_RESET);
        printf("  " C_WHITE "%-15s" C_RESET "  ", tmp[i].process);
        printf("%s%-11s" C_RESET "  ", event_color(tmp[i].event), tmp[i].event);

        /* deviation % with mini spark */
        printf("%s" C_BOLD "%8.1f%%" C_RESET "  ", dc, tmp[i].deviation * 100.0);

        printf(C_LGRAY "%12s" C_RESET "  ", bas_s);
        printf("%s%12s" C_RESET "\n", dc, cur_s);

        shown++;
    }

    if (n_anom > TOP_N) {
        printf(C_DGRAY BOX_V C_RESET);
        printf(C_DIM "  " TRIANGLE " %d more anomalies not shown\n" C_RESET,
               n_anom - TOP_N);
    }

    box_footer(W);
}

/* ================================================================== */
/*  SECTION 7 — PROCESS LIFECYCLE                                     */
/* ================================================================== */

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
            strcmp(s->event, "lifecycle") != 0) continue;

        int idx = -1;
        for (int j = 0; j < lc_count; j++)
            if (strcmp(procs[j].process, s->process) == 0) { idx = j; break; }

        if (idx == -1) {
            if (lc_count >= 64) continue;
            idx = lc_count++;
            memset(&procs[idx], 0, sizeof(procs[0]));
            strncpy(procs[idx].process, s->process, sizeof(procs[0].process)-1);
            procs[idx].pid = s->pid;
        }
        if (strcmp(s->event, "exec") == 0)
            procs[idx].execs = s->exec_count + s->count;
        else {
            procs[idx].exits = s->count;
            procs[idx].avg_life_us = s->count
                ? (s->total_latency / s->count) / 1000 : 0;
        }
    }

    if (lc_count == 0) return;

    int W = TERM_WIDTH;
    box_header("PROCESS LIFECYCLE", W, C_MAGENTA);

    printf(C_DGRAY BOX_V C_RESET);
    printf(BG_HEADER C_BOLD C_WHITE
           "  %-15s  %-7s  %10s  %10s  %14s  "
           C_RESET "\n",
           "PROCESS", "PID", "EXECS", "EXITS", "AVG LIFETIME");
    box_divider(W);

    for (int i = 0; i < lc_count; i++) {
        char life_s[16];
        fmt_us(life_s, sizeof(life_s), procs[i].avg_life_us);

        printf(C_DGRAY BOX_V C_RESET);
        printf("  " C_WHITE "%-15s" C_RESET "  ", procs[i].process);
        printf(C_LGRAY "%-7d" C_RESET "  ", procs[i].pid);
        printf(C_GREEN  "%10ld" C_RESET "  ", procs[i].execs);
        printf(C_YELLOW "%10ld" C_RESET "  ", procs[i].exits);
        printf(C_CYAN   "%14s" C_RESET "\n",  life_s);
    }

    box_footer(W);
}

/* ================================================================== */
/*  FOOTER / CONTROLS BAR                                             */
/* ================================================================== */

static void print_controls(void)
{
    printf("\n");
    printf(BG_HEADER C_DGRAY "  ");
    printf(C_BOLD C_WHITE "[q]" C_RESET BG_HEADER C_LGRAY " quit   ");
    printf(C_BOLD C_WHITE "[r]" C_RESET BG_HEADER C_LGRAY " reset stats & baselines   ");
    printf(C_BOLD C_WHITE "[e]" C_RESET BG_HEADER C_LGRAY " export snapshot (JSON+CSV)");
    printf(C_LGRAY "                                                  " C_RESET "\n");
}

/* ================================================================== */
/*  MAIN RENDER ENTRY POINT                                           */
/* ================================================================== */

void dashboard_render(double elapsed, double cpu_pct,
                      int active_pid, const char *active_comm,
                      long active_min_ms)
{
    /* Move cursor to top-left without full clear to reduce flicker */
    printf("\033[H\033[J");

    print_header(elapsed, cpu_pct, active_pid, active_comm, active_min_ms);
    print_main_table();
    print_event_summary();
    print_activity_graph();
    print_slowest_syscalls();
    print_anomaly_alerts();
    print_process_summary();
    print_controls();

    fflush(stdout);
}