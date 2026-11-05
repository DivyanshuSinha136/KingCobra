/* kcide — output panel model: interprets the ANSI stream from child processes into styled lines and
 * recognises `file:line:col: error|warning: message` diagnostics. */
#ifndef KCIDE_OUTPUT_H
#define KCIDE_OUTPUT_H
#include "term.h"
typedef struct { uint32_t ch; Color fg; uint8_t attr; } OCell;
typedef struct { OCell *c; int n, cap; int diag; /* index into Output.diags or -1 */ } OLine;
typedef struct { char *file; int line, col; int sev; /* 0 error, 1 warning */ char *msg; } Diag;
typedef struct {
    OLine *lines; int n, cap;
    Diag *diags; int ndiag, dcap;
    int col;                      /* write cursor in the last line */
    int st;                       /* parser state */
    Str esc; Color fg; uint8_t attr; uint8_t pend[4]; int npend;
    int scroll; bool follow;      /* scroll = first visible line */
    Color default_fg;
} Output;
void out_init(Output *o, Color default_fg);
void out_clear(Output *o);
void out_feed(Output *o, const char *d, int n);
void out_puts(Output *o, const char *s, Color fg, uint8_t attr);   /* appends text on a fresh line if needed */
void out_free(Output *o);
void out_add_link(Output *o, const char *file, int line, int col, const char *text, Color fg); /* clickable result line (sev 2) */
void out_drop_diags_for(Output *o, const char *file);
#endif
