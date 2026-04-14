#ifndef VSCREEN_H
#define VSCREEN_H

/* ------------------------------------------------------------------ */
/* Virtual screen buffer — cell-level diff renderer                    */
/*                                                                     */
/* Each frame:                                                         */
/*   1. vscreen_clear()            — blank the "next" buffer          */
/*   2. vscreen_put*()             — components paint cells           */
/*   3. vscreen_flush()            — diff vs "prev", emit only Δ       */
/* ------------------------------------------------------------------ */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Max supported terminal dimensions.
   Increase if you expect very large terminals. */
#define VS_MAX_ROWS 120
#define VS_MAX_COLS 400

/* ------------------------------------------------------------------ */
/* A single terminal cell                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    char     ch[5];   /* UTF-8 grapheme cluster (≤4 bytes) + NUL      */
    uint32_t fg;      /* 0x00RRGGBB; 0 = terminal default             */
    uint32_t bg;      /* 0x00RRGGBB; 0 = terminal default             */
    uint8_t  bold : 1;
    uint8_t  dim  : 1;
} vs_cell_t;

/* ------------------------------------------------------------------ */
/* Buffer pair                                                          */
/* ------------------------------------------------------------------ */

extern vs_cell_t vs_next[VS_MAX_ROWS][VS_MAX_COLS];
extern vs_cell_t vs_prev[VS_MAX_ROWS][VS_MAX_COLS];
extern int       vs_rows;
extern int       vs_cols;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* Call after every terminal resize before the next paint cycle. */
void vscreen_resize(int rows, int cols);

/* Clear the "next" frame to blank space cells. */
void vscreen_clear(void);

/* Write a single UTF-8 grapheme at (row, col) with given attributes.
   Clips silently when out of bounds. */
void vscreen_put(int row, int col,
                 const char *ch,
                 uint32_t fg, uint32_t bg,
                 bool bold, bool dim);

/* Write a NUL-terminated ASCII/UTF-8 string starting at (row, col).
   Each byte is treated as one cell (ASCII fast path).
   Returns the column after the last written character. */
int  vscreen_puts(int row, int col,
                  const char *str,
                  uint32_t fg, uint32_t bg,
                  bool bold, bool dim);

/* printf-style into a row starting at col, clipped at vs_cols.
   Returns column after last written character. */
int  vscreen_printf(int row, int col,
                    uint32_t fg, uint32_t bg,
                    bool bold, bool dim,
                    const char *fmt, ...)
    __attribute__((format(printf, 7, 8)));

/* Diff vs_next against vs_prev and write minimal ANSI to stdout.
   Swaps buffers when done. */
void vscreen_flush(void);

/* Force a full redraw on the next flush (e.g. after resize). */
void vscreen_invalidate(void);

/* Convenience: draw a horizontal line of repeated char `ch` */
void vscreen_hline(int row, int col, int len,
                   const char *ch,
                   uint32_t fg, uint32_t bg);

/* Convenience: draw a vertical line */
void vscreen_vline(int row, int col, int len,
                   const char *ch,
                   uint32_t fg, uint32_t bg);

#endif /* VSCREEN_H */
