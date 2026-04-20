#ifndef LAYOUT_H
#define LAYOUT_H

#define MIN_TERM_ROWS 20
#define MIN_TERM_COLS 60

typedef struct {
    int row;   
    int col;
    int rows; 
    int cols;  
} rect_t;


static inline int rect_valid(rect_t r)
{
    return (r.rows > 0 && r.cols > 0);
}
static inline int clamp_i(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline void layout_vsplit_fixed(rect_t src, int top_rows,
                                       rect_t *a, rect_t *b)
{
    if (top_rows < 0)        top_rows = 0;
    if (top_rows > src.rows) top_rows = src.rows;
    *a = (rect_t){ src.row,            src.col, top_rows,             src.cols };
    *b = (rect_t){ src.row + top_rows, src.col, src.rows - top_rows,  src.cols };
}


static inline void layout_vsplit(rect_t src, float frac,
                                 rect_t *a, rect_t *b)
{
    int h = (int)((float)src.rows * frac);
    h = clamp_i(h, 0, src.rows);
    layout_vsplit_fixed(src, h, a, b);
}


static inline void layout_hsplit_fixed(rect_t src, int left_cols,
                                       rect_t *a, rect_t *b)
{
    if (left_cols < 0)        left_cols = 0;
    if (left_cols > src.cols) left_cols = src.cols;
    *a = (rect_t){ src.row, src.col,             src.rows, left_cols            };
    *b = (rect_t){ src.row, src.col + left_cols, src.rows, src.cols - left_cols };
}


static inline void layout_hsplit(rect_t src, float frac,
                                 rect_t *a, rect_t *b)
{
    int w = (int)((float)src.cols * frac);
    w = clamp_i(w, 0, src.cols);
    layout_hsplit_fixed(src, w, a, b);
}


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



typedef struct {
    rect_t header;     
    rect_t processes;  
    rect_t summary;     
    rect_t activity;    
    rect_t slowest;     
    rect_t anomaly;    
    
    rect_t lifecycle;  
    rect_t sched_delay;
    rect_t controls;
} layout_t;

 * Returns 0 on success, -1 when the terminal is too small
 * (in which case every rect has rows==0 and nothing is drawn). */
int layout_compute(int term_rows, int term_cols, layout_t *L);

#endif /* LAYOUT_H */
