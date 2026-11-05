/* kcide — text document: line array, undo/redo, cached syntax highlighting, cursor/selection state. */
#ifndef KCIDE_DOC_H
#define KCIDE_DOC_H
#include "syntax.h"

typedef struct { int line, col; } Pos;                 /* col is a byte offset into the line */
static inline int pos_cmp(Pos a, Pos b) { return a.line != b.line ? (a.line < b.line ? -1 : 1) : (a.col < b.col ? -1 : a.col > b.col ? 1 : 0); }

enum { LF_CHANGED = 1, LF_BREAK = 2 };
typedef struct { char *s; int len, cap; uint8_t *hl; uint8_t state_out; uint8_t flags; bool hl_ok; } Line;
typedef struct { bool ins; Pos pos; char *text; int len; int group; } UndoOp;

typedef struct Doc {
    Line *lines; int nlines, lcap;
    char *path;                /* absolute path, NULL for untitled */
    char *title;               /* tab label */
    bool dirty, crlf, readonly, is_kc, final_nl;
    Lang lang;                 /* highlighter / editing rules; is_kc == (lang == LANG_KC) */
    int hl_valid;              /* lines [0, hl_valid) have valid highlight data */
    unsigned version;          /* bumped by every edit */
    UndoOp *undo, *redo; int nundo, ucap, nredo, rcap;
    int group, saved_depth; bool new_group, replaying;
    long mtime;
    /* view state */
    Pos cur, anchor; bool has_sel; int want_vcol; int top, left;
} Doc;

Doc *doc_new(const char *title);
void doc_set_lang(Doc *d, Lang l);                    /* also resets cached highlighting */
Doc *doc_open(const char *path, char *err, size_t errcap);
void doc_free(Doc *d);
bool doc_save(Doc *d, const char *path, bool trim_ws, char *err, size_t errcap);
bool doc_reload(Doc *d, char *err, size_t errcap);

static inline Line *doc_line(Doc *d, int i) { return &d->lines[i]; }
Pos doc_end(Doc *d);
Pos pos_after(Pos p, const char *t, int n);
char *doc_text(Doc *d, Pos a, Pos b, int *len);       /* malloc'd; '\n' between lines */
void doc_insert(Doc *d, Pos at, const char *t, int n, Pos *end);
void doc_delete(Doc *d, Pos a, Pos b);
void doc_break_undo(Doc *d);
bool doc_undo(Doc *d, Pos *cursor);
bool doc_redo(Doc *d, Pos *cursor);
Pos doc_clamp(Doc *d, Pos p);

/* syntax */
void doc_ensure_hl(Doc *d, int upto_line);
const uint8_t *doc_hl(Doc *d, int line);

/* column helpers (display columns with tab expansion) */
int doc_vcol(Doc *d, int line, int col, int tabw);   /* display column of byte offset */
int doc_col_at_vcol(Doc *d, int line, int vcol, int tabw);
int doc_indent_width(Doc *d, int line);              /* bytes of leading whitespace */

/* motion helpers */
int  doc_next_col(Doc *d, int line, int col);        /* next char boundary (or -1 at EOL) */
int  doc_prev_col(Doc *d, int line, int col);
Pos  doc_word_left(Doc *d, Pos p);
Pos  doc_word_right(Doc *d, Pos p);
bool doc_word_at(Doc *d, Pos p, int *a, int *b);     /* identifier bounds around/at p on its line */
bool doc_match_bracket(Doc *d, Pos p, Pos *out);     /* bracket at/before p -> its partner (code-only) */
#endif
