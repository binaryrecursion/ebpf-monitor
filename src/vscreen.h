#ifndef VSCREEN_H
#define VSCREEN_H


#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define VS_MAX_ROWS 120
#define VS_MAX_COLS 400


typedef struct {
    char     ch[5];  
    uint32_t fg;      
    uint32_t bg;    
    uint8_t  bold : 1;
    uint8_t  dim  : 1;
} vs_cell_t;

extern vs_cell_t vs_next[VS_MAX_ROWS][VS_MAX_COLS];
extern vs_cell_t vs_prev[VS_MAX_ROWS][VS_MAX_COLS];
extern int       vs_rows;
extern int       vs_cols;


void vscreen_resize(int rows, int cols);

void vscreen_clear(void);

void vscreen_put(int row, int col,
                 const char *ch,
                 uint32_t fg, uint32_t bg,
                 bool bold, bool dim);

int  vscreen_puts(int row, int col,
                  const char *str,
                  uint32_t fg, uint32_t bg,
                  bool bold, bool dim);

int  vscreen_printf(int row, int col,
                    uint32_t fg, uint32_t bg,
                    bool bold, bool dim,
                    const char *fmt, ...)
    __attribute__((format(printf, 7, 8)));

void vscreen_flush(void);


void vscreen_invalidate(void);


void vscreen_hline(int row, int col, int len,
                   const char *ch,
                   uint32_t fg, uint32_t bg);

void vscreen_vline(int row, int col, int len,
                   const char *ch,
                   uint32_t fg, uint32_t bg);

#endif 
