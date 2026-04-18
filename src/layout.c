/* SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause) */

/*
 * layout.c — crash-proof panel geometry engine
 *
 * KEY DESIGN DECISIONS (each prevents a specific class of bugs):
 *
 * 1. MINIMUM SIZE GUARD
 *    layout_compute() checks for MIN_TERM_ROWS / MIN_TERM_COLS first.
 *    When the terminal is too small every rect is zero-sized and every
 *    draw_* function no-ops because they all check rows >= N before
 *    drawing.  No segfault, no partial draw.
 *
 * 2. ALL SPLITS USE layout_vsplit_fixed / layout_hsplit_fixed
 *    These clamp top_rows / left_cols to [0, src.rows/src.cols], so
 *    it is IMPOSSIBLE to produce a rect with negative rows or cols.
 *    Without clamping, a small terminal causes (src.rows - top_rows)
 *    to underflow and the render loop runs for 2^32 iterations → segfault.
 *
 * 3. PROGRESSIVE REMAINDER TRACKING
 *    Each allocation consumes from `rest` and updates it.  The final
 *    panel (bottom zone) gets EXACTLY what is left — no rounding error
 *    leaves a blank stripe at the bottom.
 *
 * 4. SIDE-BY-SIDE BOTTOM PANELS
 *    Lifecycle and Scheduling Delay are split HORIZONTALLY inside the
 *    bottom zone, not stacked vertically.  Vertical stacking gave each
 *    panel only 4-6 rows which is too short to show useful data.
 *    With a horizontal split both panels share the full height.
 *    Fallback: if the bottom zone is narrower than 80 cols the right
 *    panel collapses (sched_delay.cols == 0) and lifecycle gets all the
 *    width — safe because draw_sched_delay checks cols >= 20.
 *
 * 5. MINIMUM ROW ENFORCEMENT PER PANEL
 *    If enforcing a minimum would over-commit available rows the minimum
 *    is scaled down to what is available.  The code never subtracts more
 *    rows than are present.
 */

#include "layout.h"

/* Convenience: larger of two ints */
static inline int imax(int a, int b) { return a > b ? a : b; }
/* Convenience: smaller of two ints */
static inline int imin(int a, int b) { return a < b ? a : b; }

/*
 * layout_compute — compute all panel rects for a given terminal size.
 *
 * Panel stack (top → bottom):
 *
 *  ┌─ header        (4 rows, fixed) ────────────────────────────────┐
 *  ├─ processes     (30% of body, min 7) ───────────────────────────┤
 *  ├─ summary (50%) │ activity (50%)  (35% of remaining, min 7) ────┤
 *  ├─ slowest (50%) │ anomaly  (50%)  (40% of remaining, min 7) ────┤
 *  ├─ lifecycle (50%) │ sched_delay (50%)  (ALL remaining)  ─────────┤
 *  └─ controls      (1 row, fixed) ─────────────────────────────────┘
 *
 * Returns  0 on success.
 * Returns -1 when terminal is too small (all rects zeroed, nothing drawn).
 */
int layout_compute(int term_rows, int term_cols, layout_t *L)
{
    /* Zero everything — safe default if we return early */
    *L = (layout_t){0};

    /* ── Guard: terminal too small to draw anything useful ── */
    if (term_rows < MIN_TERM_ROWS || term_cols < MIN_TERM_COLS)
        return -1;

    rect_t screen = { 0, 0, term_rows, term_cols };

    /* ── Controls: peel 1 row off the very bottom ── */
    rect_t body, ctrl;
    layout_vsplit_fixed(screen, term_rows - 1, &body, &ctrl);
    L->controls = ctrl;

    /* ── Header: fixed 4 rows from the top of body ── */
    rect_t after_hdr;
    {
        int hdr_rows = imin(4, body.rows);
        layout_vsplit_fixed(body, hdr_rows, &L->header, &after_hdr);
    }
    int avail = after_hdr.rows;

    /* ── Processes: 30% of body, min 7, hard cap at avail ──
     *
     * WHY: Without a min, a 25-row terminal gives only 5 rows to
     * processes — not enough for the header + 1 data row.  Without a
     * hard cap, rounding can exceed avail → negative remainder → crash.
     */
    int proc_rows = imax(7, (avail * 30) / 100);
    proc_rows     = imin(proc_rows, avail);

    rect_t after_proc;
    layout_vsplit_fixed(after_hdr, proc_rows, &L->processes, &after_proc);
    avail = after_proc.rows;

    /* ── Mid band (summary | activity): 35% of remaining, min 7 ── */
    int mid_rows = imax(7, (avail * 35) / 100);
    mid_rows     = imin(mid_rows, avail);

    rect_t mid_band, after_mid;
    layout_vsplit_fixed(after_proc, mid_rows, &mid_band, &after_mid);
    /* 50/50 horizontal split — WHY: equal prominence for both panels */
    layout_hsplit(mid_band, 0.5f, &L->summary, &L->activity);
    avail = after_mid.rows;

    /* ── Lower band (slowest | anomaly): 40% of remaining, min 7 ── */
    int low_rows = imax(7, (avail * 40) / 100);
    low_rows     = imin(low_rows, avail);

    rect_t low_band, after_low;
    layout_vsplit_fixed(after_mid, low_rows, &low_band, &after_low);
    layout_hsplit(low_band, 0.5f, &L->slowest, &L->anomaly);

    /* ── Bottom zone: lifecycle | sched_delay SIDE BY SIDE ──
     *
     * WHY SIDE-BY-SIDE instead of stacked:
     *   Stacking gave each panel ~4 rows = 1 header + 1 divider + 2 data.
     *   With two data rows neither panel can show useful information.
     *   Side-by-side gives both panels the FULL height of the zone.
     *
     * Fallback when too narrow (< 80 cols):
     *   sched_delay collapses to zero width; lifecycle fills all cols.
     *   draw_sched_delay() guards on cols >= 20 so nothing crashes.
     *
     * WHY after_low absorbs ALL remaining rows:
     *   No percentage calculation → no rounding loss.  The last panel
     *   always fills to the exact bottom of the terminal.
     */
    rect_t bottom_zone = after_low;   /* every row left after lower band */

    if (bottom_zone.cols >= 80) {
        /* Wide enough: split 50/50.  Both panels get the full height. */
        layout_hsplit(bottom_zone, 0.5f, &L->lifecycle, &L->sched_delay);
    } else {
        /* Narrow: lifecycle takes everything, sched_delay is empty. */
        L->lifecycle   = bottom_zone;
        L->sched_delay = (rect_t){ bottom_zone.row, bottom_zone.col,
                                   bottom_zone.rows, 0 };
    }

    return 0;
}