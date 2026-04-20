
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>

#include "vscreen.h"
#include "term.h"



vs_cell_t vs_next[VS_MAX_ROWS][VS_MAX_COLS];
vs_cell_t vs_prev[VS_MAX_ROWS][VS_MAX_COLS];
int       vs_rows = 24;
int       vs_cols = 80;

static bool g_invalidated = true;  

static const vs_cell_t BLANK = {
    .ch   = " ",
    .fg   = 0,
    .bg   = 0,
    .bold = 0,
    .dim  = 0
};

void vscreen_resize(int rows, int cols)
{
    if (rows > VS_MAX_ROWS) rows = VS_MAX_ROWS;
    if (cols > VS_MAX_COLS) cols = VS_MAX_COLS;
    vs_rows = rows;
    vs_cols = cols;

    memset(vs_prev, 0xff, sizeof(vs_prev));
    g_invalidated = true;
}


void vscreen_clear(void)
{
    for (int r = 0; r < vs_rows; r++)
        for (int c = 0; c < vs_cols; c++)
            vs_next[r][c] = BLANK;
}

void vscreen_invalidate(void)
{
    memset(vs_prev, 0xff, sizeof(vs_prev));
    g_invalidated = true;
}

void vscreen_put(int row, int col,
                 const char *ch,
                 uint32_t fg, uint32_t bg,
                 bool bold, bool dim)
{
    if (row < 0 || row >= vs_rows) return;
    if (col < 0 || col >= vs_cols) return;
    vs_cell_t *cell = &vs_next[row][col];
    cell->ch[0] = ch[0];
    cell->ch[1] = ch[1];
    cell->ch[2] = ch[2];
    cell->ch[3] = ch[3];
    cell->ch[4] = '\0';
    cell->fg   = fg;
    cell->bg   = bg;
    cell->bold = bold ? 1 : 0;
    cell->dim  = dim  ? 1 : 0;
}

int vscreen_puts(int row, int col,
                 const char *str,
                 uint32_t fg, uint32_t bg,
                 bool bold, bool dim)
{
    if (!str) return col;
    const unsigned char *p = (const unsigned char *)str;
    int c = col;
    while (*p && c < vs_cols) {
        char buf[5] = {0, 0, 0, 0, 0};
        unsigned char b0 = p[0];
        int bytes = 1;
        if      ((b0 & 0x80) == 0x00) { bytes = 1; }
        else if ((b0 & 0xE0) == 0xC0) { bytes = 2; }
        else if ((b0 & 0xF0) == 0xE0) { bytes = 3; }
        else if ((b0 & 0xF8) == 0xF0) { bytes = 4; }
        for (int i = 0; i < bytes; i++) buf[i] = (char)p[i];
        vscreen_put(row, c, buf, fg, bg, bold, dim);
        p += bytes;
        c++;
    }
    return c;
}


int vscreen_printf(int row, int col,
                   uint32_t fg, uint32_t bg,
                   bool bold, bool dim,
                   const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return vscreen_puts(row, col, buf, fg, bg, bold, dim);
}


void vscreen_hline(int row, int col, int len,
                   const char *ch,
                   uint32_t fg, uint32_t bg)
{
    for (int i = 0; i < len; i++)
        vscreen_put(row, col + i, ch, fg, bg, false, false);
}

void vscreen_vline(int row, int col, int len,
                   const char *ch,
                   uint32_t fg, uint32_t bg)
{
    for (int i = 0; i < len; i++)
        vscreen_put(row + i, col, ch, fg, bg, false, false);
}


void vscreen_flush(void)
{
    int      cur_row   = -1;
    int      cur_col   = -1;
    uint32_t cur_fg    = 0xFFFFFFFF;
    uint32_t cur_bg    = 0xFFFFFFFF;
    bool     cur_bold  = false;
    bool     cur_dim   = false;
    bool     attr_dirty = true;

    for (int r = 0; r < vs_rows; r++) {
        for (int c = 0; c < vs_cols; c++) {
            const vs_cell_t *n = &vs_next[r][c];
            const vs_cell_t *p = &vs_prev[r][c];

         
            if (!g_invalidated &&
                n->fg   == p->fg   &&
                n->bg   == p->bg   &&
                n->bold == p->bold &&
                n->dim  == p->dim  &&
                memcmp(n->ch, p->ch, 5) == 0)
                continue;

            if (cur_row != r || cur_col != c) {
                term_move(r, c);
                cur_row    = r;
                cur_col    = c;
                attr_dirty = true;
            }

            /* Emit SGR if attrs changed */
            if (attr_dirty        ||
                n->fg   != cur_fg  ||
                n->bg   != cur_bg  ||
                n->bold != cur_bold ||
                n->dim  != cur_dim)
            {
                fputs("\033[0m", stdout);           /* reset all attrs */
                if (n->bold) fputs("\033[1m", stdout);
                if (n->dim)  fputs("\033[2m", stdout);
                if (n->fg) {
                    printf("\033[38;2;%u;%u;%um",
                           (n->fg >> 16) & 0xff,
                           (n->fg >>  8) & 0xff,
                            n->fg        & 0xff);
                }
                if (n->bg) {
                    printf("\033[48;2;%u;%u;%um",
                           (n->bg >> 16) & 0xff,
                           (n->bg >>  8) & 0xff,
                            n->bg        & 0xff);
                }
                cur_fg     = n->fg;
                cur_bg     = n->bg;
                cur_bold   = n->bold;
                cur_dim    = n->dim;
                attr_dirty = false;
            }

            fputs(n->ch[0] ? n->ch : " ", stdout);
            cur_col++;

            /* Commit to prev buffer */
            vs_prev[r][c] = *n;
        }
    }

    /* Reset SGR and flush */
    fputs("\033[0m", stdout);
    fflush(stdout);
    g_invalidated = false;
}
