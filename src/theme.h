#ifndef THEME_H
#define THEME_H

#include <stdint.h>


#define rgb(r, g, b) ((uint32_t)(((r) << 16) | ((g) << 8) | (b)))

/* ------------------------------------------------------------------ */
/* Color helpers                                                        */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Theme struct                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    /* Backgrounds */
    uint32_t bg_panel;       /* panel interior background              */
    uint32_t bg_header_row;  /* table header row background            */
    uint32_t bg_alt_row;     /* alternating row background             */
    uint32_t bg_selected;    /* selected / focused row                 */

    /* Text */
    uint32_t fg_primary;     /* bright white — main content            */
    uint32_t fg_secondary;   /* medium gray — labels, units            */
    uint32_t fg_dim;         /* dark gray — decorations, borders       */

    /* Semantic */
    uint32_t fg_green;
    uint32_t fg_yellow;
    uint32_t fg_orange;
    uint32_t fg_red;
    uint32_t fg_cyan;
    uint32_t fg_blue;
    uint32_t fg_magenta;
    uint32_t fg_teal;
    uint32_t fg_purple;

    /* Border */
    uint32_t border_dim;     /* box-drawing chars                      */
    uint32_t border_title;   /* panel title text                       */

    /* Box-drawing characters (UTF-8 strings) */
    const char *bh;          /* ─ horizontal                           */
    const char *bv;          /* │ vertical                             */
    const char *btl;         /* ┌ top-left                             */
    const char *btr;         /* ┐ top-right                            */
    const char *bbl;         /* └ bottom-left                          */
    const char *bbr;         /* ┘ bottom-right                         */
    const char *bml;         /* ├ mid-left (divider)                   */
    const char *bmr;         /* ┤ mid-right                            */
    const char *bdh;         /* ═ double horizontal (header)           */
    const char *bdtl;        /* ╔ double top-left                      */
    const char *bdtr;        /* ╗ double top-right                     */
    const char *bdbl;        /* ╚ double bottom-left                   */
    const char *bdbr;        /* ╝ double bottom-right                  */
    const char *bdv;         /* ║ double vertical                      */
} theme_t;

/* ------------------------------------------------------------------ */
/* Built-in themes                                                      */
/* ------------------------------------------------------------------ */

extern const theme_t THEME_DARK;    /* default dark theme              */
extern const theme_t THEME_MOCHA;   /* Catppuccin Mocha                */

extern const theme_t *T;            /* active theme pointer            */

void theme_set(const theme_t *t);

#endif /* THEME_H */
