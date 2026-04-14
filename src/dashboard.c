/* SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause) */

/*
 * dashboard.c — btop-style TUI renderer
 *
 * Architecture:
 *   - vscreen virtual buffer (no flicker, diff-only updates)
 *   - layout_t panel grid (fully responsive to terminal size)
 *   - All drawing goes through vscreen_put / vscreen_printf
 *   - vscreen_flush() at the end emits only changed cells
 *
 * Fix summary vs original:
 *   1. draw_panel() flood-fills the entire rect before drawing borders
 *      → eliminates all ghost / leftover-cell artifacts
 *   2. Adaptive column widths in draw_processes() based on panel W
 *      → no more column collisions on narrow terminals
 *   3. Process name is bold (primary identifier); rate is normal weight
 *      → proper visual hierarchy
 *   4. Startup "loading" state written through vscreen, not printf
 *      → no stale text left on the alternate screen
 *   5. draw_header inner rows flood-filled
 *      → no ghost characters in header between renders
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdarg.h>

#include "dashboard.h"
#include "stats.h"
#include "vscreen.h"
#include "layout.h"
#include "theme.h"
#include "term.h"

/* ================================================================== */
/*  Unicode constants                                                  */
/* ================================================================== */

#define BLOCK_FULL   "\xe2\x96\x88"   /* █ */
#define BLOCK_75     "\xe2\x96\x93"   /* ▓ */
#define BLOCK_LIGHT  "\xe2\x96\x91"   /* ░ */
#define TRIANGLE     "\xe2\x96\xb6"   /* ▶ */
#define BULLET       "\xe2\x80\xa2"   /* • */
#define DIAMOND      "\xe2\x97\x86"   /* ◆ */
#define TICK         "\xe2\x9c\x94"   /* ✔ */
#define WARN         "\xe2\x9a\xa0"   /* ⚠ */
#define LIGHTNING    "\xe2\x9a\xa1"   /* ⚡ */
#define ARROW_UP     "\xe2\x86\x91"   /* ↑ */

/* Spark chars for mini bar graphs (8 levels) */
static const char *SPARK[8] = {
    " ",
    "\xe2\x96\x81",  /* ▁ */
    "\xe2\x96\x82",  /* ▂ */
    "\xe2\x96\x83",  /* ▃ */
    "\xe2\x96\x84",  /* ▄ */
    "\xe2\x96\x85",  /* ▅ */
    "\xe2\x96\x86",  /* ▆ */
    "\xe2\x96\x87",  /* ▇ */
};

/* ================================================================== */
/*  Format helpers                                                     */
/* ================================================================== */

static void fmt_rate(char *buf, int len, double r)
{
    if      (r >= 1e6) snprintf(buf, len, "%.1fM", r / 1e6);
    else if (r >= 1e3) snprintf(buf, len, "%.1fK", r / 1e3);
    else               snprintf(buf, len, "%.0f",  r);
}

static void fmt_us(char *buf, int len, long us)
{
    if      (us < 0)        snprintf(buf, len, "--");
    else if (us >= 1000000) snprintf(buf, len, "%.1fs", us / 1e6);
    else if (us >= 1000)    snprintf(buf, len, "%.1fm", us / 1e3);
    else                    snprintf(buf, len, "%ld\xc2\xb5", us); /* µ */
}

/* ================================================================== */
/*  Semantic color pickers                                             */
/* ================================================================== */

static uint32_t lat_fg(long us)
{
    if (us < 0)                  return T->fg_secondary;
    if (us < LATENCY_GREEN_US)   return T->fg_green;
    if (us < LATENCY_YELLOW_US)  return T->fg_yellow;
    if (us < 1000)               return T->fg_orange;
    return T->fg_red;
}

static uint32_t dev_fg(double d)
{
    if (d < 0.25)               return T->fg_green;
    if (d < ANOMALY_THRESHOLD)  return T->fg_yellow;
    if (d < 1.5)                return T->fg_orange;
    return T->fg_red;
}

static uint32_t event_fg(const char *ev)
{
    if (strcmp(ev, "write")     == 0) return T->fg_cyan;
    if (strcmp(ev, "read")      == 0) return T->fg_teal;
    if (strcmp(ev, "openat")    == 0) return T->fg_blue;
    if (strcmp(ev, "close")     == 0) return T->fg_purple;
    if (strcmp(ev, "mmap")      == 0) return T->fg_orange;
    if (strcmp(ev, "sched")     == 0) return T->fg_yellow;
    if (strcmp(ev, "exec")      == 0) return T->fg_green;
    if (strcmp(ev, "lifecycle") == 0) return T->fg_magenta;
    return T->fg_secondary;
}

/* ================================================================== */
/*  Aggregation                                                        */
/* ================================================================== */

typedef struct {
    char   event[32];
    long   count;
    long   total_latency;
    long   max_latency;
    long   valid_count;
    double rate;
} agg_entry_t;

static int build_event_agg(agg_entry_t *agg, int max_agg)
{
    int count = 0;
    for (int i = 0; i < stat_count; i++) {
        int found = -1;
        for (int j = 0; j < count; j++)
            if (strcmp(agg[j].event, stats[i].event) == 0) { found = j; break; }
        if (found == -1) {
            if (count >= max_agg) continue;
            strncpy(agg[count].event, stats[i].event, sizeof(agg[0].event) - 1);
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

static void sort_agg_by_rate(agg_entry_t *agg, int count)
{
    for (int i = 0; i < count - 1; i++)
        for (int j = i + 1; j < count; j++)
            if (agg[j].rate > agg[i].rate) {
                agg_entry_t t = agg[i]; agg[i] = agg[j]; agg[j] = t;
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
/*  Panel box drawing                                                  */
/*                                                                     */
/*  FIX: flood-fill the ENTIRE panel rect before drawing borders.     */
/*  This guarantees no ghost cells from previous frames survive.      */
/* ================================================================== */

static rect_t draw_panel(rect_t r, const char *title, uint32_t title_color)
{
    if (r.rows < 2 || r.cols < 2)
        return (rect_t){ r.row, r.col, 0, 0 };

    /* ── FIX #1: Flood-fill the entire panel area with blank cells ── */
    /* This is the primary fix for background ghosting / artifacts.    */
    /* Every cell in this panel's rect is explicitly set to blank,     */
    /* so the diff engine will emit a clear for any previously-drawn   */
    /* content that is no longer present this frame.                   */
    for (int rr = r.row; rr < r.row + r.rows; rr++)
        for (int cc = r.col; cc < r.col + r.cols; cc++)
            vscreen_put(rr, cc, " ", 0, 0, false, false);

    int W   = r.cols;
    int row = r.row;
    int col = r.col;

    /* Top border with title: ┌── TITLE ──────┐ */
    vscreen_put(row, col, T->btl, T->border_dim, 0, false, false);

    /* "── TITLE ──" */
    vscreen_put(row, col + 1, T->bh, T->border_dim, 0, false, false);
    vscreen_put(row, col + 2, T->bh, T->border_dim, 0, false, false);
    vscreen_put(row, col + 3, " ", T->fg_dim, 0, false, false);
    int tc = vscreen_puts(row, col + 4, title, title_color, 0, true, false);
    vscreen_put(row, tc, " ", T->fg_dim, 0, false, false);
    tc++;
    for (int c = tc; c < col + W - 1; c++)
        vscreen_put(row, c, T->bh, T->border_dim, 0, false, false);
    vscreen_put(row, col + W - 1, T->btr, T->border_dim, 0, false, false);

    /* Side borders */
    for (int rr = row + 1; rr < row + r.rows - 1; rr++) {
        vscreen_put(rr, col,         T->bv, T->border_dim, 0, false, false);
        vscreen_put(rr, col + W - 1, T->bv, T->border_dim, 0, false, false);
    }

    /* Bottom border */
    int br = row + r.rows - 1;
    vscreen_put(br, col, T->bbl, T->border_dim, 0, false, false);
    for (int c = col + 1; c < col + W - 1; c++)
        vscreen_put(br, c, T->bh, T->border_dim, 0, false, false);
    vscreen_put(br, col + W - 1, T->bbr, T->border_dim, 0, false, false);

    /* Return inner content rect */
    return (rect_t){ row + 1, col + 1, r.rows - 2, W - 2 };
}

/* Draw a horizontal divider inside a panel at `inner_row` offset from panel top */
static void draw_divider(rect_t panel, int inner_row)
{
    int r = panel.row + inner_row;
    int c = panel.col;
    int W = panel.cols;
    if (r >= panel.row + panel.rows) return;
    vscreen_put(r, c, T->bml, T->border_dim, 0, false, false);
    for (int cc = c + 1; cc < c + W - 1; cc++)
        vscreen_put(r, cc, T->bh, T->border_dim, 0, false, false);
    vscreen_put(r, c + W - 1, T->bmr, T->border_dim, 0, false, false);
}

/* Table column header row */
static void draw_table_header(rect_t content, int rel_row,
                               const char *fmt, ...)
{
    int r = content.row + rel_row;
    if (r >= content.row + content.rows) return;

    /* Fill the entire header row background */
    for (int c = content.col; c < content.col + content.cols; c++)
        vscreen_put(r, c, " ", T->fg_primary, T->bg_header_row, true, false);

    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    vscreen_puts(r, content.col + 2, buf, T->fg_primary, T->bg_header_row, true, false);
}

/* ================================================================== */
/*  Progress bar (used for activity graph)                            */
/* ================================================================== */

static void draw_bar(int row, int col, int width,
                     double fraction, uint32_t fill_fg)
{
    if (fraction < 0) fraction = 0;
    if (fraction > 1) fraction = 1;

    int filled  = (int)(fraction * width * 8);
    int full    = filled / 8;
    int partial = filled % 8;

    int c = col;
    for (int i = 0; i < full && c < col + width; i++, c++)
        vscreen_put(row, c, BLOCK_FULL, fill_fg, 0, false, false);
    if (partial > 0 && c < col + width) {
        vscreen_put(row, c, SPARK[partial], fill_fg, 0, false, false);
        c++;
    }
    for (; c < col + width; c++)
        vscreen_put(row, c, BLOCK_LIGHT, T->fg_dim, 0, false, false);
}

/* ================================================================== */
/*  SECTION 0 — HEADER BANNER                                         */
/* ================================================================== */

static void draw_header(rect_t r, double elapsed,
                         double cpu_pct,
                         int active_pid, const char *active_comm,
                         long active_min_ms)
{
    if (r.rows < 3) return;
    int W   = r.cols;
    int row = r.row;

    /* ── FIX: flood-fill every row of the header rect first ── */
    for (int rr = r.row; rr < r.row + r.rows; rr++)
        for (int cc = r.col; cc < r.col + W; cc++)
            vscreen_put(rr, cc, " ", 0, 0, false, false);

    /* ╔══ top bar ══╗ */
    vscreen_put(row, r.col, T->bdtl, T->border_dim, 0, false, false);
    for (int c = r.col + 1; c < r.col + W - 1; c++)
        vscreen_put(row, c, T->bdh, T->border_dim, 0, false, false);
    vscreen_put(row, r.col + W - 1, T->bdtr, T->border_dim, 0, false, false);

    /* ║ content row ║ */
    row++;
    vscreen_put(row, r.col,         T->bdv, T->border_dim, 0, false, false);
    vscreen_put(row, r.col + W - 1, T->bdv, T->border_dim, 0, false, false);

    int col = r.col + 2;

    /* ⚡ title */
    col = vscreen_puts(row, col, LIGHTNING " eBPF Kernel Monitor", T->fg_cyan, 0, true, false);
    col = vscreen_puts(row, col, "  v7.0", T->fg_secondary, 0, false, false);

    /* runtime */
    int h = (int)(elapsed / 3600);
    int m = ((int)elapsed % 3600) / 60;
    int s = (int)elapsed % 60;
    col = vscreen_printf(row, col, T->fg_dim, 0, false, false, "    %s  ", BULLET);
    col = vscreen_puts(row, col, "Runtime ", T->fg_secondary, 0, false, false);
    char rt[16]; snprintf(rt, sizeof(rt), "%02d:%02d:%02d", h, m, s);
    col = vscreen_puts(row, col, rt, T->fg_yellow, 0, true, false);

    /* CPU */
    uint32_t cpu_col = cpu_pct < 5.0  ? T->fg_green  :
                       cpu_pct < 30.0 ? T->fg_yellow :
                       cpu_pct < 70.0 ? T->fg_orange : T->fg_red;
    col = vscreen_printf(row, col, T->fg_dim, 0, false, false, "    %s  ", BULLET);
    col = vscreen_puts(row, col, "CPU", T->fg_secondary, 0, false, false);
    char cpu_s[16]; snprintf(cpu_s, sizeof(cpu_s), "%.1f%%", cpu_pct);
    col = vscreen_printf(row, col, cpu_col, 0, true, false, "%s", cpu_s);

    /* Slots */
    double slot_pct = (double)stat_count / MAX_STATS;
    uint32_t slot_col = slot_pct < 0.5 ? T->fg_green :
                        slot_pct < 0.8 ? T->fg_yellow : T->fg_red;
    col = vscreen_printf(row, col, T->fg_dim, 0, false, false, "    %s  ", BULLET);
    col = vscreen_puts(row, col, "Slots ", T->fg_secondary, 0, false, false);
    char slots_s[32]; snprintf(slots_s, sizeof(slots_s), "%d/%d", stat_count, MAX_STATS);
    col = vscreen_puts(row, col, slots_s, slot_col, 0, true, false);

    /* Dropped events */
    if (total_events_dropped > 0) {
        col = vscreen_printf(row, col, T->fg_dim, 0, false, false, "  ");
        char drop_s[32]; snprintf(drop_s, sizeof(drop_s), " DROPPED:%ld ", total_events_dropped);
        col = vscreen_puts(row, col, drop_s, T->fg_red, 0, true, false);
    }

    /* Clock — right-aligned */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timebuf[16];
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm_info);
    int tcol = r.col + W - 1 - (int)strlen(timebuf) - 2;
    if (tcol > col)
        vscreen_puts(row, tcol, timebuf, T->fg_secondary, 0, false, false);

    /* ║ legend / filter row ║ */
    row++;
    vscreen_put(row, r.col,         T->bdv, T->border_dim, 0, false, false);
    vscreen_put(row, r.col + W - 1, T->bdv, T->border_dim, 0, false, false);

    int any_filter = (active_pid || active_comm[0] || active_min_ms);
    col = r.col + 2;

    if (any_filter) {
        col = vscreen_puts(row, col, DIAMOND " FILTERS: ", T->fg_magenta, 0, true, false);
        if (active_pid) {
            char ps[32]; snprintf(ps, sizeof(ps), "pid=%d  ", active_pid);
            col = vscreen_puts(row, col, ps, T->fg_magenta, 0, false, false);
        }
        if (active_comm[0]) {
            char cs[32]; snprintf(cs, sizeof(cs), "comm=%s  ", active_comm);
            col = vscreen_puts(row, col, cs, T->fg_magenta, 0, false, false);
        }
        if (active_min_ms) {
            char ms[32]; snprintf(ms, sizeof(ms), "min-dur=%ldms", active_min_ms);
            col = vscreen_puts(row, col, ms, T->fg_magenta, 0, false, false);
        }
        col = vscreen_puts(row, col,
                           "  (kernel-side filter \xe2\x80\x94 zero overhead for excluded procs)",
                           T->fg_dim, 0, false, true);
    } else {
        col = vscreen_puts(row, col, "Latency: ", T->fg_secondary, 0, false, false);
        col = vscreen_puts(row, col, BLOCK_FULL " <10\xc2\xb5s  ",  T->fg_green,  0, false, false);
        col = vscreen_puts(row, col, BLOCK_FULL " <100\xc2\xb5s  ", T->fg_yellow, 0, false, false);
        col = vscreen_puts(row, col, BLOCK_FULL " <1ms  ",           T->fg_orange, 0, false, false);
        col = vscreen_puts(row, col, BLOCK_FULL " \xe2\x89\xa51ms", T->fg_red,    0, false, false);
        col = vscreen_puts(row, col,
                           "     ! = anomaly     "
                           "sched avg/p95 exclude sleep noise >200ms",
                           T->fg_dim, 0, false, false);
    }
    (void)col;

    /* ╚══ bottom bar ══╝ */
    row++;
    if (row < r.row + r.rows) {
        vscreen_put(row, r.col, T->bdbl, T->border_dim, 0, false, false);
        for (int c = r.col + 1; c < r.col + W - 1; c++)
            vscreen_put(row, c, T->bdh, T->border_dim, 0, false, false);
        vscreen_put(row, r.col + W - 1, T->bdbr, T->border_dim, 0, false, false);
    }
}

/* ================================================================== */
/*  SECTION 1 — MAIN PROCESS TABLE                                    */
/*                                                                     */
/*  FIX: adaptive column widths based on actual panel width W.        */
/*  Columns are dropped gracefully when the terminal is narrow.       */
/*  Process name is now BOLD (primary identifier).                    */
/*  Rate/latency columns are normal weight (secondary data).          */
/* ================================================================== */

#define MAX_DISPLAY 16

static void draw_processes(rect_t panel)
{
    rect_t inner = draw_panel(panel, "PROCESSES", T->fg_cyan);
    if (inner.rows < 3 || inner.cols < 40) return;

    struct syscall_stat tmp[MAX_STATS];
    memcpy(tmp, stats, (size_t)stat_count * sizeof(*tmp));
    qsort(tmp, (size_t)stat_count, sizeof(*tmp), cmp_rate);

    int W = inner.cols;

    /*
     * Adaptive column visibility based on panel width.
     *
     * Always shown  (min ~55 cols): PROCESS(14) EVENT(9) RATE(7) AVG(7) MAX(8)
     * Added at  80+: PID(7)
     * Added at  95+: P95(7)
     * Added at 115+: CTXSW(7) EXECS(6)
     */
    bool show_pid   = (W >= 80);
    bool show_p95   = (W >= 95);
    bool show_extra = (W >= 115);  /* CTXSW + EXECS */

    /* Build header string */
    char hdr[256];
    if (show_extra)
        snprintf(hdr, sizeof(hdr), "%-14s %-9s %7s %7s %7s %8s %7s %6s %6s",
                 "PROCESS", "EVENT", "RATE/s", "AVG", "P95", "MAX", "PID", "CTXSW", "EXECS");
    else if (show_p95)
        snprintf(hdr, sizeof(hdr), "%-14s %-9s %7s %7s %7s %8s %7s",
                 "PROCESS", "EVENT", "RATE/s", "AVG", "P95", "MAX", "PID");
    else if (show_pid)
        snprintf(hdr, sizeof(hdr), "%-14s %-9s %7s %7s %8s %7s",
                 "PROCESS", "EVENT", "RATE/s", "AVG", "MAX", "PID");
    else
        snprintf(hdr, sizeof(hdr), "%-14s %-9s %7s %7s %8s",
                 "PROCESS", "EVENT", "RATE/s", "AVG", "MAX");

    draw_table_header(inner, 0, "%s", hdr);
    draw_divider(panel, 2);

    /* Data rows start at inner.row+2 (inner row 0 = header, 1 = divider) */
    int data_row     = inner.row + 2;
    int max_data_rows = inner.rows - 3;  /* header + divider + footer */
    if (max_data_rows < 1) return;

    int shown = 0;
    for (int i = 0; i < stat_count && shown < MAX_DISPLAY && shown < max_data_rows; i++) {
        struct syscall_stat *s = &tmp[i];
        int r = data_row + shown;
        if (r >= inner.row + inner.rows - 1) break;

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

        /*
         * Alternating row backgrounds:
         * even rows → terminal default (0)
         * odd rows  → bg_alt_row (subtle dark tint)
         */
        uint32_t row_bg = (shown % 2 == 1) ? T->bg_alt_row : 0;

        /* Fill row background across full inner width */
        for (int c = inner.col; c < inner.col + W; c++)
            vscreen_put(r, c, " ", T->fg_primary, row_bg, false, false);

        int col = inner.col + 2;  /* 2-char left indent for breathing room */

        /* Anomaly marker — 1 char */
        if (s->is_anomaly && s->baseline_ready)
            col = vscreen_puts(r, col, "!", T->fg_red, row_bg, true, false);
        else
            col = vscreen_puts(r, col, " ", T->fg_dim, row_bg, false, false);

        /* ── FIX: Process name is BOLD — it is the primary identifier ── */
        char proc_s[16];
        snprintf(proc_s, sizeof(proc_s), "%-14s", s->process);
        col = vscreen_puts(r, col, proc_s, T->fg_primary, row_bg, true, false);
        col++;

        /* Event badge */
        char ev_s[12];
        snprintf(ev_s, sizeof(ev_s), "%-9s", s->event);
        col = vscreen_puts(r, col, ev_s, event_fg(s->event), row_bg, false, false);
        col++;

        /* Rate — right-aligned, normal weight (secondary data) */
        char rpad[12]; snprintf(rpad, sizeof(rpad), "%7s", rate_s);
        col = vscreen_puts(r, col, rpad, T->fg_primary, row_bg, false, false);
        col++;

        /* AVG */
        char apad[10]; snprintf(apad, sizeof(apad), "%7s", avg_s);
        col = vscreen_puts(r, col, apad, lat_fg(avg), row_bg, false, false);
        col++;

        /* P95 (optional) */
        if (show_p95) {
            char ppad[10]; snprintf(ppad, sizeof(ppad), "%7s", p95_s);
            col = vscreen_puts(r, col, ppad, lat_fg(p95), row_bg, false, false);
            col++;
        }

        /* MAX */
        char mpad[12]; snprintf(mpad, sizeof(mpad), "%8s", max_s);
        col = vscreen_puts(r, col, mpad, lat_fg(maxus), row_bg, false, false);
        col++;

        /* PID (optional) */
        if (show_pid) {
            char pidpad[10]; snprintf(pidpad, sizeof(pidpad), "%7d", s->pid);
            col = vscreen_puts(r, col, pidpad, T->fg_secondary, row_bg, false, false);
            col++;
        }

        /* CTXSW + EXECS (optional) */
        if (show_extra) {
            char cpad[10]; snprintf(cpad, sizeof(cpad), "%7ld", s->ctx_switches);
            col = vscreen_puts(r, col, cpad,
                               s->ctx_switches > 0 ? T->fg_yellow : T->fg_secondary,
                               row_bg, false, false);
            col++;
            char epad[10]; snprintf(epad, sizeof(epad), "%6ld", s->exec_count);
            col = vscreen_puts(r, col, epad,
                               s->exec_count > 0 ? T->fg_green : T->fg_secondary,
                               row_bg, false, false);
        }
        (void)col;

        shown++;
    }

    /* "N more entries hidden" footer */
    if (stat_count > shown) {
        int r = data_row + shown;
        if (r < inner.row + inner.rows - 1) {
            for (int c = inner.col; c < inner.col + W; c++)
                vscreen_put(r, c, " ", T->fg_dim, 0, false, false);
            char more[80];
            snprintf(more, sizeof(more),
                     "  %s %d more entries hidden  (showing top %d by rate)",
                     TRIANGLE, stat_count - shown, shown);
            vscreen_puts(r, inner.col, more, T->fg_dim, 0, false, true);
        }
    }
}

/* ================================================================== */
/*  SECTION 2 — EVENT SUMMARY                                         */
/* ================================================================== */

static void draw_event_summary(rect_t panel)
{
    rect_t inner = draw_panel(panel, "EVENT SUMMARY", T->fg_teal);
    if (inner.rows < 3 || inner.cols < 20) return;

    agg_entry_t agg[32];
    int count = build_event_agg(agg, 32);

    draw_table_header(inner, 0,
        "%-11s %8s %8s %10s %8s",
        "EVENT", "AVG", "MAX", "TOTAL", "RATE/s");
    draw_divider(panel, 2);

    int r = inner.row + 2;
    for (int i = 0; i < count && r < inner.row + inner.rows - 1; i++, r++) {
        long avg   = (agg[i].valid_count > 0 && agg[i].total_latency > 0)
                     ? (agg[i].total_latency / agg[i].valid_count) / 1000 : 0;
        long maxus = agg[i].max_latency / 1000;
        int is_outlier = (avg > 0 && maxus > avg * 10);

        char avg_s[12], max_s[12], rate_s[12], tot_s[16];
        fmt_us(avg_s,   sizeof(avg_s),  avg);
        fmt_us(max_s,   sizeof(max_s),  maxus);
        fmt_rate(rate_s, sizeof(rate_s), agg[i].rate);
        snprintf(tot_s,  sizeof(tot_s),  "%ld", agg[i].count);

        for (int c = inner.col; c < inner.col + inner.cols; c++)
            vscreen_put(r, c, " ", T->fg_primary, 0, false, false);

        int col = inner.col + 2;
        char evpad[14]; snprintf(evpad, sizeof(evpad), "%-11s", agg[i].event);
        col = vscreen_puts(r, col, evpad, event_fg(agg[i].event), 0, false, false);
        col++;
        char apad[12]; snprintf(apad, sizeof(apad), "%8s", avg_s);
        col = vscreen_puts(r, col, apad, lat_fg(avg), 0, false, false);
        col++;
        char mpad[12]; snprintf(mpad, sizeof(mpad), "%8s", max_s);
        col = vscreen_puts(r, col, mpad, lat_fg(maxus), 0, false, false);
        col++;
        char tpad[14]; snprintf(tpad, sizeof(tpad), "%10s", tot_s);
        col = vscreen_puts(r, col, tpad, T->fg_primary, 0, true, false);
        col++;
        char rpad[12]; snprintf(rpad, sizeof(rpad), "%8s", rate_s);
        col = vscreen_puts(r, col, rpad, T->fg_primary, 0, true, false);
        if (is_outlier) {
            col++;
            col = vscreen_puts(r, col, WARN " outlier", T->fg_yellow, 0, false, true);
        }
        (void)col;
    }
}

/* ================================================================== */
/*  SECTION 3 — ACTIVITY GRAPH                                        */
/* ================================================================== */

static void draw_activity(rect_t panel)
{
    agg_entry_t agg[32];
    int count = build_event_agg(agg, 32);
    sort_agg_by_rate(agg, count);

    rect_t inner = draw_panel(panel, "ACTIVITY  " ARROW_UP " rate/s", T->fg_orange);
    if (inner.rows < 2 || inner.cols < 20) return;

    double max_rate = (count > 0 && agg[0].rate > 0) ? agg[0].rate : 1.0;

    /*
     * Bar width calculation:
     * label(11) + space(1) + [bar fills remainder] + space(1) + rate(9)
     * Bar gets at least 60% of the inner width for visual impact.
     */
    int label_w = 11;
    int rate_w  = 9;
    int bar_w   = inner.cols - label_w - rate_w - 4;
    if (bar_w < 8) bar_w = 8;

    int r = inner.row;
    for (int i = 0; i < count && r < inner.row + inner.rows; i++, r++) {
        for (int c = inner.col; c < inner.col + inner.cols; c++)
            vscreen_put(r, c, " ", T->fg_primary, 0, false, false);

        double frac = agg[i].rate / max_rate;
        uint32_t bar_col = frac < 0.25 ? T->fg_green  :
                           frac < 0.50 ? T->fg_teal   :
                           frac < 0.75 ? T->fg_yellow  : T->fg_orange;

        int col = inner.col + 2;
        char evpad[14]; snprintf(evpad, sizeof(evpad), "%-11s", agg[i].event);
        col = vscreen_puts(r, col, evpad, event_fg(agg[i].event), 0, false, false);
        col++;

        draw_bar(r, col, bar_w, frac, bar_col);
        col += bar_w + 1;

        char rate_s[16]; fmt_rate(rate_s, sizeof(rate_s), agg[i].rate);
        char rpad[14]; snprintf(rpad, sizeof(rpad), "%8s/s", rate_s);
        vscreen_puts(r, col, rpad, T->fg_primary, 0, true, false);
    }
}

/* ================================================================== */
/*  SECTION 4 — SLOWEST SYSCALLS                                      */
/* ================================================================== */

#define TOP_N 6

static void draw_slowest(rect_t panel)
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

    rect_t inner = draw_panel(panel, "SLOWEST SYSCALLS", T->fg_red);
    if (inner.rows < 3 || inner.cols < 20 || n == 0) return;

    qsort(tmp, (size_t)n, sizeof(*tmp), cmp_latency);

    draw_table_header(inner, 0,
        "%-15s %-10s %10s %10s %10s",
        "PROCESS", "SYSCALL", "MAX", "P95", "AVG");
    draw_divider(panel, 2);

    int r = inner.row + 2;
    for (int i = 0; i < n && i < TOP_N && r < inner.row + inner.rows; i++, r++) {
        long maxus = tmp[i].max_latency / 1000;
        long p95   = stats_p95_us(&tmp[i]);
        long valid = tmp[i].count - tmp[i].drop_count;
        long avg   = (valid > 0 && tmp[i].total_latency > 0)
                     ? (tmp[i].total_latency / valid) / 1000 : 0;

        char max_s[12], p95_s[12], avg_s[12];
        fmt_us(max_s, sizeof(max_s), maxus);
        fmt_us(p95_s, sizeof(p95_s), p95);
        fmt_us(avg_s, sizeof(avg_s), avg);

        for (int c = inner.col; c < inner.col + inner.cols; c++)
            vscreen_put(r, c, " ", T->fg_primary, 0, false, false);

        int col = inner.col + 2;
        char proc_s[18]; snprintf(proc_s, sizeof(proc_s), "%-15s", tmp[i].process);
        col = vscreen_puts(r, col, proc_s, T->fg_primary, 0, true, false);
        col++;
        char ev_s[14]; snprintf(ev_s, sizeof(ev_s), "%-10s", tmp[i].event);
        col = vscreen_puts(r, col, ev_s, event_fg(tmp[i].event), 0, false, false);
        col++;
        char mpad[14]; snprintf(mpad, sizeof(mpad), "%10s", max_s);
        col = vscreen_puts(r, col, mpad, lat_fg(maxus), 0, false, false);
        col++;
        char ppad[14]; snprintf(ppad, sizeof(ppad), "%10s", p95_s);
        col = vscreen_puts(r, col, ppad, lat_fg(p95), 0, false, false);
        col++;
        char apad[14]; snprintf(apad, sizeof(apad), "%10s", avg_s);
        vscreen_puts(r, col, apad, lat_fg(avg), 0, false, false);
    }
}

/* ================================================================== */
/*  SECTION 5 — ANOMALY DETECTION                                     */
/* ================================================================== */

static void draw_anomalies(rect_t panel)
{
    struct syscall_stat tmp[MAX_STATS];
    memcpy(tmp, stats, (size_t)stat_count * sizeof(*tmp));
    qsort(tmp, (size_t)stat_count, sizeof(*tmp), cmp_deviation);

    int n_anom = 0;
    for (int i = 0; i < stat_count; i++)
        if (tmp[i].is_anomaly && tmp[i].baseline_ready) n_anom++;

    rect_t inner = draw_panel(panel, "ANOMALY DETECTION \xe2\x80\x94 EMA BASELINE", T->fg_red);
    if (inner.rows < 3 || inner.cols < 20) return;

    if (n_anom == 0) {
        int r = inner.row + 1;
        for (int c = inner.col; c < inner.col + inner.cols; c++)
            vscreen_put(r, c, " ", T->fg_primary, 0, false, false);
        vscreen_puts(r, inner.col + 2,
                     TICK "  All events within normal baseline.",
                     T->fg_green, 0, false, false);
        return;
    }

    draw_table_header(inner, 0,
        "%-15s %-11s %10s %10s %10s",
        "PROCESS", "EVENT", "DEVIATION", "BASELINE", "CURRENT");
    draw_divider(panel, 2);

    int r = inner.row + 2;
    int shown = 0;
    for (int i = 0; i < stat_count && shown < TOP_N && r < inner.row + inner.rows; i++) {
        if (!tmp[i].is_anomaly || !tmp[i].baseline_ready) continue;

        long valid      = tmp[i].count - tmp[i].drop_count;
        long current_us = (valid > 0 && tmp[i].total_latency > 0)
                          ? (tmp[i].total_latency / valid) / 1000 : 0;
        long baseline_us = (long)(tmp[i].baseline_latency / 1000.0);

        char cur_s[12], bas_s[12];
        fmt_us(cur_s, sizeof(cur_s), current_us);
        fmt_us(bas_s, sizeof(bas_s), baseline_us);

        uint32_t dc = dev_fg(tmp[i].deviation);

        for (int c = inner.col; c < inner.col + inner.cols; c++)
            vscreen_put(r, c, " ", T->fg_primary, 0, false, false);

        int col = inner.col + 2;
        char proc_s[18]; snprintf(proc_s, sizeof(proc_s), "%-15s", tmp[i].process);
        col = vscreen_puts(r, col, proc_s, T->fg_primary, 0, false, false);
        col++;
        char ev_s[14]; snprintf(ev_s, sizeof(ev_s), "%-11s", tmp[i].event);
        col = vscreen_puts(r, col, ev_s, event_fg(tmp[i].event), 0, false, false);
        col++;
        char dev_s[16]; snprintf(dev_s, sizeof(dev_s), "%8.1f%%", tmp[i].deviation * 100.0);
        char dpad[14]; snprintf(dpad, sizeof(dpad), "%10s", dev_s);
        col = vscreen_puts(r, col, dpad, dc, 0, true, false);
        col++;
        char bpad[14]; snprintf(bpad, sizeof(bpad), "%10s", bas_s);
        col = vscreen_puts(r, col, bpad, T->fg_secondary, 0, false, false);
        col++;
        char cpad[14]; snprintf(cpad, sizeof(cpad), "%10s", cur_s);
        vscreen_puts(r, col, cpad, dc, 0, false, false);

        shown++;
        r++;
    }

    if (n_anom > shown && r < inner.row + inner.rows) {
        for (int c = inner.col; c < inner.col + inner.cols; c++)
            vscreen_put(r, c, " ", T->fg_dim, 0, false, false);
        char more[64];
        snprintf(more, sizeof(more),
                 "  %s %d more anomalies not shown", TRIANGLE, n_anom - shown);
        vscreen_puts(r, inner.col, more, T->fg_dim, 0, false, true);
    }
}

/* ================================================================== */
/*  SECTION 6 — PROCESS LIFECYCLE                                     */
/* ================================================================== */

static void draw_lifecycle(rect_t panel)
{
    if (panel.rows < 4) return;

    struct {
        char process[16];
        int  pid;
        long execs;
        long exits;
        long avg_life_us;
    } procs[64];
    int lc_count = 0;

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
            strncpy(procs[idx].process, s->process, sizeof(procs[0].process) - 1);
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

    rect_t inner = draw_panel(panel, "PROCESS LIFECYCLE", T->fg_magenta);
    if (inner.rows < 3 || inner.cols < 20) return;

    draw_table_header(inner, 0,
        "%-15s %-7s %10s %10s %12s",
        "PROCESS", "PID", "EXECS", "EXITS", "AVG LIFETIME");
    draw_divider(panel, 2);

    int r = inner.row + 2;
    for (int i = 0; i < lc_count && r < inner.row + inner.rows; i++, r++) {
        char life_s[16]; fmt_us(life_s, sizeof(life_s), procs[i].avg_life_us);

        for (int c = inner.col; c < inner.col + inner.cols; c++)
            vscreen_put(r, c, " ", T->fg_primary, 0, false, false);

        int col = inner.col + 2;
        char proc_s[18]; snprintf(proc_s, sizeof(proc_s), "%-15s", procs[i].process);
        col = vscreen_puts(r, col, proc_s, T->fg_primary, 0, true, false);
        col++;
        char pid_s[10]; snprintf(pid_s, sizeof(pid_s), "%-7d", procs[i].pid);
        col = vscreen_puts(r, col, pid_s, T->fg_secondary, 0, false, false);
        col++;
        char ex_s[14]; snprintf(ex_s, sizeof(ex_s), "%10ld", procs[i].execs);
        col = vscreen_puts(r, col, ex_s, T->fg_green, 0, false, false);
        col++;
        char ei_s[14]; snprintf(ei_s, sizeof(ei_s), "%10ld", procs[i].exits);
        col = vscreen_puts(r, col, ei_s, T->fg_yellow, 0, false, false);
        col++;
        char av_s[16]; snprintf(av_s, sizeof(av_s), "%12s", life_s);
        vscreen_puts(r, col, av_s, T->fg_cyan, 0, false, false);
    }
}

/* ================================================================== */
/*  CONTROLS BAR                                                       */
/* ================================================================== */

static void draw_controls(rect_t r)
{
    if (r.rows < 1) return;
    int row = r.row;

    /* Fill entire row with header-row background */
    for (int c = r.col; c < r.col + r.cols; c++)
        vscreen_put(row, c, " ", T->fg_secondary, T->bg_header_row, false, false);

    int col = r.col + 2;
    col = vscreen_puts(row, col, "[q]", T->fg_primary, T->bg_header_row, true, false);
    col = vscreen_puts(row, col, " quit   ", T->fg_secondary, T->bg_header_row, false, false);
    col = vscreen_puts(row, col, "[r]", T->fg_primary, T->bg_header_row, true, false);
    col = vscreen_puts(row, col, " reset   ", T->fg_secondary, T->bg_header_row, false, false);
    col = vscreen_puts(row, col, "[e]", T->fg_primary, T->bg_header_row, true, false);
    col = vscreen_puts(row, col, " export snapshot   ", T->fg_secondary, T->bg_header_row, false, false);
    col = vscreen_puts(row, col, "[1-6]", T->fg_primary, T->bg_header_row, true, false);
    col = vscreen_puts(row, col, " sort column", T->fg_secondary, T->bg_header_row, false, false);
    (void)col;
}

/* ================================================================== */
/*  MAIN ENTRY POINT                                                   */
/* ================================================================== */

void dashboard_render(double elapsed, double cpu_pct,
                      int active_pid, const char *active_comm,
                      long active_min_ms)
{
    /* Get current terminal size every frame */
    term_size_t sz = term_get_size();

    /*
     * FIX: Only call vscreen_resize when the terminal size actually changes.
     * vscreen_resize() does a full memset of vs_prev (forces complete redraw)
     * so calling it every frame causes unnecessary flicker.
     * Resize is also handled by SIGWINCH in bootstrap.c — that path calls
     * vscreen_resize + vscreen_invalidate. Here we only need to catch the
     * case where resize happened between signal delivery and this render.
     */
    static int last_rows = 0, last_cols = 0;
    if (sz.rows != last_rows || sz.cols != last_cols) {
        vscreen_resize(sz.rows, sz.cols);
        last_rows = sz.rows;
        last_cols = sz.cols;
    }

    /* Clear the next frame — every cell set to blank */
    vscreen_clear();

    /* Compute panel layout for current terminal size */
    layout_t L;
    layout_compute(sz.rows, sz.cols, &L);

    /* Paint every panel */
    draw_header(L.header, elapsed, cpu_pct,
                active_pid, active_comm, active_min_ms);

    draw_processes(L.processes);
    draw_event_summary(L.summary);
    draw_activity(L.activity);
    draw_slowest(L.slowest);
    draw_anomalies(L.anomaly);

    if (L.lifecycle.rows >= 4)
        draw_lifecycle(L.lifecycle);

    draw_controls(L.controls);

    /* Diff vs_next against vs_prev — emit only changed cells to stdout */
    vscreen_flush();
}
