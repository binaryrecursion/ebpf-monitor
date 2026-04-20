

#include "theme.h"



const theme_t THEME_DARK = {
    .bg_panel      = 0x0d1117,  
    .bg_header_row = 0x161b22,  
    .bg_alt_row    = 0x111820,  
    .bg_selected   = 0x1f6feb,  
    .bg_anom_row   = 0x1a0808,  


    .fg_primary    = rgb(0xe6, 0xed, 0xf3),
    .fg_secondary  = rgb(0x8b, 0x94, 0x9e),
    .fg_dim        = rgb(0x48, 0x4f, 0x58),


    .fg_green      = rgb(0x3f, 0xb9, 0x50),
    .fg_yellow     = rgb(0xe3, 0xb3, 0x41),
    .fg_orange     = rgb(0xf0, 0x88, 0x3e),
    .fg_red        = rgb(0xf8, 0x51, 0x49),
    .fg_cyan       = rgb(0x79, 0xc0, 0xff),
    .fg_blue       = rgb(0x58, 0xa6, 0xff),
    .fg_magenta    = rgb(0xd2, 0xa8, 0xff),
    .fg_teal       = rgb(0x39, 0xd3, 0x53),
    .fg_purple     = rgb(0xbc, 0x8c, 0xff),

   
    .border_dim    = rgb(0x30, 0x36, 0x3d),
    .border_title  = rgb(0x8b, 0x94, 0x9e),

    
    .bh  = "\xe2\x94\x80",  
    .bv  = "\xe2\x94\x82",  
    .btl = "\xe2\x94\x8c",   /* ┌ */
    .btr = "\xe2\x94\x90",   /* ┐ */
    .bbl = "\xe2\x94\x94",   /* └ */
    .bbr = "\xe2\x94\x98",   /* ┘ */
    .bml = "\xe2\x94\x9c",   /* ├ */
    .bmr = "\xe2\x94\xa4",   /* ┤ */

    /* ── Double-line (header banner) ── */
    .bdh  = "\xe2\x95\x90",  /* ═ */
    .bdtl = "\xe2\x95\x94",  /* ╔ */
    .bdtr = "\xe2\x95\x97",  /* ╗ */
    .bdbl = "\xe2\x95\x9a",  /* ╚ */
    .bdbr = "\xe2\x95\x9d",  /* ╝ */
    .bdv  = "\xe2\x95\x91",  /* ║ */
};


const theme_t THEME_MOCHA = {
    /* ── Backgrounds ── */
    .bg_panel      = 0x1e1e2e,  /* Mocha base — the canonical Mocha canvas    */
    .bg_header_row = 0x313244,  /* Mocha surface0                              */
    .bg_alt_row    = 0x181825,  /* Mocha mantle                                */
    .bg_selected   = 0x45475a,  /* Mocha surface1                              */
    .bg_anom_row   = 0x2a1020,  /* subtle pink tint for anomaly rows           */

    /* ── Text ── */
    .fg_primary    = rgb(0xcd, 0xd6, 0xf4),  /* Mocha text   */
    .fg_secondary  = rgb(0xa6, 0xad, 0xc8),  /* Mocha subtext1 */
    .fg_dim        = rgb(0x58, 0x5b, 0x70),  /* Mocha overlay0 */

    /* ── Semantic ── */
    .fg_green      = rgb(0xa6, 0xe3, 0xa1),  /* Mocha green   */
    .fg_yellow     = rgb(0xf9, 0xe2, 0xaf),  /* Mocha yellow  */
    .fg_orange     = rgb(0xfa, 0xb3, 0x87),  /* Mocha peach   */
    .fg_red        = rgb(0xf3, 0x8b, 0xa8),  /* Mocha red     */
    .fg_cyan       = rgb(0x89, 0xdc, 0xeb),  /* Mocha sky     */
    .fg_blue       = rgb(0x89, 0xb4, 0xfa),  /* Mocha blue    */
    .fg_magenta    = rgb(0xf5, 0xc2, 0xe7),  /* Mocha pink    */
    .fg_teal       = rgb(0x94, 0xe2, 0xd5),  /* Mocha teal    */
    .fg_purple     = rgb(0xcb, 0xa6, 0xf7),  /* Mocha mauve   */

    .border_dim    = rgb(0x31, 0x32, 0x44),  /* Mocha surface0 */
    .border_title  = rgb(0xa6, 0xad, 0xc8),

    .bh  = "\xe2\x94\x80",
    .bv  = "\xe2\x94\x82",
    .btl = "\xe2\x94\x8c",
    .btr = "\xe2\x94\x90",
    .bbl = "\xe2\x94\x94",
    .bbr = "\xe2\x94\x98",
    .bml = "\xe2\x94\x9c",
    .bmr = "\xe2\x94\xa4",

    .bdh  = "\xe2\x95\x90",
    .bdtl = "\xe2\x95\x94",
    .bdtr = "\xe2\x95\x97",
    .bdbl = "\xe2\x95\x9a",
    .bdbr = "\xe2\x95\x9d",
    .bdv  = "\xe2\x95\x91",
};



const theme_t *T = &THEME_DARK;

void theme_set(const theme_t *t) { T = t; }
