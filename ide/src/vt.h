/* kcide — VT100/xterm terminal emulator used by the integrated Terminal panel.
 * Cell grid + scrollback, cursor addressing, SGR (16/256/truecolor), scroll regions, alternate screen,
 * erase/insert/delete, DSR/DA replies, bracketed-paste and application-cursor modes, UTF-8. */
#ifndef KCIDE_VT_H
#define KCIDE_VT_H
#include "term.h"
#define VT_DEF 0xFFFFFFFEu              /* "terminal default" colour */
typedef struct { Cell *c; int w; } VRow;
typedef struct VTerm {
    int w, h, cx, cy; bool wrap_pending;
    Cell *main_g, *alt_g, *g; bool alt_active;
    int top, bot;                       /* scroll region (inclusive rows) */
    Color fg, bg; uint8_t attr;
    int scx, scy; Color sfg, sbg; uint8_t sattr;   /* saved cursor */
    bool autowrap, cursor_vis, appkeys, bracketed, origin;
    int st; char pbuf[96]; int np; uint32_t cp; int cpn;    /* parser */
    VRow *sb; int sb_n, sb_cap, sb_head;                    /* scrollback ring (main screen only) */
    int view;                           /* lines scrolled back (0 = live) */
    void (*reply)(void *ctx, const char *s, int n); void *rctx;
    uint8_t *tabs; char title[128]; unsigned gen;
} VTerm;
VTerm *vt_new(int w, int h);
void vt_free(VTerm *v);
void vt_resize(VTerm *v, int w, int h);
void vt_feed(VTerm *v, const char *d, int n);
const Cell *vt_cell(VTerm *v, int x, int y);        /* honours the scrollback view offset */
void vt_scroll_view(VTerm *v, int delta);
int  vt_encode_key(const Event *e, bool appkeys, char *out);   /* bytes to send to the child for a key; 0 = none */
#endif
