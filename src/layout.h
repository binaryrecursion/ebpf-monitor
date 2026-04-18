#ifndef LAYOUT_H
#define LAYOUT_H

/* ------------------------------------------------------------------ */
/* Layout engine — safe, crash-proof panel grid                        */
/*                                                                     */
/* WHY SAFE: Every split clamps to [0, src.rows/src.cols] so that     */
/* even a 1-row terminal cannot produce a rect with negative rows.    */
/* layout_compute() enforces a minimum terminal size and returns a    */
/* degenerate layout (all panels collapsed) when there isn't enough   */
/* space.  All callers guard with rows >= N before drawing.           */
/* ------------------------------------------------------------------ */

/* Minimum terminal dimensions the dashboard requires.
   Below this the render functions simply no-op. */
#define MIN_TERM_ROWS 20
#define MIN_TERM_COLS 60

typedef struct {
    int row;   /* top-left origin, 0-based */
    int col;
    int rows;  /* height in cells (>= 0)   */
    int cols;  /* width  in cells (>= 0)   */
} rect_t;

/* ------------------------------------------------------------------
 * rect_t invariant helpers
 * ------------------------------------------------------------------ */

/* Return true if rect is drawable (non-zero area, positive dims). */
static inline int rect_valid(rect_t r)
{
    return (r.rows > 0 && r.cols > 0);
}

/* Clamp an integer into [lo, hi] (lo <= hi assumed). */
static inline int clamp_i(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ------------------------------------------------------------------
 * layout_vsplit_fixed
 *
 * Split `src` top/bottom at `top_rows` rows.
 * `a` receives the top portion, `b` the remainder.
 *
 * WHY SAFE: top_rows is clamped to [0, src.rows] so `b` never has
 * negative rows even when src.rows == 0.
 * ------------------------------------------------------------------ */
static inline void layout_vsplit_fixed(rect_t src, int top_rows,
                                       rect_t *a, rect_t *b)
{
    if (top_rows < 0)        top_rows = 0;
    if (top_rows > src.rows) top_rows = src.rows;
    *a = (rect_t){ src.row,            src.col, top_rows,             src.cols };
    *b = (rect_t){ src.row + top_rows, src.col, src.rows - top_rows,  src.cols };
}

/* Split vertically by fraction [0.0, 1.0] of parent height.
   WHY SAFE: fractional height is clamped before calling vsplit_fixed. */
static inline void layout_vsplit(rect_t src, float frac,
                                 rect_t *a, rect_t *b)
{
    int h = (int)((float)src.rows * frac);
    h = clamp_i(h, 0, src.rows);
    layout_vsplit_fixed(src, h, a, b);
}

/* ------------------------------------------------------------------
 * layout_hsplit_fixed
 *
 * Split `src` left/right at `left_cols` columns.
 * WHY SAFE: left_cols clamped to [0, src.cols].
 * ------------------------------------------------------------------ */
static inline void layout_hsplit_fixed(rect_t src, int left_cols,
                                       rect_t *a, rect_t *b)
{
    if (left_cols < 0)        left_cols = 0;
    if (left_cols > src.cols) left_cols = src.cols;
    *a = (rect_t){ src.row, src.col,             src.rows, left_cols            };
    *b = (rect_t){ src.row, src.col + left_cols, src.rows, src.cols - left_cols };
}

/* Split horizontally by fraction [0.0, 1.0] of parent width. */
static inline void layout_hsplit(rect_t src, float frac,
                                 rect_t *a, rect_t *b)
{
    int w = (int)((float)src.cols * frac);
    w = clamp_i(w, 0, src.cols);
    layout_hsplit_fixed(src, w, a, b);
}

/* ------------------------------------------------------------------
 * layout_inset
 *
 * Shrink a rect by `pad` on all four sides (used to turn an outer
 * border rect into the content area inside it).
 * WHY SAFE: rows/cols cannot go below zero.
 * ------------------------------------------------------------------ */
static inline rect_t layout_inset(rect_t r, int pad)
{
    int p2 = pad * 2;
    return (rect_t){
        r.row  + pad,
        r.col  + pad,
        r.rows > p2 ? r.rows - p2 : 0,
        r.cols > p2 ? r.cols - p2 : 0
    };
}

/* ------------------------------------------------------------------
 * Named panel rects (outer border rect, before inset)
 * ------------------------------------------------------------------ */

typedef struct {
    rect_t header;      /* top banner — 4 rows fixed               */
    rect_t processes;   /* main process table                       */
    rect_t summary;     /* event summary (left of mid band)         */
    rect_t activity;    /* activity bars (right of mid band)        */
    rect_t slowest;     /* slowest syscalls (left of lower band)    */
    rect_t anomaly;     /* anomaly detection (right of lower band)  */
    /* Bottom zone: lifecycle | sched_delay side-by-side             */
    rect_t lifecycle;   /* left half of bottom zone                 */
    rect_t sched_delay; /* right half of bottom zone                */
    rect_t controls;    /* 1-row keybinding bar at very bottom      */
} layout_t;

/* Compute the full panel layout for the given terminal size.
 * Returns 0 on success, -1 when the terminal is too small
 * (in which case every rect has rows==0 and nothing is drawn). */
int layout_compute(int term_rows, int term_cols, layout_t *L);

#endif /* LAYOUT_H */