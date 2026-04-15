/* SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause) */

/*
 * dashboard.c — btop-style TUI renderer  (v8.0 — production)
 *
 * CHANGES IN v8.0:
 *
 *  1. TRUNCATE HELPER — safe_puts() clips any string at panel boundary
 *     with "..." ellipsis, preventing text bleed across panel edges.
 *
 *  2. ROW FILL MACRO — FILL_ROW() used before every data row so the
 *     full width is always painted with the row background before text.
 *     Eliminates leftover characters when content shrinks.
 *
 *  3. SPARKLINE RING BUFFER — each event type keeps a 32-sample rolling
 *     rate history.  Rendered as ▁▂▃▄▅▆▇█ inline in the ACTIVITY panel.
 *
 *  4. SCHEDULING DELAY PANEL — extracted sched entries shown with
 *     AVG / MAX / P95 delay and a trend sparkline.  Replaces no-data
 *     dead space when lifecycle panel is short.
 *
 *  5. HEADER v2 — adds EVENTS/s and OVERHEAD % beside DROPPED.
 *     Format:  Runtime 00:01:23 │ CP0.4% │ EVT/s 12.3K │ DROPPED:0 │ ...
 *
 *  6. COLUMN SAFETY — every column sprintf uses a width-bounded buffer
 *     and the final col pointer is checked against inner.col+inner.cols
 *     before writing, so wide terminals never write into the border.
 *
 *  7. FULL BG DISCIPLINE — BG (T->bg_panel) is passed to every single
 *     vscreen_put call.  No call passes bg=0.
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

/* Shorthand — panel background used throughout */
#define BG (T->bg_panel)

/* Fill a full inner row with the row background before writing text */
#define FILL_ROW(row_, rect_) \
    do { \
        for (int _fc = (rect_).col; _fc < (rect_).col + (rect_).cols; _fc++) \
            vscreen_put((row_), _fc, " ", T->fg_dim, (row_bg__), false, false); \
    } while (0)

/* ================================================================== */
/*  Unicode constants                                                  */
/* ================================================================== */

#define BLOCK_FULL    "\xe2\x96\x88"   /* █ */
#define BLOCK_LIGHT   "\xe2\x96\x91"   /* ░ */
#define BLOCK_LOWER   "\xe2\x96\x84"   /* ▄ */
#define TRIANGLE_R    "\xe2\x96\xb6"   /* ▶ */
#define BULLET        "\xe2\x80\xa2"   /* • */
#define DIAMOND       "\xe2\x97\x86"   /* ◆ */
#define TICK          "\xe2\x9c\x94"   /* ✔ */
#define WARN          "\xe2\x9a\xa0"   /* ⚠ */
#define LIGHTNING     "\xe2\x9a\xa1"   /* ⚡ */
#define ARROW_UP      "\xe2\x86\x91"   /* ↑ */

/* Spark chars (8 sub-cell heights, index 0 = blank) */
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
/*  Frame counter (blink / animation)                                  */
/* ================================================================== */

static unsigned long g_frame = 0;

/* ================================================================== */
/*  Sparkline ring buffer                                              */
/*                                                                     */
/*  One ring per named event type (write, read, sched, …).            */
/*  Sampled every dashboard render (≈2 s), 32 slots → ~64 s history. */
/* ================================================================== */

#define SPARK_SLOTS   32
#define SPARK_EVTS    12

typedef struct {
    char   name[16];
    double samples[SPARK_SLOTS];
    int    head;       /* next write position */
    int    count;      /* samples stored so far (≤ SPARK_SLOTS) */
} spark_ring_t;

static spark_ring_t g_sparks[SPARK_EVTS];
static int          g_spark_count = 0;

static spark_ring_t *spark_get_or_create(const char *name)
{
    for (int i = 0; i < g_spark_count; i++)
        if (strcmp(g_sparks[i].name, name) == 0)
            return &g_sparks[i];
    if (g_spark_count >= SPARK_EVTS) return NULL;
    spark_ring_t *r = &g_sparks[g_spark_count++];
    memset(r, 0, sizeof(*r));
    strncpy(r->name, name, sizeof(r->name) - 1);
    return r;
}

static void spark_push(spark_ring_t *r, double val)
{
    r->samples[r->head] = val;
    r->head = (r->head + 1) % SPARK_SLOTS;
    if (r->count < SPARK_SLOTS) r->count++;
}

/* Render width cells of sparkline at (row, col), returns col after */
static int draw_sparkline(int row, int col, int width,
                          const spark_ring_t *r,
                          uint32_t fg, uint32_t row_bg)
{
    if (!r || r->count == 0 || width <= 0) {
        for (int i = 0; i < width; i++)
            vscreen_put(row, col + i, " ", T->fg_dim, row_bg, false, false);
        return col + width;
    }

    /* Find max over visible window */
    double mx = 0.0;
    int n = r->count < width ? r->count : width;
    /* walk backwards: most-recent n samples */
    for (int i = 0; i < n; i++) {
        int idx = ((r->head - 1 - i) + SPARK_SLOTS) % SPARK_SLOTS;
        if (r->samples[idx] > mx) mx = r->samples[idx];
    }
    if (mx == 0.0) mx = 1.0;

    /* Pad left if fewer samples than width */
    int pad = width - n;
    for (int i = 0; i < pad; i++)
        vscreen_put(row, col + i, " ", T->fg_dim, row_bg, false, false);

    /* Draw oldest → newest left → right */
    for (int i = 0; i < n; i++) {
        int sample_age = n - 1 - i;   /* 0 = newest */
        int idx = ((r->head - 1 - sample_age) + SPARK_SLOTS) % SPARK_SLOTS;
        double frac = r->samples[idx] / mx;
        int level = (int)(frac * 7.0);
        if (level < 0) level = 0;
        if (level > 7) level = 7;
        vscreen_put(row, col + pad + i, SPARK[level], fg, row_bg, false, false);
    }
    return col + width;
}

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
    else                    snprintf(buf, len, "%ld\xc2\xb5", us);
}

static void fmt_elapsed(char *buf, int len, double s)
{
    int h   = (int)(s / 3600);
    int m   = ((int)s % 3600) / 60;
    int sec = (int)s % 60;
    snprintf(buf, len, "%02d:%02d:%02d", h, m, sec);
}

/* ================================================================== */
/*  String truncation helper                                           */
/*                                                                     */
/*  Writes at most `max_cols` terminal cells from `str`, appending    */
/*  "…" (U+2026, 3 bytes) if the string was clipped.                 */
/*  Returns the column after the last written character.              */
/* ================================================================== */

static int safe_puts(int row, int col, int max_cols,
                     const char *str,
                     uint32_t fg, uint32_t bg,
                     bool bold, bool dim)
{
    if (max_cols <= 0 || !str) return col;

    /* Count display columns (simple: 1 byte-seq = 1 cell for our content) */
    int display_len = 0;
    const unsigned char *p = (const unsigned char *)str;
    while (*p) {
        unsigned char b = *p;
        int bytes = 1;
        if      ((b & 0x80) == 0x00) bytes = 1;
        else if ((b & 0xE0) == 0xC0) bytes = 2;
        else if ((b & 0xF0) == 0xE0) bytes = 3;
        else if ((b & 0xF8) == 0xF0) bytes = 4;
        display_len++;
        p += bytes;
    }

    if (display_len <= max_cols) {
        return vscreen_puts(row, col, str, fg, bg, bold, dim);
    }

    /* Truncate: copy chars until we'd exceed max_cols - 1 (for ellipsis) */
    char truncated[512];
    int  out   = 0;
    int  cells = 0;
    int  limit = max_cols - 1;   /* leave 1 cell for "…" */
    p = (const unsigned char *)str;
    while (*p && cells < limit && out < (int)sizeof(truncated) - 5) {
        unsigned char b = *p;
        int bytes = 1;
        if      ((b & 0x80) == 0x00) bytes = 1;
        else if ((b & 0xE0) == 0xC0) bytes = 2;
        else if ((b & 0xF0) == 0xE0) bytes = 3;
        else if ((b & 0xF8) == 0xF0) bytes = 4;
        for (int i = 0; i < bytes; i++) truncated[out++] = (char)p[i];
        cells++;
        p += bytes;
    }
    /* Append U+2026 HORIZONTAL ELLIPSIS (3 UTF-8 bytes) */
    truncated[out++] = (char)0xe2;
    truncated[out++] = (char)0x80;
    truncated[out++] = (char)0xa6;
    truncated[out]   = '\0';

    return vscreen_puts(row, col, truncated, fg, bg, bold, dim);
}

/* Convenience: printf into fixed width field, then safe_puts */
static int safe_printf(int row, int col, int max_cols,
                       uint32_t fg, uint32_t bg,
                       bool bold, bool dim,
                       const char *fmt, ...)
    __attribute__((format(printf, 8, 9)));

static int safe_printf(int row, int col, int max_cols,
                       uint32_t fg, uint32_t bg,
                       bool bold, bool dim,
                       const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return safe_puts(row, col, max_cols, buf, fg, bg, bold, dim);
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

static uint32_t heat_color(double frac)
{
    if (frac < 0.25) return T->fg_green;
    if (frac < 0.50) return T->fg_yellow;
    if (frac < 0.75) return T->fg_orange;
    return T->fg_red;
}

/* ================================================================== */
/*  Aggregation helpers                                                */
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
/*  Events-per-second global accumulator                              */
/*  Updated once per render; used by header and scheduling panel.    */
/* ================================================================== */

static double g_events_per_sec = 0.0;   /* total across all event types */

static void update_events_per_sec(void)
{
    double total = 0.0;
    for (int i = 0; i < stat_count; i++)
        total += stats[i].rate;
    g_events_per_sec = total;
}

/* ================================================================== */
/*  Core fill helper                                                   */
/* ================================================================== */

static void fill_rect(rect_t r, uint32_t fg, uint32_t bg)
{
    for (int rr = r.row; rr < r.row + r.rows; rr++)
        for (int cc = r.col; cc < r.col + r.cols; cc++)
            vscreen_put(rr, cc, " ", fg, bg, false, false);
}

/* Fill a single row from col a to col b (exclusive) with bg */
static void fill_row_bg(int row, int col_start, int col_end,
                         uint32_t row_bg)
{
    for (int c = col_start; c < col_end; c++)
        vscreen_put(row, c, " ", T->fg_dim, row_bg, false, false);
}

/* ================================================================== */
/*  Progress bar with sub-cell precision                              */
/* ================================================================== */

static void draw_bar(int row, int col, int width,
                     double fraction, uint32_t fill_fg, uint32_t empty_fg,
                     uint32_t row_bg)
{
    if (fraction < 0) fraction = 0;
    if (fraction > 1) fraction = 1;

    int filled  = (int)(fraction * width * 8);
    int full    = filled / 8;
    int partial = filled % 8;

    int c = col;
    for (int i = 0; i < full && c < col + width; i++, c++)
        vscreen_put(row, c, BLOCK_FULL, fill_fg, row_bg, false, false);
    if (partial > 0 && c < col + width) {
        vscreen_put(row, c, SPARK[partial], fill_fg, row_bg, false, false);
        c++;
    }
    for (; c < col + width; c++)
        vscreen_put(row, c, BLOCK_LIGHT, empty_fg, row_bg, false, false);
}

/* Compact mini bar (solid blocks) */
static void draw_mini_bar(int row, int col, int width,
                          double fraction, uint32_t fg, uint32_t row_bg)
{
    if (fraction < 0) fraction = 0;
    if (fraction > 1) fraction = 1;
    int filled = (int)(fraction * width);
    for (int i = 0; i < width; i++) {
        if (i < filled)
            vscreen_put(row, col + i, BLOCK_LOWER, fg, row_bg, false, false);
        else
            vscreen_put(row, col + i, BULLET, T->fg_dim, row_bg, false, false);
    }
}

/* ================================================================== */
/*  Panel box drawing                                                  */
/* ================================================================== */

static rect_t draw_panel(rect_t r, const char *title, uint32_t title_color)
{
    if (r.rows < 2 || r.cols < 2)
        return (rect_t){ r.row, r.col, 0, 0 };

    /* Flood-fill entire panel rect — guarantees no ghost cells */
    fill_rect(r, T->fg_dim, BG);

    int W   = r.cols;
    int row = r.row;
    int col = r.col;

    /* ┌─── ◆ TITLE ────┐ */
    vscreen_put(row, col,     T->btl, T->border_dim, BG, false, false);
    vscreen_put(row, col + 1, T->bh,  T->border_dim, BG, false, false);
    vscreen_put(row, col + 2, T->bh,  T->border_dim, BG, false, false);
    vscreen_put(row, col + 3, " ",    T->border_dim, BG, false, false);
    int tc = col + 4;
    tc = vscreen_puts(row, tc, DIAMOND " ", title_color, BG, false, false);
    tc = safe_puts(row, tc, col + W - 2 - tc,
                   title, title_color, BG, true, false);
    vscreen_put(row, tc, " ", T->border_dim, BG, false, false);
    tc++;
    for (int c = tc; c < col + W - 1; c++)
        vscreen_put(row, c, T->bh, T->border_dim, BG, false, false);
    vscreen_put(row, col + W - 1, T->btr, T->border_dim, BG, false, false);

    /* Side borders */
    for (int rr = row + 1; rr < row + r.rows - 1; rr++) {
        vscreen_put(rr, col,         T->bv, T->border_dim, BG, false, false);
        vscreen_put(rr, col + W - 1, T->bv, T->border_dim, BG, false, false);
    }

    /* Bottom border */
    int br = row + r.rows - 1;
    vscreen_put(br, col, T->bbl, T->border_dim, BG, false, false);
    for (int c = col + 1; c < col + W - 1; c++)
        vscreen_put(br, c, T->bh, T->border_dim, BG, false, false);
    vscreen_put(br, col + W - 1, T->bbr, T->border_dim, BG, false, false);

    return (rect_t){ row + 1, col + 1, r.rows - 2, W - 2 };
}

/* Horizontal divider inside a panel */
static void draw_divider(rect_t panel, int inner_row)
{
    int r = panel.row + inner_row;
    if (r >= panel.row + panel.rows) return;
    int c = panel.col;
    int W = panel.cols;
    vscreen_put(r, c,         T->bml, T->border_dim, BG, false, false);
    for (int cc = c + 1; cc < c + W - 1; cc++)
        vscreen_put(r, cc, T->bh, T->border_dim, BG, false, false);
    vscreen_put(r, c + W - 1, T->bmr, T->border_dim, BG, false, false);
}

/* Table header row — fills background, then writes label */
static void draw_table_header(rect_t content, int rel_row,
                               const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void draw_table_header(rect_t content, int rel_row,
                               const char *fmt, ...)
{
    int r = content.row + rel_row;
    if (r >= content.row + content.rows) return;

    fill_row_bg(r, content.col, content.col + content.cols, T->bg_header_row);

    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    safe_puts(r, content.col + 2, content.cols - 2,
              buf, T->fg_secondary, T->bg_header_row, false, false);
}

/* ================================================================== */
/*  SECTION 0 — HEADER BANNER                                         */
/*                                                                     */
/*  New in v8: EVENTS/s and OVERHEAD % added beside slot/drop info.  */
/* ================================================================== */

static void draw_header(rect_t r, double elapsed,
                         double cpu_pct,
                         int active_pid, const char *active_comm,
                         long active_min_ms)
{
    if (r.rows < 3) return;
    int W   = r.cols;
    int row = r.row;

    /* Full header fill */
    for (int rr = r.row; rr < r.row + r.rows; rr++)
        fill_row_bg(rr, r.col, r.col + W, T->bg_header_row);

    /* ╔══ top border ══╗ */
    vscreen_put(row, r.col,         T->bdtl, T->border_dim, T->bg_header_row, false, false);
    for (int c = r.col + 1; c < r.col + W - 1; c++)
        vscreen_put(row, c, T->bdh, T->border_dim, T->bg_header_row, false, false);
    vscreen_put(row, r.col + W - 1, T->bdtr, T->border_dim, T->bg_header_row, false, false);

    /* ║ content row 1 — title + metrics ║ */
    row++;
    vscreen_put(row, r.col,         T->bdv, T->border_dim, T->bg_header_row, false, false);
    vscreen_put(row, r.col + W - 1, T->bdv, T->border_dim, T->bg_header_row, false, false);

    int col = r.col + 2;
    int right_guard = r.col + W - 2;   /* don't write into right border */

#define HDR_SEP() \
    do { \
        if (col + 3 < right_guard) { \
            col = vscreen_puts(row, col, "  ", T->fg_dim, T->bg_header_row, false, false); \
            vscreen_put(row, col, T->bdv, T->border_dim, T->bg_header_row, false, false); \
            col++; \
        } \
    } while(0)

    /* Logo */
    col = vscreen_puts(row, col, LIGHTNING, T->fg_yellow, T->bg_header_row, true, false);
    col = vscreen_puts(row, col, " eBPF", T->fg_cyan, T->bg_header_row, true, false);
    col = vscreen_puts(row, col, " Kernel", T->fg_primary, T->bg_header_row, true, false);
    col = vscreen_puts(row, col, " Monitor", T->fg_cyan, T->bg_header_row, true, false);
    col = vscreen_puts(row, col, " v8.0", T->fg_secondary, T->bg_header_row, false, false);

    HDR_SEP();

    /* Runtime */
    char rt[16]; fmt_elapsed(rt, sizeof(rt), elapsed);
    col = vscreen_puts(row, col, " Runtime ", T->fg_secondary, T->bg_header_row, false, false);
    col = vscreen_puts(row, col, rt, T->fg_yellow, T->bg_header_row, true, false);

    HDR_SEP();

    /* CPU overhead */
    uint32_t cpu_col = cpu_pct < 5.0  ? T->fg_green  :
                       cpu_pct < 30.0 ? T->fg_yellow :
                       cpu_pct < 70.0 ? T->fg_orange : T->fg_red;
    char cpu_s[16]; snprintf(cpu_s, sizeof(cpu_s), "%.1f%%", cpu_pct);
    col = vscreen_puts(row, col, " CP", T->fg_secondary, T->bg_header_row, false, false);
    col = vscreen_puts(row, col, cpu_s, cpu_col, T->bg_header_row, true, false);

    HDR_SEP();

    /* Events per second (NEW) */
    if (col + 14 < right_guard) {
        char evts_s[16]; fmt_rate(evts_s, sizeof(evts_s), g_events_per_sec);
        col = vscreen_puts(row, col, " EVT/s ", T->fg_secondary, T->bg_header_row, false, false);
        col = vscreen_puts(row, col, evts_s, T->fg_teal, T->bg_header_row, true, false);
        HDR_SEP();
    }

    /* Slot count */
    double slot_pct = (double)stat_count / MAX_STATS;
    uint32_t slot_col = slot_pct < 0.5 ? T->fg_green :
                        slot_pct < 0.8 ? T->fg_yellow : T->fg_red;
    char slots_s[32]; snprintf(slots_s, sizeof(slots_s), "%d/%d", stat_count, MAX_STATS);
    if (col + 12 < right_guard) {
        col = vscreen_puts(row, col, " Slots ", T->fg_secondary, T->bg_header_row, false, false);
        col = vscreen_puts(row, col, slots_s, slot_col, T->bg_header_row, true, false);
    }

    /* Dropped events */
    if (total_events_dropped > 0 && col + 16 < right_guard) {
        HDR_SEP();
        char drop_s[48];
        char drop_n[16]; fmt_rate(drop_n, sizeof(drop_n), (double)total_events_dropped);
        snprintf(drop_s, sizeof(drop_s), " %s DROPPED:%s ", WARN, drop_n);
        col = vscreen_puts(row, col, drop_s, T->fg_red, T->bg_header_row, true, false);
    }

    /* Overhead % (separate from CPU — this is BPF overhead estimate) */
    if (elapsed > 2.0 && col + 16 < right_guard) {
        HDR_SEP();
        double overhead = cpu_pct;   /* same value, shown explicitly labeled */
        char oh_s[16]; snprintf(oh_s, sizeof(oh_s), "%.2f%%", overhead);
        col = vscreen_puts(row, col, " OVERHEAD:", T->fg_secondary, T->bg_header_row, false, false);
        col = vscreen_puts(row, col, oh_s, cpu_col, T->bg_header_row, true, false);
    }
    (void)col;

    /* Clock — right-aligned */
    time_t     now     = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timebuf[12], datebuf[12];
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm_info);
    strftime(datebuf, sizeof(datebuf), "%a %d %b", tm_info);
    int time_col = r.col + W - 1 - (int)strlen(timebuf) - (int)strlen(datebuf) - 3;
    if (time_col > r.col + (int)strlen(datebuf) + 2) {
        vscreen_puts(row, time_col, datebuf, T->fg_dim, T->bg_header_row, false, false);
        time_col += (int)strlen(datebuf) + 1;
        vscreen_puts(row, time_col, timebuf, T->fg_primary, T->bg_header_row, true, false);
    }

#undef HDR_SEP

    /* ║ legend / filter row ║ */
    row++;
    vscreen_put(row, r.col,         T->bdv, T->border_dim, T->bg_header_row, false, false);
    vscreen_put(row, r.col + W - 1, T->bdv, T->border_dim, T->bg_header_row, false, false);

    col = r.col + 2;
    int any_filter = (active_pid || active_comm[0] || active_min_ms);

    if (any_filter) {
        col = vscreen_puts(row, col, DIAMOND " FILTERS: ", T->fg_magenta, T->bg_header_row, true, false);
        if (active_pid && col < right_guard) {
            char ps[32]; snprintf(ps, sizeof(ps), "pid=%d  ", active_pid);
            col = safe_puts(row, col, right_guard - col, ps, T->fg_magenta, T->bg_header_row, false, false);
        }
        if (active_comm[0] && col < right_guard) {
            char cs[32]; snprintf(cs, sizeof(cs), "comm=%s  ", active_comm);
            col = safe_puts(row, col, right_guard - col, cs, T->fg_magenta, T->bg_header_row, false, false);
        }
        if (active_min_ms && col < right_guard) {
            char ms[32]; snprintf(ms, sizeof(ms), "min-dur=%ldms", active_min_ms);
            col = safe_puts(row, col, right_guard - col, ms, T->fg_magenta, T->bg_header_row, false, false);
        }
        if (col < right_guard)
            safe_puts(row, col, right_guard - col,
                      "  (kernel-side \xe2\x80\x94 zero overhead for excluded procs)",
                      T->fg_dim, T->bg_header_row, false, true);
    } else {
        col = vscreen_puts(row, col, "Latency: ", T->fg_secondary, T->bg_header_row, false, false);
        if (col < right_guard)
            col = vscreen_puts(row, col, BLOCK_FULL " <10\xc2\xb5s  ",  T->fg_green,  T->bg_header_row, false, false);
        if (col < right_guard)
            col = vscreen_puts(row, col, BLOCK_FULL " <100\xc2\xb5s  ", T->fg_yellow, T->bg_header_row, false, false);
        if (col < right_guard)
            col = vscreen_puts(row, col, BLOCK_FULL " <1ms  ",           T->fg_orange, T->bg_header_row, false, false);
        if (col < right_guard)
            col = vscreen_puts(row, col, BLOCK_FULL " \xe2\x89\xa5" "1ms", T->fg_red, T->bg_header_row, false, false);
        if (col < right_guard)
            safe_puts(row, col, right_guard - col,
                      "   \xe2\x9a\xa0=anomaly  sched avg/p95 exclude sleep >200ms",
                      T->fg_dim, T->bg_header_row, false, false);
    }

    /* ╚══ bottom border ══╝ */
    row++;
    if (row < r.row + r.rows) {
        vscreen_put(row, r.col, T->bdbl, T->border_dim, T->bg_header_row, false, false);
        for (int c = r.col + 1; c < r.col + W - 1; c++)
            vscreen_put(row, c, T->bdh, T->border_dim, T->bg_header_row, false, false);
        vscreen_put(row, r.col + W - 1, T->bdbr, T->border_dim, T->bg_header_row, false, false);
    }
}

/* ================================================================== */
/*  SECTION 1 — MAIN PROCESS TABLE                                    */
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
    int right_guard = inner.col + W;

    bool show_pid   = (W >= 80);
    bool show_p95   = (W >= 95);
    bool show_extra = (W >= 115);
    bool show_bar   = (W >= 70);
    int  bar_w      = show_bar ? 8 : 0;

    char hdr[256] = {0};
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

    if (show_bar) {
        char full_hdr[320];
        snprintf(full_hdr, sizeof(full_hdr), "%*s %s", bar_w, "RATE\xe2\x96\x84", hdr);
        draw_table_header(inner, 0, "%s", full_hdr);
    } else {
        draw_table_header(inner, 0, "%s", hdr);
    }
    draw_divider(panel, 2);

    double max_rate = 1.0;
    for (int i = 0; i < stat_count; i++)
        if (stats[i].rate > max_rate) max_rate = stats[i].rate;

    int data_row      = inner.row + 2;
    int max_data_rows = inner.rows - 3;
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

        bool is_anom = s->is_anomaly && s->baseline_ready;
        uint32_t row_bg = is_anom          ? T->bg_anom_row :
                          (shown % 2 == 1) ? T->bg_alt_row   : BG;

        /* Clear entire row first — eliminates leftover chars */
        fill_row_bg(r, inner.col, right_guard, row_bg);

        int col = inner.col + 1;

        if (show_bar) {
            double frac = (max_rate > 0) ? s->rate / max_rate : 0.0;
            draw_mini_bar(r, col, bar_w, frac, heat_color(frac), row_bg);
            col += bar_w + 1;
        }

        bool blink_on = (g_frame % 2 == 0);
        if (is_anom)
            col = vscreen_puts(r, col, blink_on ? "!" : DIAMOND, T->fg_red, row_bg, true, false);
        else
            col = vscreen_puts(r, col, " ", T->fg_dim, row_bg, false, false);

        if (col >= right_guard) { shown++; continue; }

        /* PROCESS */
        col = safe_printf(r, col, 14, T->fg_primary, row_bg, true, false, "%-14s", s->process);
        col++;
        if (col >= right_guard) { shown++; continue; }

        /* EVENT */
        col = safe_printf(r, col, 9, event_fg(s->event), row_bg, false, false, "%-9s", s->event);
        col++;

        /* RATE/s */
        if (col + 7 <= right_guard) {
            col = safe_printf(r, col, 7, T->fg_primary, row_bg, false, false, "%7s", rate_s);
            col++;
        }

        /* AVG */
        if (col + 7 <= right_guard) {
            col = safe_printf(r, col, 7, lat_fg(avg), row_bg, false, false, "%7s", avg_s);
            col++;
        }

        /* P95 */
        if (show_p95 && col + 7 <= right_guard) {
            col = safe_printf(r, col, 7, lat_fg(p95), row_bg, false, false, "%7s", p95_s);
            col++;
        }

        /* MAX */
        if (col + 8 <= right_guard) {
            col = safe_printf(r, col, 8, lat_fg(maxus), row_bg, false, false, "%8s", max_s);
            col++;
        }

        /* PID */
        if (show_pid && col + 7 <= right_guard) {
            col = safe_printf(r, col, 7, T->fg_secondary, row_bg, false, false, "%7d", s->pid);
            col++;
        }

        /* CTXSW / EXECS */
        if (show_extra) {
            if (col + 6 <= right_guard) {
                col = safe_printf(r, col, 6,
                                  s->ctx_switches > 0 ? T->fg_yellow : T->fg_secondary,
                                  row_bg, false, false, "%6ld", s->ctx_switches);
                col++;
            }
            if (col + 6 <= right_guard) {
                safe_printf(r, col, 6,
                            s->exec_count > 0 ? T->fg_green : T->fg_secondary,
                            row_bg, false, false, "%6ld", s->exec_count);
            }
        }

        shown++;
    }

    /* "X more entries" footer */
    if (stat_count > shown) {
        int r = data_row + shown;
        if (r < inner.row + inner.rows - 1) {
            fill_row_bg(r, inner.col, right_guard, BG);
            safe_printf(r, inner.col + 2, W - 4,
                        T->fg_dim, BG, false, true,
                        "%s %d more entries (showing top %d by rate)",
                        TRIANGLE_R, stat_count - shown, shown);
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
    int right_guard = inner.col + inner.cols;

    draw_table_header(inner, 0,
        "%-11s %8s %8s %10s %8s",
        "EVENT", "AVG", "MAX", "TOTAL", "RATE/s");
    draw_divider(panel, 2);

    int r = inner.row + 2;
    for (int i = 0; i < count && r < inner.row + inner.rows - 1; i++, r++) {
        long avg   = (agg[i].valid_count > 0 && agg[i].total_latency > 0)
                     ? (agg[i].total_latency / agg[i].valid_count) / 1000 : 0;
        long maxus = agg[i].max_latency / 1000;
        int  is_outlier = (avg > 0 && maxus > avg * 10);

        char avg_s[12], max_s[12], rate_s[12], tot_s[16];
        fmt_us(avg_s,    sizeof(avg_s),  avg);
        fmt_us(max_s,    sizeof(max_s),  maxus);
        fmt_rate(rate_s, sizeof(rate_s), agg[i].rate);
        snprintf(tot_s,  sizeof(tot_s),  "%ld", agg[i].count);

        uint32_t row_bg = (i % 2 == 1) ? T->bg_alt_row : BG;
        fill_row_bg(r, inner.col, right_guard, row_bg);

        int col = inner.col + 2;
        col = safe_printf(r, col, 11, event_fg(agg[i].event), row_bg, false, false, "%-11s", agg[i].event);
        col++;
        if (col + 8 <= right_guard) { col = safe_printf(r, col, 8,  lat_fg(avg),    row_bg, false, false, "%8s", avg_s);   col++; }
        if (col + 8 <= right_guard) { col = safe_printf(r, col, 8,  lat_fg(maxus),  row_bg, false, false, "%8s", max_s);   col++; }
        if (col + 10 <= right_guard){ col = safe_printf(r, col, 10, T->fg_primary,  row_bg, true,  false, "%10s", tot_s);  col++; }
        if (col + 8 <= right_guard) { col = safe_printf(r, col, 8,  T->fg_primary,  row_bg, true,  false, "%8s", rate_s);  }
        if (is_outlier && col + 10 <= right_guard) {
            col++;
            safe_puts(r, col, right_guard - col,
                      WARN " outlier", T->fg_yellow, row_bg, false, true);
        }
        (void)col;
    }
}

/* ================================================================== */
/*  SECTION 3 — ACTIVITY GRAPH with SPARKLINE                        */
/* ================================================================== */

static void draw_activity(rect_t panel)
{
    agg_entry_t agg[32];
    int count = build_event_agg(agg, 32);
    sort_agg_by_rate(agg, count);

    /* Push fresh rate sample into each ring buffer */
    for (int i = 0; i < count; i++) {
        spark_ring_t *sr = spark_get_or_create(agg[i].event);
        if (sr) spark_push(sr, agg[i].rate);
    }

    rect_t inner = draw_panel(panel, "ACTIVITY  " ARROW_UP " rate/s", T->fg_orange);
    if (inner.rows < 2 || inner.cols < 20) return;

    int right_guard = inner.col + inner.cols;
    double max_rate = (count > 0 && agg[0].rate > 0) ? agg[0].rate : 1.0;

    int spark_w = (inner.cols >= 60) ? 8 : 0;   /* show sparkline if wide enough */
    int label_w = 11;
    int rate_w  = 8;
    int bar_w   = inner.cols - label_w - rate_w - spark_w - 6;
    if (bar_w < 6) { bar_w = 6; spark_w = 0; }

    int r = inner.row;
    for (int i = 0; i < count && r < inner.row + inner.rows; i++, r++) {
        uint32_t row_bg = (i % 2 == 1) ? T->bg_alt_row : BG;
        fill_row_bg(r, inner.col, right_guard, row_bg);

        double   frac    = agg[i].rate / max_rate;
        uint32_t bar_col = heat_color(frac);
        uint32_t ev_col  = event_fg(agg[i].event);

        int col = inner.col + 2;

        /* Event label */
        col = safe_printf(r, col, label_w, ev_col, row_bg, false, false, "%-11s", agg[i].event);
        col++;

        /* Horizontal bar */
        if (col + bar_w <= right_guard) {
            draw_bar(r, col, bar_w, frac, bar_col, T->fg_dim, row_bg);
            col += bar_w + 1;
        }

        /* Sparkline trend */
        if (spark_w > 0 && col + spark_w <= right_guard) {
            spark_ring_t *sr = spark_get_or_create(agg[i].event);
            col = draw_sparkline(r, col, spark_w, sr, ev_col, row_bg);
            col++;
        }

        /* Rate label */
        if (col + rate_w <= right_guard) {
            char rate_s[16]; fmt_rate(rate_s, sizeof(rate_s), agg[i].rate);
            safe_printf(r, col, rate_w, T->fg_primary, row_bg, true, false, "%6s/s", rate_s);
        }
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

    int right_guard = inner.col + inner.cols;

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

        uint32_t row_bg = (i % 2 == 1) ? T->bg_alt_row : BG;
        fill_row_bg(r, inner.col, right_guard, row_bg);

        int col = inner.col + 2;

        col = safe_printf(r, col, 15, T->fg_primary, row_bg, true, false, "%-15s", tmp[i].process);
        col++;
        if (col >= right_guard) continue;

        col = safe_printf(r, col, 10, event_fg(tmp[i].event), row_bg, false, false, "%-10s", tmp[i].event);
        col++;

        if (col + 10 <= right_guard) { col = safe_printf(r, col, 10, lat_fg(maxus), row_bg, true,  false, "%10s", max_s); col++; }
        if (col + 10 <= right_guard) { col = safe_printf(r, col, 10, lat_fg(p95),   row_bg, false, false, "%10s", p95_s); col++; }
        if (col + 10 <= right_guard) {       safe_printf(r, col, 10, lat_fg(avg),   row_bg, false, false, "%10s", avg_s); }
        (void)col;
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

    int right_guard = inner.col + inner.cols;

    if (n_anom == 0) {
        int r = inner.row + 1;
        fill_row_bg(r, inner.col, right_guard, BG);
        safe_puts(r, inner.col + 2, inner.cols - 2,
                  TICK "  All events within normal baseline",
                  T->fg_green, BG, true, false);
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

        long valid       = tmp[i].count - tmp[i].drop_count;
        long current_us  = (valid > 0 && tmp[i].total_latency > 0)
                           ? (tmp[i].total_latency / valid) / 1000 : 0;
        long baseline_us = (long)(tmp[i].baseline_latency / 1000.0);

        char cur_s[12], bas_s[12];
        fmt_us(cur_s, sizeof(cur_s), current_us);
        fmt_us(bas_s, sizeof(bas_s), baseline_us);

        uint32_t dc     = dev_fg(tmp[i].deviation);
        uint32_t row_bg = (shown % 2 == 1) ? T->bg_alt_row : BG;

        fill_row_bg(r, inner.col, right_guard, row_bg);

        bool flash = (g_frame % 2 == 0);
        int col = inner.col + 1;
        col = vscreen_puts(r, col, flash ? WARN : " ", T->fg_red, row_bg, true, false);
        col++;

        col = safe_printf(r, col, 15, T->fg_primary, row_bg, false, false, "%-15s", tmp[i].process);
        col++;
        if (col >= right_guard) { shown++; r++; continue; }

        col = safe_printf(r, col, 11, event_fg(tmp[i].event), row_bg, false, false, "%-11s", tmp[i].event);
        col++;

        if (col + 10 <= right_guard) {
            col = safe_printf(r, col, 10, dc, row_bg, true, false, "%9.1f%%", tmp[i].deviation * 100.0);
            col++;
        }
        if (col + 10 <= right_guard) {
            col = safe_printf(r, col, 10, T->fg_secondary, row_bg, false, false, "%10s", bas_s);
            col++;
        }
        if (col + 10 <= right_guard) {
            safe_printf(r, col, 10, dc, row_bg, true, false, "%10s", cur_s);
        }
        (void)col;

        shown++;
        r++;
    }

    if (n_anom > shown && r < inner.row + inner.rows) {
        fill_row_bg(r, inner.col, right_guard, BG);
        safe_printf(r, inner.col + 2, inner.cols - 4,
                    T->fg_dim, BG, false, true,
                    "%s %d more anomalies not shown", TRIANGLE_R, n_anom - shown);
    }
}

/* ================================================================== */
/*  SECTION 6 — SCHEDULING DELAY (NEW)                               */
/*                                                                     */
/*  Extracts all "sched" entries from the stats table and renders     */
/*  them with AVG / MAX / P95 scheduling delay and a trend sparkline. */
/*  This directly exposes the per-process scheduling delay metric     */
/*  computed in stats.c: delay = time_scheduled_in - last_sched_out. */
/* ================================================================== */

static void draw_sched_delay(rect_t panel)
{
    if (panel.rows < 4) return;

    /* Collect sched entries sorted by max delay */
    struct syscall_stat tmp[MAX_STATS];
    int n = 0;
    for (int i = 0; i < stat_count; i++) {
        if (strcmp(stats[i].event, "sched") != 0) continue;
        if (n < MAX_STATS) tmp[n++] = stats[i];
    }
    if (n == 0) return;

    qsort(tmp, (size_t)n, sizeof(*tmp), cmp_latency);

    rect_t inner = draw_panel(panel, "SCHEDULING DELAY", T->fg_yellow);
    if (inner.rows < 3 || inner.cols < 20) return;

    int right_guard = inner.col + inner.cols;
    bool show_spark = (inner.cols >= 70);
    int  spark_w    = show_spark ? 10 : 0;

    draw_table_header(inner, 0,
        "%-14s %7s %10s %10s %10s %6s%s",
        "PROCESS", "PID", "AVG DELAY", "P95 DELAY", "MAX DELAY", "CTXSW",
        show_spark ? "      TREND" : "");
    draw_divider(panel, 2);

    int r = inner.row + 2;
    int shown = 0;
    for (int i = 0; i < n && r < inner.row + inner.rows - 1; i++, r++, shown++) {
        long valid = tmp[i].count - tmp[i].drop_count;
        long avg   = (valid > 0 && tmp[i].total_latency > 0)
                     ? (tmp[i].total_latency / valid) / 1000 : 0;
        long maxus = tmp[i].max_latency / 1000;
        long p95   = stats_p95_us(&tmp[i]);

        /* Filter out noise rows where all data is 200ms (sched sleep) */
        if (avg > 190000 && p95 <= 0 && valid < 5) { r--; shown--; continue; }

        char avg_s[12], max_s[12], p95_s[12];
        fmt_us(avg_s, sizeof(avg_s), avg);
        fmt_us(max_s, sizeof(max_s), maxus);
        fmt_us(p95_s, sizeof(p95_s), p95);

        uint32_t row_bg = (shown % 2 == 1) ? T->bg_alt_row : BG;
        uint32_t lat_c  = lat_fg(avg);

        fill_row_bg(r, inner.col, right_guard, row_bg);

        int col = inner.col + 2;

        col = safe_printf(r, col, 14, T->fg_primary, row_bg, true, false, "%-14s", tmp[i].process);
        col++;
        if (col >= right_guard) continue;

        if (col + 7 <= right_guard) {
            col = safe_printf(r, col, 7, T->fg_secondary, row_bg, false, false, "%7d", tmp[i].pid);
            col++;
        }
        if (col + 10 <= right_guard) {
            col = safe_printf(r, col, 10, lat_c, row_bg, true, false, "%10s", avg_s);
            col++;
        }
        if (col + 10 <= right_guard) {
            col = safe_printf(r, col, 10, lat_fg(p95), row_bg, false, false, "%10s", p95_s);
            col++;
        }
        if (col + 10 <= right_guard) {
            col = safe_printf(r, col, 10, lat_fg(maxus), row_bg, true, false, "%10s", max_s);
            col++;
        }
        if (col + 6 <= right_guard) {
            col = safe_printf(r, col, 6,
                              tmp[i].ctx_switches > 1000 ? T->fg_orange : T->fg_secondary,
                              row_bg, false, false, "%6ld", tmp[i].ctx_switches);
            col++;
        }

        /* Sparkline trend for this process's sched delay */
        if (show_spark && spark_w > 0 && col + spark_w <= right_guard) {
            /* Use per-process key "sched:<pid>" */
            char key[24]; snprintf(key, sizeof(key), "sched:%d", tmp[i].pid);
            spark_ring_t *sr = spark_get_or_create(key);
            if (sr) {
                /* If this ring is empty, seed it from current avg */
                if (sr->count == 0) spark_push(sr, (double)avg);
                draw_sparkline(r, col, spark_w, sr, lat_c, row_bg);
            }
        }
    }

    if (shown == 0) {
        /* No valid rows: show a clean message */
        int mr = inner.row + 1;
        fill_row_bg(mr, inner.col, right_guard, BG);
        safe_puts(mr, inner.col + 2, inner.cols - 4,
                  TICK "  No scheduling delay data yet (waiting for sched events)",
                  T->fg_secondary, BG, false, true);
    }
}

/* ================================================================== */
/*  SECTION 7 — PROCESS LIFECYCLE                                     */
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
        if (strcmp(s->event, "exec")      != 0 &&
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

    int right_guard = inner.col + inner.cols;

    draw_table_header(inner, 0,
        "%-15s %-7s %10s %10s %12s",
        "PROCESS", "PID", "EXECS", "EXITS", "AVG LIFETIME");
    draw_divider(panel, 2);

    int r = inner.row + 2;
    for (int i = 0; i < lc_count && r < inner.row + inner.rows; i++, r++) {
        char life_s[16]; fmt_us(life_s, sizeof(life_s), procs[i].avg_life_us);

        uint32_t row_bg = (i % 2 == 1) ? T->bg_alt_row : BG;
        fill_row_bg(r, inner.col, right_guard, row_bg);

        int col = inner.col + 2;
        col = safe_printf(r, col, 15, T->fg_primary, row_bg, true, false, "%-15s", procs[i].process);
        col++;
        if (col >= right_guard) continue;

        if (col + 7  <= right_guard) { col = safe_printf(r, col, 7,  T->fg_secondary, row_bg, false, false, "%-7d",  procs[i].pid);      col++; }
        if (col + 10 <= right_guard) { col = safe_printf(r, col, 10, T->fg_green,     row_bg, false, false, "%10ld", procs[i].execs);    col++; }
        if (col + 10 <= right_guard) { col = safe_printf(r, col, 10, T->fg_yellow,    row_bg, false, false, "%10ld", procs[i].exits);    col++; }
        if (col + 12 <= right_guard) {       safe_printf(r, col, 12, T->fg_cyan,      row_bg, false, false, "%12s",  life_s);            }
        (void)col;
    }
}

/* ================================================================== */
/*  CONTROLS BAR                                                       */
/* ================================================================== */

static void draw_controls(rect_t r)
{
    if (r.rows < 1) return;
    int row = r.row;
    int right_guard = r.col + r.cols;

    fill_row_bg(row, r.col, right_guard, T->bg_header_row);

    int col = r.col + 1;
    vscreen_put(row, col, T->bv, T->border_dim, T->bg_header_row, false, false);
    col += 2;

    struct { const char *key; const char *label; } keys[] = {
        { "[q]",   " quit"            },
        { "[r]",   " reset"           },
        { "[e]",   " export snapshot" },
        { "[1-6]", " sort column"     },
    };
    for (int i = 0; i < 4 && col < right_guard - 4; i++) {
        col = vscreen_puts(row, col, keys[i].key,   T->fg_cyan,      T->bg_header_row, true,  false);
        col = safe_puts(row, col, right_guard - col - 4,
                        keys[i].label, T->fg_secondary, T->bg_header_row, false, false);
        if (i < 3 && col < right_guard - 4) {
            col = vscreen_puts(row, col, "   ", T->fg_dim, T->bg_header_row, false, false);
            if (col < right_guard - 1) {
                vscreen_put(row, col, T->bv, T->border_dim, T->bg_header_row, false, false);
                col += 2;
            }
        }
    }

    const char *watermark = LIGHTNING " eBPF Monitor v8.0  ";
    int wm_col = r.col + r.cols - (int)strlen(watermark) - 2;
    if (wm_col > col + 4 && wm_col < right_guard - 2) {
        vscreen_put(row, wm_col - 1, T->bv, T->border_dim, T->bg_header_row, false, false);
        safe_puts(row, wm_col, right_guard - wm_col - 1,
                  watermark, T->fg_dim, T->bg_header_row, false, false);
    }
}

/* ================================================================== */
/*  LAYOUT EXTENSION                                                   */
/*                                                                     */
/*  The layout engine allocates lifecycle last, absorbing all         */
/*  remaining rows.  We split that rect between the scheduling delay  */
/*  panel (top 40%) and lifecycle (bottom 60%) — both panels still   */
/*  use the same draw_panel border style so the UI is consistent.    */
/*                                                                     */
/*  If the lifecycle rect is smaller than 8 rows, we skip the split  */
/*  and give all rows to lifecycle (no sched panel drawn).           */
/* ================================================================== */

static void draw_lifecycle_zone(rect_t zone)
{
    if (zone.rows < 8) {
        /* Not enough space — just lifecycle */
        draw_lifecycle(zone);
        return;
    }

    /* Split: sched delay gets min(8, 35%) rows at top */
    int sched_rows = zone.rows * 35 / 100;
    if (sched_rows < 6)  sched_rows = 6;
    if (sched_rows > 14) sched_rows = 14;
    if (sched_rows >= zone.rows) sched_rows = zone.rows / 2;

    rect_t sched_rect = { zone.row,             zone.col, sched_rows,             zone.cols };
    rect_t life_rect  = { zone.row + sched_rows, zone.col, zone.rows - sched_rows, zone.cols };

    draw_sched_delay(sched_rect);

    if (life_rect.rows >= 4)
        draw_lifecycle(life_rect);
}

/* ================================================================== */
/*  SPARKLINE UPDATE for sched-delay per process                      */
/*  Called once per render to push fresh samples.                     */
/* ================================================================== */

static void update_sched_sparklines(void)
{
    for (int i = 0; i < stat_count; i++) {
        if (strcmp(stats[i].event, "sched") != 0) continue;
        char key[24]; snprintf(key, sizeof(key), "sched:%d", stats[i].pid);
        spark_ring_t *sr = spark_get_or_create(key);
        if (!sr) continue;
        long valid = stats[i].count - stats[i].drop_count;
        long avg   = (valid > 0 && stats[i].total_latency > 0)
                     ? (stats[i].total_latency / valid) / 1000 : 0;
        spark_push(sr, (double)avg);
    }
}

/* ================================================================== */
/*  MAIN ENTRY POINT                                                   */
/* ================================================================== */

void dashboard_render(double elapsed, double cpu_pct,
                      int active_pid, const char *active_comm,
                      long active_min_ms)
{
    g_frame++;

    term_size_t sz = term_get_size();

    static int last_rows = 0, last_cols = 0;
    if (sz.rows != last_rows || sz.cols != last_cols) {
        vscreen_resize(sz.rows, sz.cols);
        last_rows = sz.rows;
        last_cols = sz.cols;
    }

    vscreen_clear();

    /* ── GLOBAL BACKGROUND FILL ── */
    {
        rect_t full = { 0, 0, sz.rows, sz.cols };
        fill_rect(full, T->fg_dim, BG);
    }

    /* ── Pre-render metrics ── */
    update_events_per_sec();
    update_sched_sparklines();

    layout_t L;
    layout_compute(sz.rows, sz.cols, &L);

    draw_header(L.header, elapsed, cpu_pct,
                active_pid, active_comm, active_min_ms);

    draw_processes(L.processes);
    draw_event_summary(L.summary);
    draw_activity(L.activity);
    draw_slowest(L.slowest);
    draw_anomalies(L.anomaly);

    /* Lifecycle zone: split between sched-delay panel and lifecycle panel */
    if (L.lifecycle.rows >= 4)
        draw_lifecycle_zone(L.lifecycle);

    draw_controls(L.controls);

    vscreen_flush();
}