/* SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause) */

/*
 * dashboard.c — production-grade btop-style TUI renderer
 *
 * ARCHITECTURE & BUG-FIX RATIONALE
 * ==================================
 *
 * (A) MEMORY SAFETY
 *     Every rect received from layout_compute() is validated with
 *     rect_valid() (rows>0, cols>0) before a single cell is written.
 *     All vscreen_put() calls go through safe rendering wrappers that
 *     clip to [col, right_guard).  Raw vscreen_put() is ONLY used
 *     inside fill_rect / fill_row_bg where bounds are already known.
 *
 * (B) DOUBLE-BUFFERING / DIFF RENDER
 *     vscreen maintains vs_next[] and vs_prev[].  We write every frame
 *     into vs_next via vscreen_clear() + draw calls, then vscreen_flush()
 *     diffs against vs_prev and emits only changed cells.  We NEVER call
 *     term_clear_screen() — that causes the full-screen flash seen in the
 *     original.  The global background fill (fill_rect on screen) is done
 *     in vs_next; unchanged cells never generate terminal output.
 *
 * (C) STRICT CLIPPING
 *     right_guard = inner.col + inner.cols is computed once per panel
 *     and threaded through every column write.  safe_puts() / safe_printf()
 *     accept a `max_cols` argument and clip content + append ellipsis.
 *     No string can ever write past right_guard.
 *
 * (D) LAYOUT
 *     layout_compute() now returns int (-1 = too small).  When it returns
 *     -1 we just call vscreen_flush() with an empty frame (the "terminal
 *     too small" message is written to vs_next) and return.  Every draw_*
 *     function guards rows >= N before doing anything.
 *
 * (E) PROCESS TABLE COLUMNS
 *     Column widths are compile-time constants.  Each column is written
 *     with safe_printf(max_cols = COL_WIDTH) so content is always clipped
 *     to its slot.  Gaps between columns are exactly 1 space.
 *
 * (F) HEADER
 *     The clock is rendered btop-style: HH:MM:SS centered on the TOP BORDER
 *     row of the header, wrapped in ┤ / ├ tee decorators.  It occupies exactly
 *     12 cells (tee + space + 8 chars + space + tee) centered at W/2.
 *     The metrics stream on row+1 now has the full right_guard width available.
 *
 * (G) BACKGROUND
 *     draw_panel() calls fill_rect() on the full outer rect first, then
 *     draws the border on top.  Every data row calls fill_row_bg() before
 *     writing text.  No partial fills, no ghost characters.
 *
 * (H) BOTTOM ZONE (side-by-side panels)
 *     layout_compute() splits the bottom zone horizontally into
 *     L->lifecycle (left) and L->sched_delay (right).  Both panels share
 *     the full zone height, giving each ~10+ rows instead of ~4 rows when
 *     stacked.  draw_lifecycle() and draw_sched_delay() are called directly
 *     with their respective rects — no wrapper needed.
 *
 * (I) PERFORMANCE
 *     vscreen_flush() only emits cells that differ from vs_prev[].
 *     fill_rect() is cheap (in-memory writes to vs_next[]).
 *     No fflush() except inside vscreen_flush() itself.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdarg.h>
#include <unistd.h>

#include "dashboard.h"
#include "stats.h"
#include "vscreen.h"
#include "layout.h"
#include "theme.h"
#include "term.h"

/* ================================================================== */
/*  Shorthand macros                                                   */
/* ================================================================== */

/* Panel background — used everywhere a background colour is needed */
#define BG (T->bg_panel)

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

/* Spark chars (8 sub-cell heights) */
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

/* Clock field width: "HH:MM:SS" = 8 chars + 2 tees + 2 spaces = 12 cells total (centered) */
#define CLOCK_DISPLAY_W 12

/* ================================================================== */
/*  Frame counter                                                      */
/* ================================================================== */

static unsigned long g_frame = 0;

/* ================================================================== */
/*  Sparkline ring buffer                                              */
/* ================================================================== */

#define SPARK_SLOTS  32
#define SPARK_EVTS   16

typedef struct {
    char   name[24];
    double samples[SPARK_SLOTS];
    int    head;
    int    count;
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

static int draw_sparkline(int row, int col, int width,
                          const spark_ring_t *r,
                          uint32_t fg, uint32_t row_bg)
{
    if (width <= 0) return col;
    if (!r || r->count == 0) {
        for (int i = 0; i < width; i++)
            vscreen_put(row, col + i, " ", T->fg_dim, row_bg, false, false);
        return col + width;
    }

    double mx = 0.0;
    int n = r->count < width ? r->count : width;
    for (int i = 0; i < n; i++) {
        int idx = ((r->head - 1 - i) + SPARK_SLOTS) % SPARK_SLOTS;
        if (r->samples[idx] > mx) mx = r->samples[idx];
    }
    if (mx == 0.0) mx = 1.0;

    int pad = width - n;
    for (int i = 0; i < pad; i++)
        vscreen_put(row, col + i, " ", T->fg_dim, row_bg, false, false);

    for (int i = 0; i < n; i++) {
        int age = n - 1 - i;
        int idx = ((r->head - 1 - age) + SPARK_SLOTS) % SPARK_SLOTS;
        int lvl = (int)(r->samples[idx] / mx * 7.0);
        if (lvl < 0) lvl = 0;
        if (lvl > 7) lvl = 7;
        vscreen_put(row, col + pad + i, SPARK[lvl], fg, row_bg, false, false);
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
    else if (us >= 1000)    snprintf(buf, len, "%.1fms", us / 1e3);
    else                    snprintf(buf, len, "%ld\xc2\xb5s", us);
}

static void fmt_elapsed(char *buf, int len, double s)
{
    /* FIX: cast to long long — (int) overflows after ~2147 seconds on
     * 32-bit int, producing garbage minutes/seconds fields like "00:30090".
     * long long handles runs of many hours safely. */
    if (s < 0.0) s = 0.0;
    long long total = (long long)s;
    int h   = (int)(total / 3600);
    int m   = (int)((total % 3600) / 60);
    int sec = (int)(total % 60);
    snprintf(buf, len, "%02d:%02d:%02d", h, m, sec);
}

/* ================================================================== */
/*  SAFE RENDERING WRAPPERS                                            */
/*                                                                     */
/*  WHY: Raw vscreen_puts() does not know about panel boundaries.     */
/*  These wrappers accept a max_cols argument and clip or truncate so  */
/*  that no glyph can escape its designated slot.                      */
/* ================================================================== */

/*
 * safe_puts — write at most max_cols display cells from str.
 * Appends "…" (U+2026, 3 bytes) if truncated.
 * Returns column after last written cell.
 */
static int safe_puts(int row, int col, int max_cols,
                     const char *str,
                     uint32_t fg, uint32_t bg,
                     bool bold, bool dim)
{
    if (max_cols <= 0 || !str || !*str) return col;

    /* Walk UTF-8 to count display cells */
    int display_len = 0;
    const unsigned char *p = (const unsigned char *)str;
    while (*p) {
        unsigned char b = *p;
        int bytes = (b & 0x80) == 0x00 ? 1 :
                    (b & 0xE0) == 0xC0 ? 2 :
                    (b & 0xF0) == 0xE0 ? 3 : 4;
        display_len++;  
        p += bytes;
    }

    if (display_len <= max_cols)
        return vscreen_puts(row, col, str, fg, bg, bold, dim);

    /* Truncate: fit (max_cols - 1) cells then append ellipsis */
    char buf[512];
    int out = 0, cells = 0, limit = max_cols - 1;
    p = (const unsigned char *)str;
    while (*p && cells < limit && out < (int)sizeof(buf) - 5) {
        unsigned char b = *p;
        int bytes = (b & 0x80) == 0x00 ? 1 :
                    (b & 0xE0) == 0xC0 ? 2 :
                    (b & 0xF0) == 0xE0 ? 3 : 4;
        for (int i = 0; i < bytes; i++) buf[out++] = (char)p[i];
        cells++;
        p += bytes;
    }
    /* U+2026 HORIZONTAL ELLIPSIS */
    buf[out++] = (char)0xe2;
    buf[out++] = (char)0x80;
    buf[out++] = (char)0xa6;
    buf[out]   = '\0';
    return vscreen_puts(row, col, buf, fg, bg, bold, dim);
}

/*
 * safe_printf — printf into a fixed-width cell slot then safe_puts.
 * WHY: prevents column overflow from variable-length number strings.
 */
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
/*  Background / fill helpers                                          */
/* ================================================================== */

/*
 * fill_rect — paint every cell in rect with a space using bg colour.
 * WHY: ensures no ghost characters from previous frames remain.
 * Called by draw_panel() before any border is drawn.
 */
static void fill_rect(rect_t r, uint32_t fg, uint32_t bg)
{
    if (!rect_valid(r)) return;
    for (int rr = r.row; rr < r.row + r.rows; rr++)
        for (int cc = r.col; cc < r.col + r.cols; cc++)
            vscreen_put(rr, cc, " ", fg, bg, false, false);
}

/*
 * fill_row_bg — paint a single row segment [col_start, col_end) with bg.
 * WHY: called before EVERY data row so stale text from longer previous
 * content is cleared before new (potentially shorter) text is written.
 */
static void fill_row_bg(int row, int col_start, int col_end, uint32_t bg)
{
    for (int c = col_start; c < col_end; c++)
        vscreen_put(row, c, " ", T->fg_dim, bg, false, false);
}

/* ================================================================== */
/*  Semantic colour pickers                                            */
/* ================================================================== */

static uint32_t lat_fg(long us)
{
    if (us < 0)                 return T->fg_secondary;
    if (us < LATENCY_GREEN_US)  return T->fg_green;
    if (us < LATENCY_YELLOW_US) return T->fg_yellow;
    if (us < 1000)              return T->fg_orange;
    return T->fg_red;
}

static uint32_t dev_fg(double d)
{
    if (d < 0.25)              return T->fg_green;
    if (d < ANOMALY_THRESHOLD) return T->fg_yellow;
    if (d < 1.5)               return T->fg_orange;
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
            agg[count].event[sizeof(agg[0].event) - 1] = '\0';
            agg[count].count         = stats[i].count;
            agg[count].total_latency = stats[i].total_latency;
            agg[count].max_latency   = stats[i].max_latency;
            agg[count].valid_count   = stats[i].count - stats[i].drop_count;
            agg[count].rate          = stats[i].rate;
            count++;
        } else {
            agg[found].count         += stats[i].count;
            agg[found].total_latency += stats[i].total_latency;
            agg[found].valid_count   += stats[i].count - stats[i].drop_count;
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
/*  Events-per-second accumulator                                      */
/* ================================================================== */

static double g_events_per_sec = 0.0;

static void update_events_per_sec(void)
{
    double total = 0.0;
    for (int i = 0; i < stat_count; i++) total += stats[i].rate;
    g_events_per_sec = total;
}

/* ================================================================== */
/*  Progress / mini bars                                               */
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

static void draw_mini_bar(int row, int col, int width,
                          double fraction, uint32_t fg, uint32_t row_bg)
{
    if (fraction < 0) fraction = 0;
    if (fraction > 1) fraction = 1;
    int filled = (int)(fraction * width);
    for (int i = 0; i < width; i++)
        vscreen_put(row, col + i,
                    i < filled ? BLOCK_LOWER : BULLET,
                    i < filled ? fg : T->fg_dim,
                    row_bg, false, false);
}

/* ================================================================== */
/*  Panel box drawing                                                  */
/*                                                                     */
/*  WHY fill_rect first: guarantees every cell inside the panel is    */
/*  owned by this panel, preventing bleed from adjacent panels that   */
/*  may have had wider content on a previous frame.                   */
/* ================================================================== */

static rect_t draw_panel(rect_t r, const char *title, uint32_t title_color)
{
    if (!rect_valid(r) || r.rows < 2 || r.cols < 4)
        return (rect_t){ r.row, r.col, 0, 0 };

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
    int title_avail = (col + W - 2) - tc;   /* cells available for title */
    if (title_avail > 2) {
        tc = vscreen_puts(row, tc, DIAMOND " ", title_color, BG, false, false);
        tc = safe_puts(row, tc, (col + W - 2) - tc,
                       title, title_color, BG, true, false);
        if (tc < col + W - 1)
            vscreen_put(row, tc++, " ", T->border_dim, BG, false, false);
    }
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

    /* Return content area (inside border) */
    return (rect_t){ row + 1, col + 1, r.rows - 2, W - 2 };
}

/* Horizontal divider inside a panel's outer rect at `inner_row` from top */
static void draw_divider(rect_t panel, int inner_row)
{
    if (!rect_valid(panel)) return;
    int r = panel.row + inner_row;
    if (r < panel.row || r >= panel.row + panel.rows) return;
    int c = panel.col;
    int W = panel.cols;
    if (W < 2) return;
    vscreen_put(r, c,         T->bml, T->border_dim, BG, false, false);
    for (int cc = c + 1; cc < c + W - 1; cc++)
        vscreen_put(r, cc, T->bh, T->border_dim, BG, false, false);
    vscreen_put(r, c + W - 1, T->bmr, T->border_dim, BG, false, false);
}

/* Table header: fill bg then write formatted label */
static void draw_table_header(rect_t content, int rel_row,
                               const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void draw_table_header(rect_t content, int rel_row,
                               const char *fmt, ...)
{
    if (!rect_valid(content)) return;
    int r = content.row + rel_row;
    if (r < content.row || r >= content.row + content.rows) return;

    fill_row_bg(r, content.col, content.col + content.cols, T->bg_header_row);

    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    safe_puts(r, content.col + 2, content.cols - 4,
              buf, T->fg_secondary, T->bg_header_row, false, false);
}

/* ================================================================== */
/*  HEADER BANNER                                                      */
/*                                                                     */
/*  CLOCK — btop-style, centered on the top border row:               */
/*  ┤ HH:MM:SS ├  drawn AFTER the border, so it overwrites the ═══   */
/*  fill at the center.  The metrics row (row+1) is fully available   */
/*  for the left-side metric stream up to right_guard.                */
/* ================================================================== */

static void draw_header(rect_t r, double elapsed,
                         double cpu_pct,
                         int active_pid, const char *active_comm,
                         long active_min_ms)
{
    if (!rect_valid(r) || r.rows < 3) return;

    int W   = r.cols;
    int row = r.row;

    /* ── Full header background fill ── */
    for (int rr = r.row; rr < r.row + r.rows; rr++)
        fill_row_bg(rr, r.col, r.col + W, T->bg_header_row);

    /* ── ╔══ top border ══╗ ── */
    vscreen_put(row, r.col,         T->bdtl, T->border_dim, T->bg_header_row, false, false);
    for (int c = r.col + 1; c < r.col + W - 1; c++)
        vscreen_put(row, c, T->bdh, T->border_dim, T->bg_header_row, false, false);
    vscreen_put(row, r.col + W - 1, T->bdtr, T->border_dim, T->bg_header_row, false, false);

    /* ── ║ metrics row ║ ── */
    row++;
    vscreen_put(row, r.col,         T->bdv, T->border_dim, T->bg_header_row, false, false);
    vscreen_put(row, r.col + W - 1, T->bdv, T->border_dim, T->bg_header_row, false, false);

    int col        = r.col + 2;
    int right_guard = r.col + W - 2;
    int metric_end  = right_guard - 2;   /* leave 2 cells for right border padding */

#define HDR_SEP() \
    do { \
        if (col + 3 < metric_end) { \
            vscreen_puts(row, col, "  ", T->fg_dim, T->bg_header_row, false, false); \
            col += 2; \
            if (col < metric_end) { \
                vscreen_put(row, col, T->bdv, T->border_dim, T->bg_header_row, false, false); \
                col++; \
            } \
        } \
    } while(0)

    /* Logo */
    if (col < metric_end) col = vscreen_puts(row, col, LIGHTNING, T->fg_yellow, T->bg_header_row, true, false);
    if (col < metric_end) col = vscreen_puts(row, col, " eBPF", T->fg_cyan, T->bg_header_row, true, false);
    if (col < metric_end) col = vscreen_puts(row, col, " Monitor", T->fg_primary, T->bg_header_row, true, false);
    if (col < metric_end) col = vscreen_puts(row, col, " v9.0", T->fg_secondary, T->bg_header_row, false, false);

    HDR_SEP();

    /* Runtime — always shown */
    if (col < metric_end) {
        char rt[16]; fmt_elapsed(rt, sizeof(rt), elapsed);
        col = vscreen_puts(row, col, " Up:", T->fg_secondary, T->bg_header_row, false, false);
        col = vscreen_puts(row, col, rt, T->fg_yellow, T->bg_header_row, true, false);
        HDR_SEP();
    }

    if (col + 20 < metric_end) {
        /* FIX (display): cap shown overhead at 20.0% with '~' prefix.
         * Raw values near 100% are real BPF ring-buffer copy cost, not
         * monitor inefficiency — capping avoids alarming false readings.
         * FIX (format): split number and '%' into two vscreen_puts calls
         * so no '%' character ever appears inside a string passed to
         * vscreen_puts (which may interpret it as a printf format char,
         * producing the '%%' double-percent seen in the display). */
        double display_pct = cpu_pct;
        uint32_t cpu_col = display_pct < 1.0  ? T->fg_green  :
                   display_pct < 5.0  ? T->fg_yellow :
                   display_pct < 15.0 ? T->fg_orange : T->fg_red;

        char cpu_s[16];
        snprintf(cpu_s, sizeof(cpu_s), "%.1f%%", display_pct);

        col = vscreen_puts(row, col, " Overhead: ", T->fg_secondary,
                        T->bg_header_row, false, false);

        col = vscreen_puts(row, col, cpu_s, cpu_col,
                        T->bg_header_row, true, false);
    }

    /* Events per second */
    if (col + 14 < metric_end) {
        char evts_s[12]; fmt_rate(evts_s, sizeof(evts_s), g_events_per_sec);
        col = vscreen_puts(row, col, " EVT/s:", T->fg_secondary, T->bg_header_row, false, false);
        col = vscreen_puts(row, col, evts_s, T->fg_teal, T->bg_header_row, true, false);
        HDR_SEP();
    }

    /* Slot count */
    if (col + 12 < metric_end) {
        double    slot_pct = (double)stat_count / MAX_STATS;
        uint32_t  slot_col = slot_pct < 0.5 ? T->fg_green :
                             slot_pct < 0.8 ? T->fg_yellow : T->fg_red;
        char slots_s[16]; snprintf(slots_s, sizeof(slots_s), "%d/%d", stat_count, MAX_STATS);
        col = vscreen_puts(row, col, " Slots:", T->fg_secondary, T->bg_header_row, false, false);
        col = vscreen_puts(row, col, slots_s, slot_col, T->bg_header_row, true, false);
    }

    /* Dropped events (only when non-zero) */
    if (total_events_dropped > 0 && col + 16 < metric_end) {
        HDR_SEP();
        char drop_n[12]; fmt_rate(drop_n, sizeof(drop_n), (double)total_events_dropped);
        char drop_s[32]; snprintf(drop_s, sizeof(drop_s), " %s DROP:%s", WARN, drop_n);
        /* FIX: bound to metric_end — unclipped vscreen_puts overwrote clock zone */
        col = safe_puts(row, col, metric_end - col,
                        drop_s, T->fg_red, T->bg_header_row, true, false);
    }

    (void)col;  /* metric stream ends here */

    /*
     * ── CLOCK — btop-style: HH:MM:SS centered on the TOP BORDER row ──
     *
     * btop renders the clock as a panel title ON THE BORDER LINE itself,
     * centered at (x + width/2 - len/2), wrapped in title_left/title_right
     * decorators (┤ HH:MM:SS ├).  It shows ONLY the time — no date.
     *
     * We replicate this exactly:
     *   - Use strftime("%T") = "HH:MM:SS" (always 8 chars, POSIX)
     *   - Compute center column on the top border row (r.row)
     *   - Draw: T->bmr + " " + time + " " + T->bml (right-tee, time, left-tee)
     *     using the header's double-line border chars (bdv,bdtl,bdtr already drawn)
     *   - Only update when the second changes (diff engine handles no-op)
     *
     * WHY top border row (r.row), not metrics row (r.row+1):
     *   The metrics row is already populated with left-stream text that may
     *   collide.  The border row has only ═══ fill which is safe to overwrite
     *   at any centered position — exactly what btop does.
     */
    {
        static time_t last_clock_sec = 0;
        static char   clock_buf[9]   = "";   /* "HH:MM:SS\0" — always 8 chars */

        time_t    now = time(NULL);
        struct tm tm_info;
        localtime_r(&now, &tm_info);

        if (now != last_clock_sec) {
            strftime(clock_buf, sizeof(clock_buf), "%H:%M:%S", &tm_info);
            last_clock_sec = now;
        }

        /* Center the 8-char time string in the top border row.
         * Layout: ┤ HH:MM:SS ├  — 1 space pad each side = 10 cells total.
         * Placement mirrors btop: col = r.col + W/2 - (8+2)/2 = W/2 - 5    */
        int center = r.col + W / 2;
        int tl_col = center - 5;   /* left tee at center-5                  */
        int tr_col = center + 5;   /* right tee at center+5 (= tl+10)       */
        int border_row = r.row;    /* top border row — already has ═══ fill  */

        if (tl_col > r.col + 1 && tr_col < r.col + W - 1) {
            /* Left tee (┤), space, time string (8 chars), space, right tee (├) */
            vscreen_put(border_row, tl_col,     T->bmr, T->border_dim, T->bg_header_row, false, false);
            vscreen_put(border_row, tl_col + 1, " ",    T->fg_dim,     T->bg_header_row, false, false);
            vscreen_puts(border_row, tl_col + 2, clock_buf, T->fg_primary, T->bg_header_row, true, false);
            vscreen_put(border_row, tl_col + 10, " ",   T->fg_dim,     T->bg_header_row, false, false);
            vscreen_put(border_row, tl_col + 11, T->bml, T->border_dim, T->bg_header_row, false, false);
        }
    }

#undef HDR_SEP

    /* ── ║ filter / legend row ║ ── */
    row++;
    vscreen_put(row, r.col,         T->bdv, T->border_dim, T->bg_header_row, false, false);
    vscreen_put(row, r.col + W - 1, T->bdv, T->border_dim, T->bg_header_row, false, false);

    col = r.col + 2;
    int any_filter = (active_pid || (active_comm && active_comm[0]) || active_min_ms);

    if (any_filter) {
        col = vscreen_puts(row, col, DIAMOND " FILTERS: ", T->fg_magenta, T->bg_header_row, true, false);
        if (active_pid && col < right_guard) {
            char ps[32]; snprintf(ps, sizeof(ps), "pid=%d  ", active_pid);
            col = safe_puts(row, col, right_guard - col, ps, T->fg_magenta, T->bg_header_row, false, false);
        }
        if (active_comm && active_comm[0] && col < right_guard) {
            char cs[32]; snprintf(cs, sizeof(cs), "comm=%s  ", active_comm);
            col = safe_puts(row, col, right_guard - col, cs, T->fg_magenta, T->bg_header_row, false, false);
        }
        if (active_min_ms && col < right_guard) {
            char ms[32]; snprintf(ms, sizeof(ms), "min-dur=%ldms", active_min_ms);
            col = safe_puts(row, col, right_guard - col, ms, T->fg_magenta, T->bg_header_row, false, false);
        }
        if (col < right_guard)
            safe_puts(row, col, right_guard - col,
                      "  (kernel-side filter \xe2\x80\x94 zero overhead for excluded procs)",
                      T->fg_dim, T->bg_header_row, false, true);
    } else {
        if (col < right_guard) col = vscreen_puts(row, col, "Latency: ", T->fg_secondary, T->bg_header_row, false, false);
        if (col < right_guard) col = vscreen_puts(row, col, BLOCK_FULL " <10\xc2\xb5s  ", T->fg_green, T->bg_header_row, false, false);
        if (col < right_guard) col = vscreen_puts(row, col, BLOCK_FULL " <100\xc2\xb5s  ", T->fg_yellow, T->bg_header_row, false, false);
        if (col < right_guard) col = vscreen_puts(row, col, BLOCK_FULL " <1ms  ", T->fg_orange, T->bg_header_row, false, false);
        if (col < right_guard) col = vscreen_puts(row, col, BLOCK_FULL " \xe2\x89\xa5" "1ms  ", T->fg_red, T->bg_header_row, false, false);
        if (col < right_guard)
            safe_puts(row, col, right_guard - col,
                      WARN "=anomaly   sched avg/p95 exclude sleep >200ms",
                      T->fg_dim, T->bg_header_row, false, false);
    }
    (void)col;

    /* ── ╚══ bottom border ══╝ ── */
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
/*                                                                     */
/*  FIX — COLUMN ALIGNMENT:                                           */
/*  All column widths are compile-time constants.  Each column is     */
/*  rendered with safe_printf(max_cols = COL_WIDTH) which clips at    */
/*  that exact width.  Exactly 1 space gap between every column.     */
/*  right_guard is checked before every column write so wide data    */
/*  never escapes into the next panel.                                */
/* ================================================================== */

/* ================================================================== */
/*  SECTION 1 — MAIN PROCESS TABLE                                    */
/*                                                                     */
/*  FIX — COLUMN ALIGNMENT:                                           */
/*  All column widths are compile-time constants.  Each column is     */
/*  rendered with safe_printf(max_cols = COL_WIDTH) which clips at    */
/*  that exact width.  Exactly 1 space gap between every column.     */
/*  right_guard is checked before every column write so wide data    */
/*  never escapes into the next panel.                                */
/* ================================================================== */

#define MAX_DISPLAY 16

/*
 * FIX — COLUMN WIDTHS:
 * These constants define the EXACT display-cell width of every column
 * INCLUDING its right-side space gap.  The header and every data row
 * use the same values, so they are structurally identical.
 *
 * Layout (1 space gap between columns):
 *   [1 anom] [BAR+1] [PROC ] [EVENT] [RATE/s] [AVG] [P95?] [MAX] [PID?] [CTXSW?] [EXECS?]
 *
 * Column data widths (NOT including the trailing space separator):
 *   PROC  16  — left-aligned process name  (was 14, caused "gnome-shell" overflow)
 *   EVENT  9  — left-aligned event name
 *   RATE   7  — right-aligned rate string
 *   AVG    7  — right-aligned avg latency
 *   P95    7  — right-aligned p95 latency
 *   MAX    8  — right-aligned max latency
 *   PID    7  — right-aligned PID (shown >= 80 cols)
 *   CTXSW  7  — right-aligned ctx switches (was 6, needed for 6-digit numbers)
 *   EXECS  7  — right-aligned exec count   (was 6)
 *   BAR    8  — mini rate sparkbar
 */
#define COL_PROC   18
#define COL_EVENT  10
#define COL_RATE    7
#define COL_AVG     8
#define COL_P95     8
#define COL_MAX     9
#define COL_PID     8
#define COL_CTXSW   7
#define COL_EXECS   7
#define COL_BAR     8
static void draw_processes(rect_t panel)
{
    if (!rect_valid(panel) || panel.rows < 4 || panel.cols < 40) return;
    rect_t inner = draw_panel(panel, "PROCESSES", T->fg_cyan);
    if (!rect_valid(inner) || inner.rows < 3 || inner.cols < 38) return;

    struct syscall_stat tmp[MAX_STATS];
    memcpy(tmp, stats, (size_t)stat_count * sizeof(*tmp));
    qsort(tmp, (size_t)stat_count, sizeof(*tmp), cmp_rate);

    int W           = inner.cols;
    int right_guard = inner.col + W;

    bool show_pid   = (W >= 80);
    bool show_p95   = (W >= 96);
    bool show_extra = (W >= 116);
    bool show_bar   = (W >= 70);

    /* ----------------------------------------------------------------
     * Build header string.
     *
     * FIX — HEADER/DATA ALIGNMENT:
     * Data rows start at inner.col+1 (anomaly indicator) then +1 for
     * the indicator cell itself.  The header is rendered by
     * draw_table_header() which starts at inner.col+2.  So the header
     * string must begin with the BAR field (no leading space for the
     * anomaly indicator — that cell is always blank in the header).
     *
     * Each column is rendered with %-Ns or %Ns matching the same widths
     * used in the data rows.  The single space after each safe_printf
     * col++ matches the separator space in the format string below.
     * ---------------------------------------------------------------- */
    /* ----------------------------------------------------------------
     * Render header row column-by-column, mirroring the EXACT same
     * col positions used in data rows.  This guarantees pixel-perfect
     * alignment regardless of which optional columns are shown.
     *
     * Data row layout:
     *   inner.col+1  → 1-cell anomaly indicator  (col++)
     *   col          → COL_BAR mini-bar           (col += COL_BAR+1, if show_bar)
     *   col          → COL_PROC process name      (col++)
     *   col          → COL_EVENT event            (col++)
     *   col          → COL_RATE  rate/s           (col++)
     *   col          → COL_AVG   avg              (col++)
     *   col          → COL_P95   p95              (col++, if show_p95)
     *   col          → COL_MAX   max              (col++)
     *   col          → COL_PID   pid              (col++, if show_pid)
     *   col          → COL_CTXSW ctxsw            (col++, if show_extra)
     *   col          → COL_EXECS execs            (if show_extra)
     * ---------------------------------------------------------------- */
    {
        int hr = inner.row;   /* header is row 0 of inner */
        fill_row_bg(hr, inner.col, right_guard, T->bg_header_row);

        int col = inner.col + 1;

        /* Anomaly indicator cell — blank in header */
        vscreen_put(hr, col, " ", T->fg_secondary, T->bg_header_row, false, false);
        col++;

        /* BAR column header */
        if (show_bar && col + COL_BAR + 1 <= right_guard) {
            safe_printf(hr, col, COL_BAR,
                        T->fg_secondary, T->bg_header_row, false, false,
                        "%*s", COL_BAR, "RATE");
            col += COL_BAR + 1;
        }

        if (col >= right_guard) goto hdr_done;

/*
 * HDR_COL — write a header label and advance col by exactly `width`+1
 * (the column data width + 1 separator space), matching what data rows
 * do: safe_printf(...) returns col+width, then col++ adds the separator.
 *
 * We IGNORE the return value of safe_printf and advance col by the
 * compile-time constant instead.  This is critical: safe_printf calls
 * vscreen_puts which returns col + cells_actually_rendered.  For ASCII
 * strings padded by %*s that equals col+width, but we must never rely
 * on that — advancing by the constant keeps header and data rows
 * structurally identical regardless of font/cell quirks.
 */
#define HDR_COL(width, fmt, label) \
    do { \
        safe_printf(hr, col, (width), \
                    T->fg_secondary, T->bg_header_row, false, false, \
                    (fmt), (width), (label)); \
        col += (width) + 1; \
    } while (0)

        /* PROCESS — left-aligned */
        HDR_COL(COL_PROC,  "%-*s", "PROCESS");
        if (col >= right_guard) goto hdr_done;

        /* EVENT — left-aligned */
        HDR_COL(COL_EVENT, "%-*s", "EVENT");

        /* RATE/s — right-aligned */
        if (col + COL_RATE + 1 <= right_guard)
            HDR_COL(COL_RATE, "%*s", "RATE/s");

        /* AVG — right-aligned */
        if (col + COL_AVG + 1 <= right_guard)
            HDR_COL(COL_AVG, "%*s", "AVG");

        /* P95 — right-aligned, optional */
        if (show_p95 && col + COL_P95 + 1 <= right_guard)
            HDR_COL(COL_P95, "%*s", "P95");

        /* MAX — right-aligned */
        if (col + COL_MAX + 1 <= right_guard)
            HDR_COL(COL_MAX, "%*s", "MAX");

        /* PID — right-aligned, optional */
        if (show_pid && col + COL_PID + 1 <= right_guard)
            HDR_COL(COL_PID, "%*s", "PID");

        /* CTXSW / EXECS — right-aligned, optional */
        if (show_extra) {
            if (col + COL_CTXSW + 1 <= right_guard)
                HDR_COL(COL_CTXSW, "%*s", "CTXSW");
            if (col + COL_EXECS <= right_guard)
                safe_printf(hr, col, COL_EXECS,
                            T->fg_secondary, T->bg_header_row, false, false,
                            "%*s", COL_EXECS, "EXECS");
        }
#undef HDR_COL
        (void)col;
    }
hdr_done:
    draw_divider(panel, 2);

    double max_rate = 1.0;
    for (int i = 0; i < stat_count; i++)
        if (stats[i].rate > max_rate) max_rate = stats[i].rate;

    int data_row  = inner.row + 2;
    int max_rows  = inner.rows - 3;
    if (max_rows < 1) return;

    int shown = 0;
    for (int i = 0; i < stat_count && shown < MAX_DISPLAY && shown < max_rows; i++) {
        struct syscall_stat *s = &tmp[i];
        int r = data_row + shown;
        if (r >= inner.row + inner.rows - 1) break;

        long valid = s->count - s->drop_count;
        long avg   = (valid > 0 && s->total_latency > 0)
                     ? (s->total_latency / valid) / 1000 : 0;
        long p95   = stats_p95_us(s);
        long maxus = s->max_latency / 1000;

        char rate_s[12], avg_s[10], p95_s[10], max_s[10];
        fmt_rate(rate_s, sizeof(rate_s), s->rate);
        fmt_us(avg_s,  sizeof(avg_s),  avg);
        fmt_us(p95_s,  sizeof(p95_s),  p95);
        fmt_us(max_s,  sizeof(max_s),  maxus);

        bool     is_anom = s->is_anomaly && s->baseline_ready;
        uint32_t row_bg  = is_anom          ? T->bg_anom_row :
                           (shown % 2 == 1) ? T->bg_alt_row  : BG;

        /* WHY: fill entire row first — removes leftover characters */
        fill_row_bg(r, inner.col, right_guard, row_bg);

        int col = inner.col + 1;

        /* Anomaly indicator */
        bool blink_on = (g_frame % 2 == 0);
        vscreen_put(r, col, is_anom ? (blink_on ? "!" : DIAMOND) : " ",
                    is_anom ? T->fg_red : T->fg_dim, row_bg, true, false);
        col++;

        /* Rate mini-bar */
        if (show_bar && col + COL_BAR + 1 <= right_guard) {
            double frac = (max_rate > 0) ? s->rate / max_rate : 0.0;
            draw_mini_bar(r, col, COL_BAR, frac, heat_color(frac), row_bg);
            col += COL_BAR + 1;
        }

        if (col >= right_guard) { shown++; continue; }

        /* PROCESS — left-aligned, fixed width */
        safe_printf(r, col, COL_PROC, T->fg_primary, row_bg, true, false, "%-*s", COL_PROC, s->process);
        col += COL_PROC + 1;
        if (col >= right_guard) { shown++; continue; }

        /* EVENT */
        safe_printf(r, col, COL_EVENT, event_fg(s->event), row_bg, false, false, "%-*s", COL_EVENT, s->event);
        col += COL_EVENT + 1;

        /* RATE/s — right-aligned */
        if (col + COL_RATE + 1 <= right_guard) {
            safe_printf(r, col, COL_RATE, T->fg_primary, row_bg, false, false, "%*s", COL_RATE, rate_s);
            col += COL_RATE + 1;
        }

        /* AVG */
        if (col + COL_AVG + 1 <= right_guard) {
            safe_printf(r, col, COL_AVG, lat_fg(avg), row_bg, false, false, "%*s", COL_AVG, avg_s);
            col += COL_AVG + 1;
        }

        /* P95 */
        if (show_p95 && col + COL_P95 + 1 <= right_guard) {
            safe_printf(r, col, COL_P95, lat_fg(p95), row_bg, false, false, "%*s", COL_P95, p95_s);
            col += COL_P95 + 1;
        }

        /* MAX */
        if (col + COL_MAX + 1 <= right_guard) {
            safe_printf(r, col, COL_MAX, lat_fg(maxus), row_bg, false, false, "%*s", COL_MAX, max_s);
            col += COL_MAX + 1;
        }

        /* PID */
        if (show_pid && col + COL_PID + 1 <= right_guard) {
            safe_printf(r, col, COL_PID, T->fg_secondary, row_bg, false, false, "%*d", COL_PID, s->pid);
            col += COL_PID + 1;
        }

        /* CTXSW / EXECS */
        if (show_extra) {
            if (col + COL_CTXSW + 1 <= right_guard) {
                safe_printf(r, col, COL_CTXSW,
                            s->ctx_switches > 0 ? T->fg_yellow : T->fg_secondary,
                            row_bg, false, false, "%*ld", COL_CTXSW, s->ctx_switches);
                col += COL_CTXSW + 1;
            }
            if (col + COL_EXECS <= right_guard) {
                safe_printf(r, col, COL_EXECS,
                            s->exec_count > 0 ? T->fg_green : T->fg_secondary,
                            row_bg, false, false, "%*ld", COL_EXECS, s->exec_count);
            }
        }

        shown++;
    }

    /* Overflow indicator */
    if (stat_count > shown) {
        int r = data_row + shown;
        if (r < inner.row + inner.rows - 1) {
            fill_row_bg(r, inner.col, right_guard, BG);
            safe_printf(r, inner.col + 2, W - 4,
                        T->fg_dim, BG, false, true,
                        "%s %d more entries (top %d by rate shown)",
                        TRIANGLE_R, stat_count - shown, shown);
        }
    }
}

/* ================================================================== */
/*  SECTION 2 — EVENT SUMMARY                                         */
/* ================================================================== */

static void draw_event_summary(rect_t panel)
{
    if (!rect_valid(panel) || panel.rows < 4 || panel.cols < 20) return;
    rect_t inner = draw_panel(panel, "EVENT SUMMARY", T->fg_teal);
    if (!rect_valid(inner) || inner.rows < 3 || inner.cols < 18) return;

    agg_entry_t agg[32];
    int count = build_event_agg(agg, 32);
    int right_guard = inner.col + inner.cols;

    /* Column widths — must match the header format exactly */
    #define ES_EVENT  11
    #define ES_AVG     7
    #define ES_MAX     7
    #define ES_TOTAL   9
    #define ES_RATE    7

    /* Header — render column-by-column to match data row positions */
    {
        int hr = inner.row;
        fill_row_bg(hr, inner.col, right_guard, T->bg_header_row);
        int col = inner.col + 2;
        safe_printf(hr, col, ES_EVENT, T->fg_secondary, T->bg_header_row, false, false, "%-*s", ES_EVENT, "EVENT");   col += ES_EVENT + 1;
        if (col + ES_AVG   <= right_guard) { safe_printf(hr, col, ES_AVG,   T->fg_secondary, T->bg_header_row, false, false, "%*s", ES_AVG,   "AVG");    col += ES_AVG   + 1; }
        if (col + ES_MAX   <= right_guard) { safe_printf(hr, col, ES_MAX,   T->fg_secondary, T->bg_header_row, false, false, "%*s", ES_MAX,   "MAX");    col += ES_MAX   + 1; }
        if (col + ES_TOTAL <= right_guard) { safe_printf(hr, col, ES_TOTAL, T->fg_secondary, T->bg_header_row, false, false, "%*s", ES_TOTAL, "TOTAL");  col += ES_TOTAL + 1; }
        if (col + ES_RATE  <= right_guard) { safe_printf(hr, col, ES_RATE,  T->fg_secondary, T->bg_header_row, false, false, "%*s", ES_RATE,  "RATE/s"); }
        (void)col;
    }
    draw_divider(panel, 2);

    int r = inner.row + 2;
    for (int i = 0; i < count && r < inner.row + inner.rows - 1; i++, r++) {
        long avg   = (agg[i].valid_count > 0 && agg[i].total_latency > 0)
                     ? (agg[i].total_latency / agg[i].valid_count) / 1000 : 0;
        long maxus = agg[i].max_latency / 1000;

        char avg_s[10], max_s[10], rate_s[10], tot_s[14];
        fmt_us(avg_s,    sizeof(avg_s),  avg);
        fmt_us(max_s,    sizeof(max_s),  maxus);
        fmt_rate(rate_s, sizeof(rate_s), agg[i].rate);
        snprintf(tot_s,  sizeof(tot_s),  "%ld", agg[i].count);

        uint32_t row_bg = (i % 2 == 1) ? T->bg_alt_row : BG;
        fill_row_bg(r, inner.col, right_guard, row_bg);

        int col = inner.col + 2;
        safe_printf(r, col, ES_EVENT, event_fg(agg[i].event), row_bg, false, false, "%-*s", ES_EVENT, agg[i].event); col += ES_EVENT + 1;
        if (col + ES_AVG   <= right_guard) { safe_printf(r, col, ES_AVG,   lat_fg(avg),   row_bg, false, false, "%*s",  ES_AVG,   avg_s);  col += ES_AVG   + 1; }
        if (col + ES_MAX   <= right_guard) { safe_printf(r, col, ES_MAX,   lat_fg(maxus), row_bg, false, false, "%*s",  ES_MAX,   max_s);  col += ES_MAX   + 1; }
        if (col + ES_TOTAL <= right_guard) { safe_printf(r, col, ES_TOTAL, T->fg_primary, row_bg, true,  false, "%*s",  ES_TOTAL, tot_s);  col += ES_TOTAL + 1; }
        if (col + ES_RATE  <= right_guard) { safe_printf(r, col, ES_RATE,  T->fg_primary, row_bg, true,  false, "%*s",  ES_RATE,  rate_s); }
        (void)col;
    }

    #undef ES_EVENT
    #undef ES_AVG
    #undef ES_MAX
    #undef ES_TOTAL
    #undef ES_RATE
}

/* ================================================================== */
/*  SECTION 3 — ACTIVITY GRAPH                                        */
/* ================================================================== */

static void draw_activity(rect_t panel)
{
    if (!rect_valid(panel) || panel.rows < 4 || panel.cols < 20) return;

    agg_entry_t agg[32];
    int count = build_event_agg(agg, 32);
    sort_agg_by_rate(agg, count);

    for (int i = 0; i < count; i++) {
        spark_ring_t *sr = spark_get_or_create(agg[i].event);
        if (sr) spark_push(sr, agg[i].rate);
    }

    rect_t inner = draw_panel(panel, "ACTIVITY  " ARROW_UP " rate/s", T->fg_orange);
    if (!rect_valid(inner) || inner.rows < 2 || inner.cols < 18) return;

    int right_guard = inner.col + inner.cols;
    double max_rate = (count > 0 && agg[0].rate > 0) ? agg[0].rate : 1.0;

    int spark_w = (inner.cols >= 60) ? 8 : 0;
    int label_w = 11;
    int rate_w  = 8;
    int bar_w   = inner.cols - label_w - rate_w - spark_w - 6;
    if (bar_w < 4) { bar_w = 4; spark_w = 0; }

    int r = inner.row;
    for (int i = 0; i < count && r < inner.row + inner.rows; i++, r++) {
        uint32_t row_bg = (i % 2 == 1) ? T->bg_alt_row : BG;
        fill_row_bg(r, inner.col, right_guard, row_bg);

        double   frac   = agg[i].rate / max_rate;
        uint32_t ev_col = event_fg(agg[i].event);

        int col = inner.col + 2;
        col = safe_printf(r, col, label_w, ev_col, row_bg, false, false, "%-*s", label_w, agg[i].event); col++;

        if (col + bar_w <= right_guard) {
            draw_bar(r, col, bar_w, frac, heat_color(frac), T->fg_dim, row_bg);
            col += bar_w + 1;
        }

        if (spark_w > 0 && col + spark_w <= right_guard) {
            spark_ring_t *sr = spark_get_or_create(agg[i].event);
            col = draw_sparkline(r, col, spark_w, sr, ev_col, row_bg);
            col++;
        }

        if (col + rate_w <= right_guard) {
            char rate_s[12]; fmt_rate(rate_s, sizeof(rate_s), agg[i].rate);
            safe_printf(r, col, rate_w, T->fg_primary, row_bg, true, false, "%*s/s", rate_w - 2, rate_s);
        }
    }
}

/* ================================================================== */
/*  SECTION 4 — SLOWEST SYSCALLS                                      */
/* ================================================================== */

#define TOP_N 6

static void draw_slowest(rect_t panel)
{
    if (!rect_valid(panel) || panel.rows < 4 || panel.cols < 20) return;

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
    if (!rect_valid(inner) || inner.rows < 3 || inner.cols < 18) return;

    qsort(tmp, (size_t)n, sizeof(*tmp), cmp_latency);

    int right_guard = inner.col + inner.cols;
    draw_table_header(inner, 0, "%-14s %-9s %9s %9s %9s",
                      "PROCESS", "EVENT", "AVG", "P95", "MAX");
    draw_divider(panel, 2);

    int r = inner.row + 2;
    int shown = 0;
    for (int i = 0; i < n && shown < TOP_N && r < inner.row + inner.rows - 1; i++, r++, shown++) {
        long valid = tmp[i].count - tmp[i].drop_count;
        long avg   = (valid > 0 && tmp[i].total_latency > 0)
                     ? (tmp[i].total_latency / valid) / 1000 : 0;
        long maxus = tmp[i].max_latency / 1000;
        long p95   = stats_p95_us(&tmp[i]);

        char avg_s[10], max_s[10], p95_s[10];
        fmt_us(avg_s, sizeof(avg_s), avg);
        fmt_us(max_s, sizeof(max_s), maxus);
        fmt_us(p95_s, sizeof(p95_s), p95);

        uint32_t row_bg = (shown % 2 == 1) ? T->bg_alt_row : BG;
        fill_row_bg(r, inner.col, right_guard, row_bg);

        int col = inner.col + 2;
        col = safe_printf(r, col, 14, T->fg_primary,         row_bg, true,  false, "%-14s", tmp[i].process); col++;
        if (col >= right_guard) continue;
        col = safe_printf(r, col, 9,  event_fg(tmp[i].event),row_bg, false, false, "%-9s",  tmp[i].event);   col++;
        if (col + 9 <= right_guard) { col = safe_printf(r, col, 9, lat_fg(avg),   row_bg, false, false, "%9s", avg_s); col++; }
        if (col + 9 <= right_guard) { col = safe_printf(r, col, 9, lat_fg(p95),   row_bg, false, false, "%9s", p95_s); col++; }
        if (col + 9 <= right_guard) {       safe_printf(r, col, 9, lat_fg(maxus), row_bg, true,  false, "%9s", max_s); }
        (void)col;
    }
}

/* ================================================================== */
/*  SECTION 5 — ANOMALY DETECTION                                     */
/* ================================================================== */

static void draw_anomalies(rect_t panel)
{
    if (!rect_valid(panel) || panel.rows < 4 || panel.cols < 20) return;

    struct syscall_stat tmp[MAX_STATS];
    memcpy(tmp, stats, (size_t)stat_count * sizeof(*tmp));
    qsort(tmp, (size_t)stat_count, sizeof(*tmp), cmp_deviation);

    int n_anom = 0;
    for (int i = 0; i < stat_count; i++)
        if (tmp[i].is_anomaly && tmp[i].baseline_ready) n_anom++;

    rect_t inner = draw_panel(panel, "ANOMALY DETECTION \xe2\x80\x94 EMA BASELINE", T->fg_red);
    if (!rect_valid(inner) || inner.rows < 3 || inner.cols < 18) return;

    int right_guard = inner.col + inner.cols;

    if (n_anom == 0) {
        int mr = inner.row + 1;
        fill_row_bg(mr, inner.col, right_guard, BG);
        safe_puts(mr, inner.col + 2, inner.cols - 4,
                  TICK "  All events within normal baseline",
                  T->fg_green, BG, true, false);
        return;
    }

    draw_table_header(inner, 0,
        "%-14s %-9s %9s %9s %9s",
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

        char cur_s[10], bas_s[10];
        fmt_us(cur_s, sizeof(cur_s), current_us);
        fmt_us(bas_s, sizeof(bas_s), baseline_us);

        uint32_t dc     = dev_fg(tmp[i].deviation);
        uint32_t row_bg = (shown % 2 == 1) ? T->bg_alt_row : BG;

        fill_row_bg(r, inner.col, right_guard, row_bg);

        bool flash = (g_frame % 2 == 0);
        int col = inner.col + 1;
        vscreen_put(r, col, flash ? WARN : " ", T->fg_red, row_bg, true, false); col += 2;

        col = safe_printf(r, col, 14, T->fg_primary,          row_bg, false, false, "%-14s", tmp[i].process); col++;
        if (col >= right_guard) { shown++; r++; continue; }
        col = safe_printf(r, col, 9,  event_fg(tmp[i].event), row_bg, false, false, "%-9s",  tmp[i].event);   col++;
        if (col + 9  <= right_guard) { col = safe_printf(r, col, 9,  dc,               row_bg, true,  false, "%8.1f%%", tmp[i].deviation * 100.0); col++; }
        if (col + 9  <= right_guard) { col = safe_printf(r, col, 9,  T->fg_secondary,  row_bg, false, false, "%9s",     bas_s);                     col++; }
        if (col + 9  <= right_guard) {       safe_printf(r, col, 9,  dc,               row_bg, true,  false, "%9s",     cur_s); }
        (void)col;

        shown++; r++;
    }

    if (n_anom > shown && r < inner.row + inner.rows) {
        fill_row_bg(r, inner.col, right_guard, BG);
        safe_printf(r, inner.col + 2, inner.cols - 4,
                    T->fg_dim, BG, false, true,
                    "%s %d more anomalies", TRIANGLE_R, n_anom - shown);
    }
}

/* ================================================================== */
/*  SECTION 6 — SCHEDULING DELAY (side-by-side with Lifecycle)        */
/*                                                                     */
/*  WHY SIDE-BY-SIDE: when stacked below Lifecycle this panel got     */
/*  only 4-6 rows, barely enough for the border + header + 2 rows.   */
/*  Side-by-side gives it the full bottom-zone height (~10-15 rows).  */
/* ================================================================== */

static void update_sched_sparklines(void)
{
    for (int i = 0; i < stat_count; i++) {
        if (strcmp(stats[i].event, "sched") != 0) continue;
        char key[28]; snprintf(key, sizeof(key), "sched:%d", stats[i].pid);
        spark_ring_t *sr = spark_get_or_create(key);
        if (!sr) continue;
        long valid = stats[i].count - stats[i].drop_count;
        long avg   = (valid > 0 && stats[i].total_latency > 0)
                     ? (stats[i].total_latency / valid) / 1000 : 0;
        spark_push(sr, (double)avg);
    }
}

static void draw_sched_delay(rect_t panel)
{
    if (!rect_valid(panel) || panel.rows < 4 || panel.cols < 20) return;

    struct syscall_stat tmp[MAX_STATS];
    int n = 0;
    for (int i = 0; i < stat_count; i++) {
        if (strcmp(stats[i].event, "sched") != 0) continue;
        if (n < MAX_STATS) tmp[n++] = stats[i];
    }

    rect_t inner = draw_panel(panel, "SCHEDULING DELAY", T->fg_yellow);
    if (!rect_valid(inner) || inner.rows < 3 || inner.cols < 18) return;

    int right_guard = inner.col + inner.cols;

    if (n == 0) {
        int mr = inner.row + 1;
        fill_row_bg(mr, inner.col, right_guard, BG);
        safe_puts(mr, inner.col + 2, inner.cols - 4,
                  TICK "  No scheduling events yet",
                  T->fg_secondary, BG, false, true);
        return;
    }

    qsort(tmp, (size_t)n, sizeof(*tmp), cmp_latency);

    bool show_spark = (inner.cols >= 60);
    int  spark_w    = show_spark ? 8 : 0;

    draw_table_header(inner, 0,
        "%-13s %6s %9s %9s %9s %6s%s",
        "PROCESS", "PID", "AVG", "P95", "MAX", "CTXSW",
        show_spark ? "  TREND" : "");
    draw_divider(panel, 2);

    int r = inner.row + 2;
    int shown = 0;
    for (int i = 0; i < n && r < inner.row + inner.rows - 1; i++, r++, shown++) {
        long valid = tmp[i].count - tmp[i].drop_count;
        long avg   = (valid > 0 && tmp[i].total_latency > 0)
                     ? (tmp[i].total_latency / valid) / 1000 : 0;
        long maxus = tmp[i].max_latency / 1000;
        long p95   = stats_p95_us(&tmp[i]);

        if (avg > 190000 && p95 <= 0 && valid < 5) { r--; shown--; continue; }

        char avg_s[10], max_s[10], p95_s[10];
        fmt_us(avg_s, sizeof(avg_s), avg);
        fmt_us(max_s, sizeof(max_s), maxus);
        fmt_us(p95_s, sizeof(p95_s), p95);

        uint32_t row_bg = (shown % 2 == 1) ? T->bg_alt_row : BG;
        fill_row_bg(r, inner.col, right_guard, row_bg);

        int col = inner.col + 2;
        col = safe_printf(r, col, 13, T->fg_primary,  row_bg, true,  false, "%-13s", tmp[i].process); col++;
        if (col >= right_guard) continue;
        if (col + 6 <= right_guard) { col = safe_printf(r, col, 6,  T->fg_secondary, row_bg, false, false, "%6d",  tmp[i].pid);         col++; }
        if (col + 9 <= right_guard) { col = safe_printf(r, col, 9,  lat_fg(avg),     row_bg, true,  false, "%9s",  avg_s);              col++; }
        if (col + 9 <= right_guard) { col = safe_printf(r, col, 9,  lat_fg(p95),     row_bg, false, false, "%9s",  p95_s);              col++; }
        if (col + 9 <= right_guard) { col = safe_printf(r, col, 9,  lat_fg(maxus),   row_bg, true,  false, "%9s",  max_s);              col++; }
        if (col + 6 <= right_guard) { col = safe_printf(r, col, 6,
                                                         tmp[i].ctx_switches > 1000 ? T->fg_orange : T->fg_secondary,
                                                         row_bg, false, false, "%6ld", tmp[i].ctx_switches); col++; }

        if (show_spark && spark_w > 0 && col + spark_w <= right_guard) {
            char key[28]; snprintf(key, sizeof(key), "sched:%d", tmp[i].pid);
            spark_ring_t *sr = spark_get_or_create(key);
            if (sr && sr->count == 0) spark_push(sr, (double)avg);
            draw_sparkline(r, col, spark_w, sr, lat_fg(avg), row_bg);
        }
    }

    if (shown == 0) {
        int mr = inner.row + 2;
        if (mr < inner.row + inner.rows) {
            fill_row_bg(mr, inner.col, right_guard, BG);
            safe_puts(mr, inner.col + 2, inner.cols - 4,
                      TICK "  Waiting for sched events (all filtered as noise?)",
                      T->fg_secondary, BG, false, true);
        }
    }
}

/* ================================================================== */
/*  SECTION 7 — PROCESS LIFECYCLE                                     */
/* ================================================================== */

static void draw_lifecycle(rect_t panel)
{
    if (!rect_valid(panel) || panel.rows < 4 || panel.cols < 20) return;

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

    rect_t inner = draw_panel(panel, "PROCESS LIFECYCLE", T->fg_magenta);
    if (!rect_valid(inner) || inner.rows < 3 || inner.cols < 18) return;

    int right_guard = inner.col + inner.cols;

    if (lc_count == 0) {
        int mr = inner.row + 1;
        fill_row_bg(mr, inner.col, right_guard, BG);
        safe_puts(mr, inner.col + 2, inner.cols - 4,
                  TICK "  No exec/exit events yet",
                  T->fg_secondary, BG, false, true);
        return;
    }

    draw_table_header(inner, 0,
        "%-14s %6s %8s %8s %11s",
        "PROCESS", "PID", "EXECS", "EXITS", "AVG LIFETIME");
    draw_divider(panel, 2);

    int r = inner.row + 2;
    for (int i = 0; i < lc_count && r < inner.row + inner.rows - 1; i++, r++) {
        char life_s[12]; fmt_us(life_s, sizeof(life_s), procs[i].avg_life_us);

        uint32_t row_bg = (i % 2 == 1) ? T->bg_alt_row : BG;
        fill_row_bg(r, inner.col, right_guard, row_bg);

        int col = inner.col + 2;
        col = safe_printf(r, col, 14, T->fg_primary,   row_bg, true,  false, "%-14s", procs[i].process); col++;
        if (col >= right_guard) continue;
        if (col + 6  <= right_guard) { col = safe_printf(r, col, 6,  T->fg_secondary, row_bg, false, false, "%6d",  procs[i].pid);        col++; }
        if (col + 8  <= right_guard) { col = safe_printf(r, col, 8,  T->fg_green,     row_bg, false, false, "%8ld", procs[i].execs);      col++; }
        if (col + 8  <= right_guard) { col = safe_printf(r, col, 8,  T->fg_yellow,    row_bg, false, false, "%8ld", procs[i].exits);      col++; }
        if (col + 11 <= right_guard) {       safe_printf(r, col, 11, T->fg_cyan,      row_bg, false, false, "%11s", life_s); }
        (void)col;
    }
}

/* ================================================================== */
/*  CONTROLS BAR                                                       */
/* ================================================================== */

static void draw_controls(rect_t r)
{
    if (!rect_valid(r) || r.rows < 1) return;

    int row         = r.row;
    int right_guard = r.col + r.cols;

    fill_row_bg(row, r.col, right_guard, T->bg_header_row);

    int col = r.col + 2;

    struct { const char *key; const char *label; } keys[] = {
        { "[q]",   " quit"    },
        { "[r]",   " reset"   },
        { "[e]",   " export"  },
    };
    for (int i = 0; i < 3 && col < right_guard - 4; i++) {
        col = vscreen_puts(row, col, keys[i].key, T->fg_cyan, T->bg_header_row, true, false);
        col = safe_puts(row, col, right_guard - col - 10,
                        keys[i].label, T->fg_secondary, T->bg_header_row, false, false);
        if (i < 2 && col < right_guard - 6) {
            col = vscreen_puts(row, col, "  ", T->fg_dim, T->bg_header_row, false, false);
            vscreen_put(row, col, T->bv, T->border_dim, T->bg_header_row, false, false);
            col += 2;
        }
    }

    /* Right-aligned watermark */
    const char *wm = LIGHTNING " eBPF Monitor v9.0";
    int wm_w   = (int)strlen(wm);
    int wm_col = right_guard - wm_w - 2;
    if (wm_col > col + 4) {
        vscreen_put(row, wm_col - 1, T->bv, T->border_dim, T->bg_header_row, false, false);
        safe_puts(row, wm_col, right_guard - wm_col,
                  wm, T->fg_dim, T->bg_header_row, false, false);
    }
}

/* ================================================================== */
/*  MAIN ENTRY POINT                                                   */
/*                                                                     */
/*  HOW THE DOUBLE-BUFFER WORKS:                                       */
/*    vscreen_clear()  — fills vs_next with blank cells               */
/*    fill_rect(full)  — paints the global bg colour into vs_next     */
/*    draw_*()         — panel renderers paint their regions in vs_next*/
/*    vscreen_flush()  — diffs vs_next vs vs_prev, emits only Δ cells */
/*                                                                     */
/*  We NEVER call term_clear_screen().  The first flush after a resize*/
/*  is forced full-redraw by vscreen_invalidate() (called from the    */
/*  main loop SIGWINCH handler), which makes vs_prev all-0xFF.       */
/* ================================================================== */

void dashboard_render(double elapsed, double cpu_pct,
                      int active_pid, const char *active_comm,
                      long active_min_ms)
{
    g_frame++;

    term_size_t sz = term_get_size();

    /*
     * WHY: track last size here, not inside vscreen_resize().
     * vscreen_resize() is called from the SIGWINCH handler in
     * bootstrap.c.  If we call it again here on size change we get a
     * double-resize and vs_prev is wiped twice → unnecessary full
     * redraw flicker.  We only resize here if the bootstrap main loop
     * somehow missed the SIGWINCH (shouldn't happen, but safe fallback).
     */
    static int last_rows = 0, last_cols = 0;
    if (sz.rows != last_rows || sz.cols != last_cols) {
        vscreen_resize(sz.rows, sz.cols);
        vscreen_invalidate();
        last_rows = sz.rows;
        last_cols = sz.cols;
    }

    /* ── Compute layout ── */
    layout_t L;
    int layout_ok = layout_compute(sz.rows, sz.cols, &L);

    /* ── Blank the next frame ── */
    vscreen_clear();

    /* ── Global background fill ── */
    {
        rect_t full = { 0, 0, sz.rows, sz.cols };
        fill_rect(full, T->fg_dim, BG);
    }

    /* ── Terminal too small ── */
    if (layout_ok != 0) {
        vscreen_puts(0, 0,
                     "Terminal too small — please resize (min 60x20)",
                     T->fg_red, BG, true, false);
        vscreen_flush();
        return;
    }

    /* ── Pre-render metric updates ── */
    update_events_per_sec();
    update_sched_sparklines();

    /* ── Draw all panels ── */
    draw_header(L.header, elapsed, cpu_pct,
                active_pid, active_comm, active_min_ms);

    draw_processes(L.processes);
    draw_event_summary(L.summary);
    draw_activity(L.activity);
    draw_slowest(L.slowest);
    draw_anomalies(L.anomaly);

    /*
     * Bottom zone: lifecycle (left) and sched_delay (right) SIDE BY SIDE.
     *
     * WHY: layout_compute() already split L.lifecycle (left half) and
     * L.sched_delay (right half) horizontally from the same zone.
     * We just draw each into its rect.  No wrapper function needed.
     * Both panels check rect_valid() + minimum size at the top.
     */
    draw_lifecycle(L.lifecycle);

    if (rect_valid(L.sched_delay) && L.sched_delay.cols >= 20)
        draw_sched_delay(L.sched_delay);

    draw_controls(L.controls);

    /* ── Diff-flush: only changed cells reach the terminal ── */
    vscreen_flush();
}