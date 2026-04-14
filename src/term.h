#ifndef TERM_H
#define TERM_H

/* SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause) */

#include <stdio.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <signal.h>

/* ------------------------------------------------------------------ */
/* Terminal size                                                        */
/* ------------------------------------------------------------------ */

typedef struct { int rows; int cols; } term_size_t;

static inline term_size_t term_get_size(void)
{
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0 && w.ws_col > 0)
        return (term_size_t){ w.ws_row, w.ws_col };
    return (term_size_t){ 24, 80 };  /* safe fallback */
}

/* ------------------------------------------------------------------ */
/* Alternate screen + mouse                                            */
/* ------------------------------------------------------------------ */

static inline void term_enter_alt_screen(void)
{
    fputs("\033[?1049h"   /* enter alternate screen buffer */
          "\033[?25l",    /* hide cursor                   */
          stdout);
    fflush(stdout);
}

static inline void term_leave_alt_screen(void)
{
    fputs("\033[?25h"     /* show cursor                   */
          "\033[?1049l",  /* leave alternate screen buffer */
          stdout);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Cursor movement (0-based row/col)                                   */
/* ------------------------------------------------------------------ */

static inline void term_move(int row, int col)
{
    printf("\033[%d;%dH", row + 1, col + 1);
}

static inline void term_clear_screen(void)
{
    fputs("\033[2J\033[H", stdout);
}

/* ------------------------------------------------------------------ */
/* Raw / non-blocking stdin                                            */
/* ------------------------------------------------------------------ */

static struct termios _term_orig;

static inline void term_setup_raw(void)
{
    struct termios t;
    tcgetattr(STDIN_FILENO, &_term_orig);
    t          = _term_orig;
    t.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
}

static inline void term_restore_raw(void)
{
    tcsetattr(STDIN_FILENO, TCSANOW, &_term_orig);
    /* restore blocking */
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK);
}

/* Read one byte from stdin; returns -1 if no data available */
static inline int term_read_key(void)
{
    unsigned char c;
    return (read(STDIN_FILENO, &c, 1) == 1) ? (int)c : -1;
}

/* ------------------------------------------------------------------ */
/* ANSI SGR helpers                                                    */
/* ------------------------------------------------------------------ */

#define TERM_RESET      "\033[0m"
#define TERM_BOLD       "\033[1m"
#define TERM_DIM        "\033[2m"

/* 24-bit foreground: r,g,b each 0-255 */
static inline void term_fg(unsigned r, unsigned g, unsigned b)
{
    printf("\033[38;2;%u;%u;%um", r, g, b);
}

/* 24-bit background */
static inline void term_bg(unsigned r, unsigned g, unsigned b)
{
    printf("\033[48;2;%u;%u;%um", r, g, b);
}

static inline void term_reset(void) { fputs(TERM_RESET, stdout); }

#endif /* TERM_H */
