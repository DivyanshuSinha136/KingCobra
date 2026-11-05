/* kcide — terminal layer: raw mode, VT input decoding (keys, SGR mouse, bracketed paste),
 * and a double-buffered cell screen that is flushed as a minimal diff. */
#ifndef KCIDE_TERM_H
#define KCIDE_TERM_H
#include "util.h"

typedef uint32_t Color;                 /* 0xRRGGBB */
enum { ATTR_BOLD = 1, ATTR_ITALIC = 2, ATTR_UNDERLINE = 4, ATTR_REVERSE = 8, ATTR_DIM = 16 };
typedef struct { Color fg, bg; uint8_t attr; } Style;
typedef struct { uint32_t ch; Color fg, bg; uint8_t attr; } Cell;
#define ST(f, b) ((Style){ (f), (b), 0 })
#define STA(f, b, a) ((Style){ (f), (b), (uint8_t)(a) })

enum { MOD_SHIFT = 1, MOD_ALT = 2, MOD_CTRL = 4 };
enum {
    K_NONE = 0, K_UP = 0x200001, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN, K_INS, K_DEL,
    K_BKSP, K_ENTER, K_TAB, K_ESC,
    K_F1 = 0x200100, /* K_F1 + n - 1 */
};
typedef enum { EV_NONE, EV_KEY, EV_MOUSE, EV_RESIZE, EV_PASTE } EvType;
typedef struct {
    EvType type;
    int key;            /* K_* or the (lower-case) unicode codepoint */
    int mods;           /* MOD_* */
    int mx, my;         /* 0-based cell, for EV_MOUSE */
    int btn;            /* 0 left, 1 middle, 2 right, 3 none(motion), 4 wheel up, 5 wheel down */
    bool press, release, drag;
    char *paste;        /* EV_PASTE: malloc'd UTF-8 text (caller frees) */
} Event;
#define K_FN(n) (K_F1 + (n) - 1)

extern int term_w, term_h;
extern bool term_truecolor;

bool term_open(void);
void term_close(void);
void term_suspend(void);   /* leave the alt screen / restore cooked mode (to run a program in the terminal) */
void term_resume(void);
bool term_next_event(Event *ev, int timeout_ms, const int *fds, int nfds, bool *extra_ready);   /* fds: extra descriptors to wake on (POSIX) */

/* screen */
void scr_clear(Style s);
void scr_putc(int x, int y, uint32_t ch, Style s);
int  scr_text(int x, int y, const char *utf8, int maxw, Style s);     /* returns columns drawn */
int  scr_textn(int x, int y, const char *utf8, int n, int maxw, Style s);
void scr_fill(int x, int y, int w, int h, Style s);
void scr_restyle(int x, int y, int w, int h, Color fg, Color bg);     /* recolor without changing glyphs (fg/bg 0xFFFFFFFF = keep) */
Cell *scr_cell(int x, int y);
void scr_cursor(int x, int y, bool visible);
void scr_flush(void);
void scr_invalidate(void);
void term_set_title(const char *t);
void term_clipboard_set(const char *utf8, size_t n);                  /* OSC 52 */
#define KEEP 0xFFFFFFFFu
#endif
