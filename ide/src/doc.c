#include "doc.h"
#include <time.h>

/* ---------------------------------------------------------------- lines */
static void line_init(Line *l) { memset(l, 0, sizeof *l); l->cap = 16; l->s = xmalloc((size_t)l->cap); l->s[0] = 0; }
static void line_free(Line *l) { free(l->s); free(l->hl); }
static void line_reserve(Line *l, int extra) {
    if (l->len + extra + 1 <= l->cap) return;
    while (l->len + extra + 1 > l->cap) l->cap *= 2;
    l->s = xrealloc(l->s, (size_t)l->cap);
}
static void line_set(Line *l, const char *s, int n) { l->len = 0; line_reserve(l, n); memcpy(l->s, s, (size_t)n); l->len = n; l->s[n] = 0; }
static void lines_insert(Doc *d, int at, int count) {
    if (d->nlines + count > d->lcap) { while (d->nlines + count > d->lcap) d->lcap *= 2; d->lines = xrealloc(d->lines, (size_t)d->lcap * sizeof(Line)); }
    memmove(d->lines + at + count, d->lines + at, (size_t)(d->nlines - at) * sizeof(Line));
    for (int i = 0; i < count; i++) line_init(&d->lines[at + i]);
    d->nlines += count;
}
static void lines_remove(Doc *d, int at, int count) {
    for (int i = 0; i < count; i++) line_free(&d->lines[at + i]);
    memmove(d->lines + at, d->lines + at + count, (size_t)(d->nlines - at - count) * sizeof(Line));
    d->nlines -= count;
}
static void touch(Doc *d, int line) {
    if (line < d->hl_valid) d->hl_valid = line;
    for (int i = line; i < d->nlines && i <= line; i++) d->lines[i].hl_ok = false;
    d->version++;
}

void doc_set_lang(Doc *d, Lang l) { d->lang = l; d->is_kc = l == LANG_KC; d->hl_valid = 0; d->version++; }
Doc *doc_new(const char *title) {
    Doc *d = xcalloc(1, sizeof *d);
    d->lcap = 64; d->lines = xmalloc((size_t)d->lcap * sizeof(Line));
    d->nlines = 1; line_init(&d->lines[0]);
    d->title = xstrdup(title ? title : "untitled");
    d->final_nl = true; d->saved_depth = 0;
    d->ucap = d->rcap = 64; d->undo = xmalloc((size_t)d->ucap * sizeof(UndoOp)); d->redo = xmalloc((size_t)d->rcap * sizeof(UndoOp));
    return d;
}
void doc_free(Doc *d) {
    if (!d) return;
    for (int i = 0; i < d->nlines; i++) line_free(&d->lines[i]);
    for (int i = 0; i < d->nundo; i++) free(d->undo[i].text);
    for (int i = 0; i < d->nredo; i++) free(d->redo[i].text);
    free(d->lines); free(d->undo); free(d->redo); free(d->path); free(d->title); free(d);
}

static void load_text(Doc *d, const char *data, size_t n) {
    for (int i = 0; i < d->nlines; i++) line_free(&d->lines[i]);
    d->nlines = 0; d->crlf = false;
    size_t i = 0; bool first = true;
    while (i <= n) {
        size_t j = i; while (j < n && data[j] != '\n') j++;
        size_t e = j; bool cr = e > i && data[e - 1] == '\r'; if (cr) e--;
        if (first && cr) d->crlf = true;
        first = false;
        lines_insert(d, d->nlines, 1);
        line_set(&d->lines[d->nlines - 1], data + i, (int)(e - i));
        if (j >= n) break;
        i = j + 1;
    }
    d->final_nl = n > 0 && data[n - 1] == '\n';
    if (d->final_nl && d->nlines > 1 && d->lines[d->nlines - 1].len == 0) lines_remove(d, d->nlines - 1, 1);
    if (d->nlines == 0) { lines_insert(d, 0, 1); }
    d->hl_valid = 0; d->version++;
}

Doc *doc_open(const char *path, char *err, size_t errcap) {
    size_t n; char *data = read_file(path, &n);
    if (!data) { snprintf(err, errcap, "cannot read %s", path); return NULL; }
    Doc *d = doc_new(path_basename(path));
    d->path = path_abs(path);
    load_text(d, data, n);
    { Lang l = lang_from_path(path); if (l == LANG_KCM_MUSIC) { Lang k = lang_sniff_kcm(data, n); l = k; } doc_set_lang(d, l); }
    free(data);
    d->mtime = file_mtime(d->path);
    if (n == 0) d->final_nl = true;
    return d;
}

static void clear_undo(Doc *d) {
    for (int i = 0; i < d->nundo; i++) free(d->undo[i].text);
    for (int i = 0; i < d->nredo; i++) free(d->redo[i].text);
    d->nundo = d->nredo = 0; d->saved_depth = 0;
}
bool doc_reload(Doc *d, char *err, size_t errcap) {
    if (!d->path) return false;
    size_t n; char *data = read_file(d->path, &n);
    if (!data) { snprintf(err, errcap, "cannot read %s", d->path); return false; }
    load_text(d, data, n); free(data); clear_undo(d);
    d->dirty = false; d->mtime = file_mtime(d->path); d->cur = d->anchor = (Pos){ 0, 0 }; d->has_sel = false;
    return true;
}

bool doc_save(Doc *d, const char *path, bool trim_ws, char *err, size_t errcap) {
    const char *target = path ? path : d->path;
    if (!target) { snprintf(err, errcap, "no file name"); return false; }
    if (trim_ws) for (int i = 0; i < d->nlines; i++) {
        Line *l = &d->lines[i]; int e = l->len;
        while (e > 0 && (l->s[e - 1] == ' ' || l->s[e - 1] == '\t')) e--;
        if (e != l->len) { Pos a = { i, e }, b = { i, l->len }; doc_break_undo(d); doc_delete(d, a, b); if (d->cur.line == i && d->cur.col > e) d->cur.col = e; }
    }
    Str s; str_init(&s);
    const char *nl = d->crlf ? "\r\n" : "\n";
    for (int i = 0; i < d->nlines; i++) {
        str_add(&s, d->lines[i].s, (size_t)d->lines[i].len);
        if (i + 1 < d->nlines || d->final_nl) str_adds(&s, nl);
    }
    char *abs = path ? path_abs(path) : NULL;
    bool ok = write_file_atomic(target, s.p, s.n);
    str_free(&s);
    if (!ok) { snprintf(err, errcap, "cannot write %s", target); free(abs); return false; }
    if (path) {
        free(d->path); d->path = abs; free(d->title); d->title = xstrdup(path_basename(path));
        doc_set_lang(d, lang_from_path(path));
        if (d->lang == LANG_KCM_MUSIC) { int tn; char *tx = doc_text(d, (Pos){ 0, 0 }, doc_end(d), &tn); doc_set_lang(d, lang_sniff_kcm(tx, (size_t)tn)); free(tx); }
    }
    d->saved_depth = d->nundo; d->dirty = false;
    for (int i = 0; i < d->nlines; i++) d->lines[i].flags &= (uint8_t)~LF_CHANGED;   /* breakpoints (LF_BREAK) survive a save */
    d->mtime = file_mtime(d->path);
    return true;
}

/* ---------------------------------------------------------------- positions */
Pos doc_end(Doc *d) { return (Pos){ d->nlines - 1, d->lines[d->nlines - 1].len }; }
Pos doc_clamp(Doc *d, Pos p) {
    if (p.line < 0) return (Pos){ 0, 0 };
    if (p.line >= d->nlines) return doc_end(d);
    if (p.col < 0) p.col = 0;
    if (p.col > d->lines[p.line].len) p.col = d->lines[p.line].len;
    while (p.col > 0 && p.col < d->lines[p.line].len && ((unsigned char)d->lines[p.line].s[p.col] & 0xC0) == 0x80) p.col--;
    return p;
}
Pos pos_after(Pos p, const char *t, int n) {
    int nl = 0, last = -1;
    for (int i = 0; i < n; i++) if (t[i] == '\n') { nl++; last = i; }
    if (!nl) return (Pos){ p.line, p.col + n };
    return (Pos){ p.line + nl, n - last - 1 };
}

/* ---------------------------------------------------------------- raw edits */
static void raw_insert(Doc *d, Pos at, const char *t, int n, Pos *end) {
    Line *L = &d->lines[at.line];
    int nl = 0; for (int i = 0; i < n; i++) if (t[i] == '\n') nl++;
    if (!nl) {
        line_reserve(L, n);
        memmove(L->s + at.col + n, L->s + at.col, (size_t)(L->len - at.col + 1));
        memcpy(L->s + at.col, t, (size_t)n); L->len += n; L->flags |= LF_CHANGED;
        if (end) *end = (Pos){ at.line, at.col + n };
        touch(d, at.line); return;
    }
    int tail_len = L->len - at.col; char *tail = xstrndup(L->s + at.col, (size_t)tail_len);
    L->len = at.col; L->s[L->len] = 0; L->flags |= LF_CHANGED;
    int i = 0, line = at.line; bool firstseg = true;
    while (i <= n) {
        int j = i; while (j < n && t[j] != '\n') j++;
        if (firstseg) { Line *l = &d->lines[line]; line_reserve(l, j - i); memcpy(l->s + l->len, t + i, (size_t)(j - i)); l->len += j - i; l->s[l->len] = 0; firstseg = false; }
        else {
            lines_insert(d, line, 1);
            Line *l = &d->lines[line]; line_set(l, t + i, j - i); l->flags |= LF_CHANGED;
        }
        if (j >= n) break;
        i = j + 1; line++;
        if (line >= d->nlines || (!firstseg && j < n)) { /* ensure slot for next */ }
        /* next iteration will insert a fresh line at index line+? */
        line = line; 
        /* new line index = previous + 1; for first segment we need to start inserting after at.line */
        if (i <= n) { /* lines_insert places at `line`, which equals previous+1 */ }
    }
    /* after loop `line` is the last inserted line (or at.line when no split); append tail there */
    Line *last = &d->lines[line];
    int endcol = last->len;
    line_reserve(last, tail_len); memcpy(last->s + last->len, tail, (size_t)tail_len); last->len += tail_len; last->s[last->len] = 0;
    free(tail);
    if (end) *end = (Pos){ line, endcol };
    touch(d, at.line);
}
static void raw_delete(Doc *d, Pos a, Pos b) {
    if (a.line == b.line) {
        Line *L = &d->lines[a.line];
        memmove(L->s + a.col, L->s + b.col, (size_t)(L->len - b.col + 1)); L->len -= b.col - a.col; L->flags |= LF_CHANGED;
    } else {
        Line *A = &d->lines[a.line], *B = &d->lines[b.line];
        A->len = a.col; A->s[A->len] = 0;
        int tl = B->len - b.col; line_reserve(A, tl); memcpy(A->s + A->len, B->s + b.col, (size_t)tl); A->len += tl; A->s[A->len] = 0; A->flags |= LF_CHANGED;
        lines_remove(d, a.line + 1, b.line - a.line);
    }
    touch(d, a.line);
}

/* ---------------------------------------------------------------- undo */
void doc_break_undo(Doc *d) { d->new_group = true; }
static void push_op(Doc *d, bool ins, Pos pos, const char *t, int n) {
    if (d->replaying) return;
    for (int i = 0; i < d->nredo; i++) free(d->redo[i].text);
    d->nredo = 0;
    if (d->nundo < d->saved_depth) d->saved_depth = -1;
    if (d->new_group || d->nundo == 0) { d->group++; d->new_group = false; }
    if (d->nundo == d->ucap) { d->ucap *= 2; d->undo = xrealloc(d->undo, (size_t)d->ucap * sizeof(UndoOp)); }
    d->undo[d->nundo++] = (UndoOp){ ins, pos, xstrndup(t, (size_t)n), n, d->group };
    if (d->nundo > 20000) { /* drop the oldest group */
        int g = d->undo[0].group, k = 0; while (k < d->nundo && d->undo[k].group == g) { free(d->undo[k].text); k++; }
        memmove(d->undo, d->undo + k, (size_t)(d->nundo - k) * sizeof(UndoOp)); d->nundo -= k; if (d->saved_depth >= 0) d->saved_depth -= k; if (d->saved_depth < 0) d->saved_depth = -1;
    }
}
static void update_dirty(Doc *d) { d->dirty = d->nundo != d->saved_depth; }

void doc_insert(Doc *d, Pos at, const char *t, int n, Pos *end) {
    if (n <= 0) { if (end) *end = at; return; }
    char *norm = NULL;
    if (memchr(t, '\r', (size_t)n)) {
        norm = xmalloc((size_t)n + 1); int m = 0;
        for (int i = 0; i < n; i++) { if (t[i] == '\r') { if (i + 1 < n && t[i + 1] == '\n') continue; norm[m++] = '\n'; } else norm[m++] = t[i]; }
        t = norm; n = m;
    }
    at = doc_clamp(d, at);
    Pos e; raw_insert(d, at, t, n, &e);
    push_op(d, true, at, t, n);
    if (end) *end = e;
    free(norm);
    update_dirty(d);
}
void doc_delete(Doc *d, Pos a, Pos b) {
    if (pos_cmp(a, b) > 0) { Pos t = a; a = b; b = t; }
    a = doc_clamp(d, a); b = doc_clamp(d, b);
    if (pos_cmp(a, b) == 0) return;
    int n; char *t = doc_text(d, a, b, &n);
    raw_delete(d, a, b);
    push_op(d, false, a, t, n);
    free(t); update_dirty(d);
}
char *doc_text(Doc *d, Pos a, Pos b, int *len) {
    Str s; str_init(&s);
    for (int i = a.line; i <= b.line && i < d->nlines; i++) {
        int from = i == a.line ? a.col : 0, to = i == b.line ? b.col : d->lines[i].len;
        if (to > d->lines[i].len) to = d->lines[i].len;
        if (to > from) str_add(&s, d->lines[i].s + from, (size_t)(to - from));
        if (i < b.line) str_addc(&s, '\n');
    }
    if (len) *len = (int)s.n;
    return str_take(&s);
}
bool doc_undo(Doc *d, Pos *cursor) {
    if (d->nundo == 0) return false;
    int g = d->undo[d->nundo - 1].group; d->replaying = true;
    while (d->nundo > 0 && d->undo[d->nundo - 1].group == g) {
        UndoOp op = d->undo[--d->nundo];
        if (op.ins) { Pos e = pos_after(op.pos, op.text, op.len); raw_delete(d, op.pos, e); *cursor = op.pos; }
        else { Pos e; raw_insert(d, op.pos, op.text, op.len, &e); *cursor = e; }
        if (d->nredo == d->rcap) { d->rcap *= 2; d->redo = xrealloc(d->redo, (size_t)d->rcap * sizeof(UndoOp)); }
        d->redo[d->nredo++] = op;
    }
    d->replaying = false; d->new_group = true; update_dirty(d);
    return true;
}
bool doc_redo(Doc *d, Pos *cursor) {
    if (d->nredo == 0) return false;
    int g = d->redo[d->nredo - 1].group; d->replaying = true;
    while (d->nredo > 0 && d->redo[d->nredo - 1].group == g) {
        UndoOp op = d->redo[--d->nredo];
        if (op.ins) { Pos e; raw_insert(d, op.pos, op.text, op.len, &e); *cursor = e; }
        else { Pos e = pos_after(op.pos, op.text, op.len); raw_delete(d, op.pos, e); *cursor = op.pos; }
        if (d->nundo == d->ucap) { d->ucap *= 2; d->undo = xrealloc(d->undo, (size_t)d->ucap * sizeof(UndoOp)); }
        d->undo[d->nundo++] = op;
    }
    d->replaying = false; d->new_group = true; update_dirty(d);
    return true;
}

/* ---------------------------------------------------------------- highlighting */
void doc_ensure_hl(Doc *d, int upto) {
    if (upto >= d->nlines) upto = d->nlines - 1;
    for (int i = d->hl_valid; i <= upto; i++) {
        Line *l = &d->lines[i];
        uint8_t st = i ? d->lines[i - 1].state_out : 0;
        l->hl = xrealloc(l->hl, (size_t)l->len + 1);
        if (d->lang != LANG_PLAIN) l->state_out = hl_line_lang(d->lang, l->s, l->len, st, l->hl);
        else { memset(l->hl, HC_TEXT, (size_t)l->len + 1); l->state_out = 0; }
        l->hl_ok = true;
        d->hl_valid = i + 1;
    }
}
const uint8_t *doc_hl(Doc *d, int line) { doc_ensure_hl(d, line); return d->lines[line].hl; }

/* ---------------------------------------------------------------- columns */
int doc_vcol(Doc *d, int line, int col, int tabw) {
    Line *l = &d->lines[line]; int v = 0, i = 0;
    while (i < col && i < l->len) {
        uint32_t cp; int k = utf8_decode(l->s + i, (size_t)(l->len - i), &cp); i += k;
        if (cp == '\t') v += tabw - v % tabw; else if (cp < 32) v += 1; else v += cp_width(cp);
    }
    return v;
}
int doc_col_at_vcol(Doc *d, int line, int vcol, int tabw) {
    Line *l = &d->lines[line]; int v = 0, i = 0;
    while (i < l->len) {
        uint32_t cp; int k = utf8_decode(l->s + i, (size_t)(l->len - i), &cp);
        int w = cp == '\t' ? tabw - v % tabw : cp < 32 ? 1 : cp_width(cp);
        if (v + w > vcol && !(w == 1 && v >= vcol)) { if (v + w / 2 >= vcol || w == 1) return i; return i + k; }
        v += w; i += k;
        if (v >= vcol) return i;
    }
    return l->len;
}
int doc_indent_width(Doc *d, int line) { Line *l = &d->lines[line]; int i = 0; while (i < l->len && (l->s[i] == ' ' || l->s[i] == '\t')) i++; return i; }

/* ---------------------------------------------------------------- motion */
int doc_next_col(Doc *d, int line, int col) {
    Line *l = &d->lines[line]; if (col >= l->len) return -1;
    uint32_t cp; int k = utf8_decode(l->s + col, (size_t)(l->len - col), &cp); col += k;
    while (col < l->len && cp_width(({ uint32_t c2; utf8_decode(l->s + col, (size_t)(l->len - col), &c2); c2; })) == 0) { uint32_t c3; col += utf8_decode(l->s + col, (size_t)(l->len - col), &c3); }
    return col;
}
int doc_prev_col(Doc *d, int line, int col) {
    Line *l = &d->lines[line]; if (col <= 0) return -1;
    int p = utf8_prev(l->s, col);
    while (p > 0) { uint32_t cp; utf8_decode(l->s + p, (size_t)(l->len - p), &cp); if (cp_width(cp) != 0) break; p = utf8_prev(l->s, p); }
    return p;
}
static uint32_t cp_at(Doc *d, int line, int col) { uint32_t cp = 0; Line *l = &d->lines[line]; if (col < l->len) utf8_decode(l->s + col, (size_t)(l->len - col), &cp); return cp; }
static int cls_of(uint32_t cp) { return cp_is_word(cp) ? 1 : cp_is_space(cp) ? 0 : 2; }
Pos doc_word_left(Doc *d, Pos p) {
    for (;;) {
        if (p.col == 0) { if (p.line == 0) return p; p.line--; p.col = d->lines[p.line].len; return p; }
        int q = doc_prev_col(d, p.line, p.col); if (cls_of(cp_at(d, p.line, q)) != 0) break; p.col = q;
    }
    if (p.col == 0) return p;
    int c = cls_of(cp_at(d, p.line, doc_prev_col(d, p.line, p.col)));
    while (p.col > 0) { int q = doc_prev_col(d, p.line, p.col); if (cls_of(cp_at(d, p.line, q)) != c) break; p.col = q; }
    return p;
}
Pos doc_word_right(Doc *d, Pos p) {
    Line *l = &d->lines[p.line];
    if (p.col >= l->len) { if (p.line + 1 < d->nlines) return (Pos){ p.line + 1, 0 }; return p; }
    int c = cls_of(cp_at(d, p.line, p.col));
    while (p.col < l->len && cls_of(cp_at(d, p.line, p.col)) == c && c != 0) p.col = doc_next_col(d, p.line, p.col);
    while (p.col < l->len && cls_of(cp_at(d, p.line, p.col)) == 0) p.col = doc_next_col(d, p.line, p.col);
    return p;
}
bool doc_word_at(Doc *d, Pos p, int *a, int *b) {
    Line *l = &d->lines[p.line]; int c = p.col;
    if (c > l->len) c = l->len;
    if (!(c < l->len && cp_is_word(cp_at(d, p.line, c)))) { if (c > 0 && cp_is_word(cp_at(d, p.line, doc_prev_col(d, p.line, c)))) c = doc_prev_col(d, p.line, c); else return false; }
    int s = c, e = c;
    while (s > 0 && cp_is_word(cp_at(d, p.line, doc_prev_col(d, p.line, s)))) s = doc_prev_col(d, p.line, s);
    while (e < l->len && cp_is_word(cp_at(d, p.line, e))) e = doc_next_col(d, p.line, e);
    *a = s; *b = e; return true;
}
bool doc_match_bracket(Doc *d, Pos p, Pos *out) {
    static const char OPEN[] = "([{", CLOSE[] = ")]}";
    Line *l = &d->lines[p.line];
    for (int attempt = 0; attempt < 2; attempt++) {
        int col = attempt == 0 ? p.col : p.col - 1;
        if (col < 0 || col >= l->len) continue;
        char c = l->s[col]; const char *o = strchr(OPEN, c), *cl = strchr(CLOSE, c);
        if (c == 0 || (!o && !cl)) continue;
        doc_ensure_hl(d, p.line);
        if (!hl_is_code(l->hl[col])) continue;
        int dir = o ? 1 : -1; char want = o ? CLOSE[o - OPEN] : OPEN[cl - CLOSE]; int depth = 0;
        int line = p.line, i = col, scanned = 0;
        while (line >= 0 && line < d->nlines && scanned < 4000) {
            Line *L = &d->lines[line]; doc_ensure_hl(d, line);
            for (; i >= 0 && i < L->len; i += dir) {
                if (!hl_is_code(L->hl[i])) continue;
                char ch = L->s[i];
                if (ch == c) depth++;
                else if (ch == want) { if (--depth == 0) { *out = (Pos){ line, i }; return true; } }
            }
            line += dir; scanned++;
            if (line >= 0 && line < d->nlines) i = dir > 0 ? 0 : d->lines[line].len - 1;
        }
        return false;
    }
    return false;
}
