/* SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause) */

#include "layout.h"
#include "vscreen.h"

#define MAX(a,b) ((a)>(b)?(a):(b))

/*
 * Panel layout (top to bottom):
 *
 *  ┌─ header (4 rows) ─────────────────────────────────────────────┐
 *  ├─ processes  (30% of body, min 7) ─────────────────────────────┤
 *  ├─ summary (~30% of remaining) │  activity (~30%) ──────────────┤
 *  ├─ slowest  (~35% of remaining) │  anomaly  (~35%) ──────────────┤
 *  ├─ lifecycle (absorbs ALL remaining rows — zero waste) ──────────┤
 *  └─ controls (1 row) ────────────────────────────────────────────┘
 *
 * Key principle: the LAST panel always absorbs leftover rows so the
 * terminal is 100% filled with no blank gap at the bottom.
 */
void layout_compute(int term_rows, int term_cols, layout_t *L)
{
    rect_t screen = { 0, 0, term_rows, term_cols };

    /* Header: fixed 4 rows */
    rect_t rest;
    layout_vsplit_fixed(screen, 4, &L->header, &rest);

    /* Controls: fixed 1 row at the very bottom */
    rect_t body;
    layout_vsplit_fixed(rest,
                        rest.rows > 1 ? rest.rows - 1 : rest.rows,
                        &body,
                        &L->controls);

    int avail = body.rows;

    /* ── Processes: 30% of body, min 7 rows ── */
    int proc_rows = MAX(7, (avail * 30) / 100);
    if (proc_rows > avail) proc_rows = avail;

    rect_t after_proc;
    layout_vsplit_fixed(body, proc_rows, &L->processes, &after_proc);
    avail = after_proc.rows;

    /* ── Middle band (summary | activity): 35% of remaining, min 7 ── */
    int mid_rows = MAX(7, (avail * 35) / 100);
    if (mid_rows > avail) mid_rows = avail;

    rect_t mid_band, after_mid;
    layout_vsplit_fixed(after_proc, mid_rows, &mid_band, &after_mid);
    layout_hsplit(mid_band, 0.5f, &L->summary, &L->activity);
    avail = after_mid.rows;

    /* ── Lower band (slowest | anomaly): 40% of remaining, min 7 ── */
    int low_rows = MAX(7, (avail * 40) / 100);
    if (low_rows > avail) low_rows = avail;

    rect_t low_band, after_low;
    layout_vsplit_fixed(after_mid, low_rows, &low_band, &after_low);
    layout_hsplit(low_band, 0.5f, &L->slowest, &L->anomaly);

    /*
     * ── Lifecycle: absorb EVERY remaining row ──
     * No percentage, no minimum check that discards rows.
     * after_low has exactly the rows that are left — use them all.
     * dashboard_render still guards with rows >= 4 before drawing.
     */
    L->lifecycle = after_low;
}
