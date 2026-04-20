#include "layout.h"

static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int imin(int a, int b) { return a < b ? a : b; }


int layout_compute(int term_rows, int term_cols, layout_t *L)
{
    
    *L = (layout_t){0};

    if (term_rows < MIN_TERM_ROWS || term_cols < MIN_TERM_COLS)
        return -1;

    rect_t screen = { 0, 0, term_rows, term_cols };

    rect_t body, ctrl;
    layout_vsplit_fixed(screen, term_rows - 1, &body, &ctrl);
    L->controls = ctrl;

    rect_t after_hdr;
    {
        int hdr_rows = imin(4, body.rows);
        layout_vsplit_fixed(body, hdr_rows, &L->header, &after_hdr);
    }
    int avail = after_hdr.rows;

   
    int proc_rows = imax(7, (avail * 30) / 100);
    proc_rows     = imin(proc_rows, avail);

    rect_t after_proc;
    layout_vsplit_fixed(after_hdr, proc_rows, &L->processes, &after_proc);
    avail = after_proc.rows;

   
    int mid_rows = imax(7, (avail * 35) / 100);
    mid_rows     = imin(mid_rows, avail);

    rect_t mid_band, after_mid;
    layout_vsplit_fixed(after_proc, mid_rows, &mid_band, &after_mid);
   
    layout_hsplit(mid_band, 0.5f, &L->summary, &L->activity);
    avail = after_mid.rows;

    
    int low_rows = imax(7, (avail * 40) / 100);
    low_rows     = imin(low_rows, avail);

    rect_t low_band, after_low;
    layout_vsplit_fixed(after_mid, low_rows, &low_band, &after_low);
    layout_hsplit(low_band, 0.5f, &L->slowest, &L->anomaly);

    rect_t bottom_zone = after_low;   

    if (bottom_zone.cols >= 80) {
       
        layout_hsplit(bottom_zone, 0.5f, &L->lifecycle, &L->sched_delay);
    } else {
        
        L->lifecycle   = bottom_zone;
        L->sched_delay = (rect_t){ bottom_zone.row, bottom_zone.col,
                                   bottom_zone.rows, 0 };
    }

    return 0;
}
