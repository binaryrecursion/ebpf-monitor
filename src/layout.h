#ifndef LAYOUT_H
#define LAYOUT_H

/* ------------------------------------------------------------------ */
/* Layout engine — fractional panel grid                               */
/*                                                                     */
/* All panels are defined as fractions of the terminal.               */
/* Call layout_compute() every frame after a resize.                  */
/* ------------------------------------------------------------------ */

typedef struct {
    int row;    /* top-left, 0-based */
    int col;
    int rows;   /* height */
    int cols;   /* width  */
} rect_t;

/* Split `src` vertically (top/bottom) at a fixed row count `top_rows`.
   `a` gets the top portion, `b` gets the rest. */
static inline void layout_vsplit_fixed(rect_t src, int top_rows,
                                       rect_t *a, rect_t *b)
{
    if (top_rows > src.rows) top_rows = src.rows;
    *a = (rect_t){ src.row,             src.col, top_rows,             src.cols };
    *b = (rect_t){ src.row + top_rows,  src.col, src.rows - top_rows,  src.cols };
}

/* Split vertically by fraction (0.0–1.0) of parent height */
static inline void layout_vsplit(rect_t src, float frac,
                                 rect_t *a, rect_t *b)
{
    int h = (int)(src.rows * frac);
    if (h < 0) h = 0;
    if (h > src.rows) h = src.rows;
    layout_vsplit_fixed(src, h, a, b);
}

/* Split `src` horizontally (left/right) at a fixed col count `left_cols` */
static inline void layout_hsplit_fixed(rect_t src, int left_cols,
                                       rect_t *a, rect_t *b)
{
    if (left_cols > src.cols) left_cols = src.cols;
    *a = (rect_t){ src.row, src.col,            src.rows, left_cols };
    *b = (rect_t){ src.row, src.col + left_cols, src.rows, src.cols - left_cols };
}

/* Split horizontally by fraction of parent width */
static inline void layout_hsplit(rect_t src, float frac,
                                 rect_t *a, rect_t *b)
{
    int w = (int)(src.cols * frac);
    if (w < 0) w = 0;
    if (w > src.cols) w = src.cols;
    layout_hsplit_fixed(src, w, a, b);
}

/* Shrink a rect by `pad` on all sides (used to get content area from panel) */
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

/* ------------------------------------------------------------------ */
/* Named panel rectangles (outer border rect, before inset)           */
/* ------------------------------------------------------------------ */

typedef struct {
    rect_t header;      /* top status bar        */
    rect_t processes;   /* main process table    */
    rect_t summary;     /* event summary         */
    rect_t activity;    /* activity bar graph    */
    rect_t slowest;     /* slowest syscalls      */
    rect_t anomaly;     /* anomaly detection     */
    rect_t lifecycle;   /* process lifecycle     */
    rect_t controls;    /* bottom keybindings    */
} layout_t;

/* Compute the full panel layout for the given terminal size. */
void layout_compute(int term_rows, int term_cols, layout_t *L);

#endif /* LAYOUT_H */
