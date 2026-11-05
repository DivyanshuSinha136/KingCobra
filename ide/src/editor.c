/* kcide — editing commands, editor rendering, find/replace. */
#include "ide.h"
#include <ctype.h>

Doc *cur_doc(void) { return G.ndocs ? G.docs[G.active] : NULL; }

/* ---------------------------------------------------------------- helpers */
static int g_last_cmd = 0;
static void begin_cmd(Doc *d, int kind) { if (kind == 0 || kind != g_last_cmd) doc_break_undo(d); g_last_cmd = kind; }
static void sel_range(Doc *d, Pos *a, Pos *b) { if (pos_cmp(d->anchor, d->cur) <= 0) { *a = d->anchor; *b = d->cur; } else { *a = d->cur; *b = d->anchor; } }
static bool has_sel(Doc *d) { return d->has_sel && pos_cmp(d->anchor, d->cur) != 0; }
static void set_want(Doc *d) { d->want_vcol = doc_vcol(d, d->cur.line, d->cur.col, G.cfg.tab_width); }
static const char *indent_unit(void) { static char u[16]; if (G.cfg.use_spaces) { int n = G.cfg.tab_width; if (n > 15) n = 15; memset(u, ' ', (size_t)n); u[n] = 0; } else strcpy(u, "\t"); return u; }
static void comp_off(void) { if (G.comp.open) comp_close(&G.comp); }

void ed_reveal(Doc *d) {
    int th = G.text_h > 0 ? G.text_h : 1, tw = G.text_w > 4 ? G.text_w : 4;
    if (d->cur.line < d->top) d->top = d->cur.line;
    if (d->cur.line >= d->top + th) d->top = d->cur.line - th + 1;
    if (d->top < 0) d->top = 0;
    int v = doc_vcol(d, d->cur.line, d->cur.col, G.cfg.tab_width);
    if (v < d->left) d->left = v;
    if (v >= d->left + tw - 1) d->left = v - tw + 2;
    if (d->left < 0) d->left = 0;
}
void ed_set_cursor(Doc *d, Pos p, bool extend) {
    p = doc_clamp(d, p);
    if (extend) { if (!d->has_sel) { d->anchor = d->cur; d->has_sel = true; } } else d->has_sel = false;
    d->cur = p;
    if (!extend) d->anchor = p;
    ed_reveal(d);
}
void ed_goto(Doc *d, int line, int col, bool push) {
    if (push) nav_push();
    if (line < 0) line = 0; if (line >= d->nlines) line = d->nlines - 1;
    Pos p = doc_clamp(d, (Pos){ line, col < 0 ? 0 : col });
    d->has_sel = false; d->cur = d->anchor = p; set_want(d);
    if (p.line < d->top || p.line >= d->top + G.text_h) { d->top = p.line - G.text_h / 3; if (d->top < 0) d->top = 0; }
    ed_reveal(d);
}
void ed_delete_selection(Doc *d) {
    if (!has_sel(d)) { d->has_sel = false; return; }
    Pos a, b; sel_range(d, &a, &b); doc_delete(d, a, b); d->cur = d->anchor = a; d->has_sel = false; set_want(d);
}
char *ed_selected_text(Doc *d, int *n) { Pos a, b; sel_range(d, &a, &b); return doc_text(d, a, b, n); }

/* ---------------------------------------------------------------- typing */
static void after_edit(Doc *d, bool word_or_dot) {
    set_want(d); ed_reveal(d);
    if (!G.cfg.auto_complete || d->lang == LANG_PLAIN) { comp_off(); return; }
    if (word_or_dot || G.comp.open) comp_build(&G.comp, d, G.docs, G.ndocs, false);
}
static void insert_here(Doc *d, const char *t, int n) {
    if (has_sel(d)) ed_delete_selection(d);
    Pos e; doc_insert(d, d->cur, t, n, &e); d->cur = d->anchor = e; d->has_sel = false;
}
static bool in_string_or_comment(Doc *d, Pos p) {
    if (!d->is_kc) return false;
    doc_ensure_hl(d, p.line); Line *l = &d->lines[p.line];
    if (p.col > 0 && p.col <= l->len) return !hl_is_code(l->hl[p.col - 1]) && !(p.col < l->len && hl_is_code(l->hl[p.col]));
    return false;
}
void ed_type(Doc *d, const char *t, int n) {
    if (d->readonly) { status(2, "Read-only document"); return; }
    begin_cmd(d, 1);
    if (n == 1 && G.cfg.auto_pairs && d->lang != LANG_PLAIN) {
        char c = t[0]; Line *l = &d->lines[d->cur.line]; char next = d->cur.col < l->len ? l->s[d->cur.col] : 0, prev = d->cur.col > 0 ? l->s[d->cur.col - 1] : 0;
        const char *op = d->lang == LANG_SSM ? "\"" : "([{\"", *cl = d->lang == LANG_SSM ? "\"" : ")]}\"";
        const char *o = strchr(op, c);
        if (has_sel(d) && o && c != 0) {                                  /* wrap selection */
            Pos a, b; sel_range(d, &a, &b); int len; char *tx = doc_text(d, a, b, &len);
            if (!memchr(tx, '\n', (size_t)len)) {
                char close = cl[o - op]; doc_break_undo(d); doc_delete(d, a, b);
                Str s; str_init(&s); str_addc(&s, c); str_add(&s, tx, (size_t)len); str_addc(&s, close);
                Pos e; doc_insert(d, a, s.p, (int)s.n, &e); str_free(&s); free(tx);
                d->anchor = (Pos){ a.line, a.col + 1 }; d->cur = (Pos){ e.line, e.col - 1 }; d->has_sel = true; ed_reveal(d); return;
            }
            free(tx);
        }
        if ((c == ')' || c == ']' || c == '}' || c == '"') && next == c && !has_sel(d)) { d->cur.col++; d->anchor = d->cur; set_want(d); ed_reveal(d); return; }   /* type over */
        if (o && !has_sel(d)) {
            bool ok = !next || strchr(" \t)]};,", next);
            if (c == '"') ok = ok && !cp_is_word((unsigned char)prev) && prev != '\\' && !in_string_or_comment(d, d->cur);
            else ok = ok && !in_string_or_comment(d, d->cur);
            if (ok) { char two[2] = { c, cl[o - op] }; Pos e; doc_insert(d, d->cur, two, 2, &e); d->cur = d->anchor = (Pos){ e.line, e.col - 1 }; set_want(d); ed_reveal(d); after_edit(d, false); return; }
        }
        if (c == '>' && d->lang == LANG_SSM && !has_sel(d)) {              /* auto-close an opening SSML tag: <p> -> <p>|</p> */
            int lt = d->cur.col - 1; while (lt >= 0 && l->s[lt] != '<' && l->s[lt] != '>') lt--;
            if (lt >= 0 && l->s[lt] == '<' && lt + 1 < d->cur.col && l->s[lt + 1] != '/' && l->s[lt + 1] != '!' && l->s[lt + 1] != '?' && prev != '/') {
                int a = lt + 1, e = a; while (e < d->cur.col && (isalnum((unsigned char)l->s[e]) || l->s[e] == '-' || l->s[e] == '_' || l->s[e] == ':')) e++;
                if (e > a && !(e - a == 5 && !strncmp(l->s + a, "break", 5)) && !(e - a == 4 && !strncmp(l->s + a, "mark", 4))) {
                    Str t; str_init(&t); str_addc(&t, '>'); str_adds(&t, "</"); str_add(&t, l->s + a, (size_t)(e - a)); str_addc(&t, '>');
                    Pos st = d->cur, en; doc_insert(d, st, t.p, (int)t.n, &en); d->cur = d->anchor = (Pos){ st.line, st.col + 1 }; str_free(&t); set_want(d); ed_reveal(d); return;
                }
            }
        }
        if (c == '}' && !has_sel(d) && d->lang == LANG_KC) {                                    /* dedent a closing brace typed on a blank line */
            bool blank = true; for (int i = 0; i < d->cur.col; i++) if (l->s[i] != ' ' && l->s[i] != '\t') blank = false;
            if (blank && d->cur.col > 0) {
                int rm = G.cfg.use_spaces ? (d->cur.col < G.cfg.tab_width ? d->cur.col : G.cfg.tab_width) : 1;
                if (l->s[d->cur.col - 1] == ' ' && G.cfg.use_spaces) { int k = 0; while (k < rm && d->cur.col - k > 0 && l->s[d->cur.col - k - 1] == ' ') k++; rm = k; }
                doc_delete(d, (Pos){ d->cur.line, d->cur.col - rm }, d->cur); d->cur.col -= rm; d->anchor = d->cur;
            }
        }
    }
    bool word = n >= 1 && (cp_is_word((unsigned char)t[0]) || (unsigned char)t[0] >= 0x80);
    if (!word) doc_break_undo(d);
    insert_here(d, t, n);
    after_edit(d, word || (n == 1 && t[0] == '.'));
}
static void newline(Doc *d) {
    begin_cmd(d, 0);
    if (has_sel(d)) ed_delete_selection(d);
    Line *l = &d->lines[d->cur.line]; int ind = doc_indent_width(d, d->cur.line); if (ind > d->cur.col) ind = d->cur.col;
    Str s; str_init(&s); str_addc(&s, '\n'); str_add(&s, l->s, (size_t)ind);
    int k = d->cur.col; while (k > 0 && (l->s[k - 1] == ' ' || l->s[k - 1] == '\t')) k--;
    char before = k > 0 ? l->s[k - 1] : 0, after = d->cur.col < l->len ? l->s[d->cur.col] : 0;
    bool extra = d->is_kc && (before == '{' || before == '(' || before == '[');
    bool split = extra && ((before == '{' && after == '}') || (before == '(' && after == ')') || (before == '[' && after == ']'));
    if (extra) str_adds(&s, indent_unit());
    int cursor_off = (int)s.n;
    if (split) { str_addc(&s, '\n'); str_add(&s, l->s, (size_t)ind); }
    Pos start = d->cur, e; doc_insert(d, start, s.p, (int)s.n, &e);
    d->cur = d->anchor = pos_after(start, s.p, cursor_off); d->has_sel = false; str_free(&s);
    doc_break_undo(d); g_last_cmd = 0; comp_off(); set_want(d); ed_reveal(d);
}
static void backspace(Doc *d, bool word) {
    begin_cmd(d, 2);
    if (has_sel(d)) { ed_delete_selection(d); after_edit(d, false); return; }
    Pos p = d->cur; if (p.line == 0 && p.col == 0) return;
    Line *l = &d->lines[p.line]; Pos from;
    if (p.col == 0) { from = (Pos){ p.line - 1, d->lines[p.line - 1].len }; }
    else if (word) from = doc_word_left(d, p);
    else {
        bool lead = true; for (int i = 0; i < p.col; i++) if (l->s[i] != ' ') lead = false;
        if (lead && G.cfg.use_spaces && p.col > 0) { int stop = ((p.col - 1) / G.cfg.tab_width) * G.cfg.tab_width; from = (Pos){ p.line, stop }; }
        else {
            from = (Pos){ p.line, doc_prev_col(d, p.line, p.col) };
            char a = l->s[from.col], b = p.col < l->len ? l->s[p.col] : 0;
            if ((a == '(' && b == ')') || (a == '[' && b == ']') || (a == '{' && b == '}') || (a == '"' && b == '"')) p.col++;
        }
    }
    doc_delete(d, from, p); d->cur = d->anchor = from; set_want(d); ed_reveal(d); after_edit(d, false);
}
static void del_fwd(Doc *d, bool word) {
    begin_cmd(d, 3);
    if (has_sel(d)) { ed_delete_selection(d); return; }
    Pos p = d->cur; Line *l = &d->lines[p.line]; Pos to;
    if (p.col >= l->len) { if (p.line + 1 >= d->nlines) return; to = (Pos){ p.line + 1, 0 }; }
    else if (word) to = doc_word_right(d, p); else to = (Pos){ p.line, doc_next_col(d, p.line, p.col) };
    doc_delete(d, p, to); set_want(d); ed_reveal(d);
}

/* ---------------------------------------------------------------- clipboard / undo */
void ed_copy(Doc *d, bool cut) {
    int n; char *t;
    if (has_sel(d)) t = ed_selected_text(d, &n);
    else { Line *l = &d->lines[d->cur.line]; Str s; str_init(&s); str_add(&s, l->s, (size_t)l->len); str_addc(&s, '\n'); n = (int)s.n; t = str_take(&s); }
    free(G.clip); G.clip = t; G.cliplen = (size_t)n; term_clipboard_set(t, (size_t)n);
    if (cut && !d->readonly) {
        begin_cmd(d, 0);
        if (has_sel(d)) ed_delete_selection(d);
        else { int ln = d->cur.line; Pos a = { ln, 0 }, b = ln + 1 < d->nlines ? (Pos){ ln + 1, 0 } : (Pos){ ln, d->lines[ln].len };
               if (ln + 1 >= d->nlines && ln > 0) a = (Pos){ ln - 1, d->lines[ln - 1].len };
               doc_delete(d, a, b); d->cur = d->anchor = doc_clamp(d, (Pos){ a.line < d->nlines ? a.line : d->nlines - 1, 0 }); }
        set_want(d); ed_reveal(d);
    }
    status(0, cut ? "Cut" : "Copied");
}
void ed_paste_text(Doc *d, const char *t, int n) {
    if (d->readonly || n <= 0) return;
    begin_cmd(d, 0);
    bool whole_line = !has_sel(d) && n > 0 && t[n - 1] == '\n' && memchr(t, '\n', (size_t)n) == t + n - 1;   /* line copied without selection */
    if (whole_line) { Pos at = { d->cur.line, 0 }, e; doc_insert(d, at, t, n, &e); d->cur = d->anchor = (Pos){ d->cur.line + 1, d->cur.col > d->lines[d->cur.line + 1].len ? d->lines[d->cur.line + 1].len : d->cur.col }; }
    else insert_here(d, t, n);
    doc_break_undo(d); comp_off(); set_want(d); ed_reveal(d);
}
void ed_paste(Doc *d) { if (G.clip) ed_paste_text(d, G.clip, (int)G.cliplen); }
void ed_undo(Doc *d) { Pos c; comp_off(); if (doc_undo(d, &c)) { d->cur = d->anchor = doc_clamp(d, c); d->has_sel = false; set_want(d); ed_reveal(d); } else status(0, "Nothing to undo"); g_last_cmd = 0; }
void ed_redo(Doc *d) { Pos c; comp_off(); if (doc_redo(d, &c)) { d->cur = d->anchor = doc_clamp(d, c); d->has_sel = false; set_want(d); ed_reveal(d); } else status(0, "Nothing to redo"); g_last_cmd = 0; }
void ed_select_all(Doc *d) { d->anchor = (Pos){ 0, 0 }; d->cur = doc_end(d); d->has_sel = true; ed_reveal(d); }
void ed_select_line(Doc *d) { int ln = d->cur.line; d->anchor = (Pos){ ln, 0 }; d->cur = ln + 1 < d->nlines ? (Pos){ ln + 1, 0 } : (Pos){ ln, d->lines[ln].len }; d->has_sel = true; ed_reveal(d); }

/* ---------------------------------------------------------------- line operations */
static void line_block(Doc *d, int *a, int *b) {
    Pos p, q; if (has_sel(d)) sel_range(d, &p, &q); else p = q = d->cur;
    *a = p.line; *b = q.line; if (has_sel(d) && q.col == 0 && q.line > p.line) (*b)--;
}
void ed_dup_line(Doc *d) {
    int a, b; line_block(d, &a, &b); begin_cmd(d, 0);
    int n; char *t = doc_text(d, (Pos){ a, 0 }, (Pos){ b, d->lines[b].len }, &n);
    Str s; str_init(&s); str_addc(&s, '\n'); str_add(&s, t, (size_t)n); free(t);
    Pos e; doc_insert(d, (Pos){ b, d->lines[b].len }, s.p, (int)s.n, &e); str_free(&s);
    int off = b - a + 1; d->cur.line += off; d->anchor.line += off; set_want(d); ed_reveal(d);
}
void ed_del_line(Doc *d) {
    int a, b; line_block(d, &a, &b); begin_cmd(d, 0);
    Pos from = { a, 0 }, to = b + 1 < d->nlines ? (Pos){ b + 1, 0 } : (Pos){ b, d->lines[b].len };
    if (b + 1 >= d->nlines && a > 0) from = (Pos){ a - 1, d->lines[a - 1].len };
    doc_delete(d, from, to); int ln = a < d->nlines ? a : d->nlines - 1;
    d->cur = d->anchor = (Pos){ ln, 0 }; d->has_sel = false; set_want(d); ed_reveal(d);
}
void ed_move_lines(Doc *d, int dir) {
    int a, b; line_block(d, &a, &b);
    if ((dir < 0 && a == 0) || (dir > 0 && b + 1 >= d->nlines)) return;
    begin_cmd(d, 0);
    Pos c = d->cur, an = d->anchor;
    if (dir < 0) {
        int n; char *t = doc_text(d, (Pos){ a - 1, 0 }, (Pos){ a - 1, d->lines[a - 1].len }, &n);
        doc_delete(d, (Pos){ a - 1, 0 }, (Pos){ a, 0 });
        Str s; str_init(&s);
        if (b < d->nlines) { str_add(&s, t, (size_t)n); str_addc(&s, '\n'); Pos e; doc_insert(d, (Pos){ b, 0 }, s.p, (int)s.n, &e); }
        else { str_addc(&s, '\n'); str_add(&s, t, (size_t)n); Pos e; doc_insert(d, (Pos){ b - 1, d->lines[b - 1].len }, s.p, (int)s.n, &e); }
        str_free(&s); free(t); c.line--; an.line--;
    } else {
        int n; char *t = doc_text(d, (Pos){ b + 1, 0 }, (Pos){ b + 1, d->lines[b + 1].len }, &n);
        Pos from = { b, d->lines[b].len }, to = { b + 1, d->lines[b + 1].len };
        doc_delete(d, from, to);
        Str s; str_init(&s); str_add(&s, t, (size_t)n); str_addc(&s, '\n'); Pos e; doc_insert(d, (Pos){ a, 0 }, s.p, (int)s.n, &e); str_free(&s); free(t);
        c.line++; an.line++;
    }
    d->cur = doc_clamp(d, c); d->anchor = doc_clamp(d, an); ed_reveal(d);
}
void ed_toggle_comment(Doc *d) {
    int a, b; line_block(d, &a, &b); begin_cmd(d, 0);
    const char *pre = lang_comment(d->lang); int pl = pre ? (int)strlen(pre) : 0;
    if (d->lang == LANG_PLAIN) { pre = "# "; pl = 2; }
    bool all = true; int minind = 1 << 30, any = 0;
    for (int i = a; i <= b; i++) {
        Line *l = &d->lines[i]; int ind = doc_indent_width(d, i); if (ind == l->len) continue;
        any++; if (ind < minind) minind = ind;
        if (!pre) { int e = l->len; while (e > ind && (l->s[e - 1] == ' ' || l->s[e - 1] == '\r')) e--; if (!(e - ind >= 7 && !strncmp(l->s + ind, "<!--", 4) && !strncmp(l->s + e - 3, "-->", 3))) all = false; }
        else if (!(l->len - ind >= pl - 1 && !strncmp(l->s + ind, pre, (size_t)pl - 1))) all = false;
    }
    if (!any) return;
    for (int i = a; i <= b; i++) {
        Line *l = &d->lines[i]; int ind = doc_indent_width(d, i); if (ind == l->len) continue;
        if (!pre) {
            if (all) { int e = l->len; while (e > ind && (l->s[e - 1] == ' ' || l->s[e - 1] == '\r')) e--; int tail = (e - 4 > ind && l->s[e - 4] == ' ') ? 4 : 3; doc_delete(d, (Pos){ i, e - tail }, (Pos){ i, e }); int head = (ind + 4 < l->len && l->s[ind + 4] == ' ') ? 5 : 4; doc_delete(d, (Pos){ i, ind }, (Pos){ i, ind + head }); }
            else { Pos e; doc_insert(d, (Pos){ i, l->len }, " -->", 4, &e); doc_insert(d, (Pos){ i, minind }, "<!-- ", 5, &e); }
        }
        else if (all) { int rm = pl - 1; if (ind + rm < l->len && l->s[ind + rm] == ' ') rm++; doc_delete(d, (Pos){ i, ind }, (Pos){ i, ind + rm }); }
        else { Pos e; doc_insert(d, (Pos){ i, minind }, pre, pl, &e); }
    }
    d->cur = doc_clamp(d, d->cur); d->anchor = doc_clamp(d, d->anchor); set_want(d);
}
void ed_indent(Doc *d, bool out) {
    int a, b; line_block(d, &a, &b); begin_cmd(d, 0); const char *u = indent_unit(); int ul = (int)strlen(u);
    for (int i = a; i <= b; i++) {
        Line *l = &d->lines[i];
        if (!out) { if (l->len == 0) continue; Pos e; doc_insert(d, (Pos){ i, 0 }, u, ul, &e); if (d->cur.line == i) d->cur.col += ul; if (d->anchor.line == i) d->anchor.col += ul; }
        else {
            int rm = 0; if (l->len && l->s[0] == '\t') rm = 1; else while (rm < ul && rm < l->len && l->s[rm] == ' ') rm++;
            if (rm) { doc_delete(d, (Pos){ i, 0 }, (Pos){ i, rm }); if (d->cur.line == i) d->cur.col = d->cur.col > rm ? d->cur.col - rm : 0; if (d->anchor.line == i) d->anchor.col = d->anchor.col > rm ? d->anchor.col - rm : 0; }
        }
    }
    set_want(d); ed_reveal(d);
}
void ed_format(Doc *d) {
    if (!d->is_kc) { status(2, "Format works on .kc files"); return; }
    int a = 0, b = d->nlines - 1; if (has_sel(d)) line_block(d, &a, &b);
    begin_cmd(d, 0); const char *u = indent_unit(); int depth = 0, changed = 0;
    for (int i = 0; i < d->nlines; i++) {
        doc_ensure_hl(d, i); Line *l = &d->lines[i];
        bool protectedl = i > 0 && (d->lines[i - 1].state_out & 7) != HS_NORMAL;
        int ind = doc_indent_width(d, i);
        int lead = 0; if (!protectedl) for (int k = ind; k < l->len && strchr(")]}", l->s[k]) && hl_is_code(l->hl[k]); k++) lead++;
        int want_depth = depth - lead; if (want_depth < 0) want_depth = 0;
        if (!protectedl && i >= a && i <= b && l->len > ind) {
            Str want; str_init(&want); for (int k = 0; k < want_depth; k++) str_adds(&want, u);
            if ((int)want.n != ind || memcmp(want.p, l->s, want.n) != 0) {
                doc_delete(d, (Pos){ i, 0 }, (Pos){ i, ind }); Pos e; doc_insert(d, (Pos){ i, 0 }, want.p, (int)want.n, &e); changed++;
                if (d->cur.line == i) d->cur.col = d->cur.col >= ind ? d->cur.col - ind + (int)want.n : (int)want.n;
            }
            str_free(&want); l = &d->lines[i]; doc_ensure_hl(d, i);
        }
        if (i >= a && i <= b) { int e = l->len; while (e > 0 && (l->s[e - 1] == ' ' || l->s[e - 1] == '\t')) e--; if (e < l->len && !protectedl) { doc_delete(d, (Pos){ i, e }, (Pos){ i, l->len }); changed++; l = &d->lines[i]; doc_ensure_hl(d, i); } }
        for (int k = 0; k < l->len; k++) { if (!hl_is_code(l->hl[k])) continue; if (strchr("([{", l->s[k])) depth++; else if (strchr(")]}", l->s[k])) depth--; }
        if (depth < 0) depth = 0;
    }
    d->cur = doc_clamp(d, d->cur); d->anchor = doc_clamp(d, d->anchor); set_want(d); ed_reveal(d);
    status(1, changed ? "Formatted (%d line%s changed)" : "Already formatted", changed, changed == 1 ? "" : "s");
}

/* ---------------------------------------------------------------- completion accept */
static void comp_accept(Doc *d) {
    Comp *c = &G.comp; if (!c->open || c->sel >= c->n) return;
    CItem it = c->v[c->sel]; char *ins = xstrdup(it.insert); Pos start = c->start; CKind k = it.kind; comp_close(c);
    begin_cmd(d, 0);
    doc_delete(d, start, d->cur);
    if (k == CK_SNIPPET) {
        int ind = doc_indent_width(d, start.line); if (ind > start.col) ind = start.col;
        Str s; str_init(&s); int curoff = -1;
        for (char *p = ins; *p; p++) {
            if (*p == '$' && p[1] == '0') { curoff = (int)s.n; p++; }
            else if (*p == '\n') { str_addc(&s, '\n'); str_add(&s, d->lines[start.line].s, (size_t)ind); }
            else if (*p == '\t') str_adds(&s, indent_unit());
            else str_addc(&s, *p);
        }
        if (curoff < 0) curoff = (int)s.n;
        Pos e; doc_insert(d, start, s.p, (int)s.n, &e); d->cur = d->anchor = pos_after(start, s.p, curoff); str_free(&s);
    } else { Pos e; doc_insert(d, start, ins, (int)strlen(ins), &e); d->cur = d->anchor = e; }
    d->has_sel = false; free(ins); doc_break_undo(d); set_want(d); ed_reveal(d);
}
void ed_trigger_complete(bool manual) { Doc *d = cur_doc(); if (d) { comp_build(&G.comp, d, G.docs, G.ndocs, manual); if (!G.comp.open && manual) status(0, "No completions"); } }

/* ---------------------------------------------------------------- key handling */
static void move_to(Doc *d, Pos p, bool shift, bool keep_want) { ed_set_cursor(d, p, shift); if (!keep_want) set_want(d); }
bool ed_try_key(Doc *d, Event *e) {
    int k = e->key, m = e->mods; bool sh = m & MOD_SHIFT, ct = m & MOD_CTRL, al = m & MOD_ALT;
    if (G.comp.open) {
        Comp *c = &G.comp;
        if (k == K_DOWN && !m) { c->sel = (c->sel + 1) % c->n; return true; }
        if (k == K_UP && !m) { c->sel = (c->sel + c->n - 1) % c->n; return true; }
        if (k == K_PGDN) { c->sel = c->sel + 8 < c->n ? c->sel + 8 : c->n - 1; return true; }
        if (k == K_PGUP) { c->sel = c->sel > 8 ? c->sel - 8 : 0; return true; }
        if ((k == K_TAB || k == K_ENTER) && !m) { comp_accept(d); return true; }
        if (k == K_ESC) { comp_off(); return true; }
        if (k == K_LEFT || k == K_RIGHT || k == K_HOME || k == K_END || k == K_ENTER) comp_off();
    }
    if (k == K_ESC) { if (has_sel(d)) { d->has_sel = false; return true; } if (G.fs.open) { find_close(); return true; } return false; }
    if (ct || al) {
        if (k == K_LEFT && ct) { move_to(d, doc_word_left(d, d->cur), sh, false); return true; }
        if (k == K_RIGHT && ct) { move_to(d, doc_word_right(d, d->cur), sh, false); return true; }
        if (k == K_HOME && ct) { move_to(d, (Pos){ 0, 0 }, sh, false); return true; }
        if (k == K_END && ct) { move_to(d, doc_end(d), sh, false); return true; }
        if (k == K_BKSP && ct) { if (!d->readonly) backspace(d, true); return true; }
        if (k == K_DEL && ct) { if (!d->readonly) del_fwd(d, true); return true; }
        if (k == K_UP && ct) { if (d->top > 0) d->top--; return true; }
        if (k == K_DOWN && ct) { if (d->top + 1 < d->nlines) d->top++; return true; }
        return false;
    }
    switch (k) {
    case K_LEFT:
        if (has_sel(d) && !sh) { Pos a, b; sel_range(d, &a, &b); move_to(d, a, false, false); }
        else { Pos p = d->cur; int c = doc_prev_col(d, p.line, p.col); if (c >= 0) p.col = c; else if (p.line > 0) { p.line--; p.col = d->lines[p.line].len; } move_to(d, p, sh, false); }
        return true;
    case K_RIGHT:
        if (has_sel(d) && !sh) { Pos a, b; sel_range(d, &a, &b); move_to(d, b, false, false); }
        else { Pos p = d->cur; int c = doc_next_col(d, p.line, p.col); if (c >= 0) p.col = c; else if (p.line + 1 < d->nlines) { p.line++; p.col = 0; } move_to(d, p, sh, false); }
        return true;
    case K_UP: case K_DOWN: {
        Pos p = d->cur; int dl = k == K_UP ? -1 : 1;
        if (p.line + dl < 0) { move_to(d, (Pos){ 0, 0 }, sh, false); return true; }
        if (p.line + dl >= d->nlines) { move_to(d, doc_end(d), sh, false); return true; }
        p.line += dl; p.col = doc_col_at_vcol(d, p.line, d->want_vcol, G.cfg.tab_width); move_to(d, p, sh, true); return true; }
    case K_PGUP: case K_PGDN: {
        int dl = (k == K_PGUP ? -1 : 1) * (G.text_h > 2 ? G.text_h - 1 : 1); Pos p = d->cur; p.line += dl;
        if (p.line < 0) p.line = 0; if (p.line >= d->nlines) p.line = d->nlines - 1;
        d->top += dl; if (d->top < 0) d->top = 0; if (d->top >= d->nlines) d->top = d->nlines - 1;
        p.col = doc_col_at_vcol(d, p.line, d->want_vcol, G.cfg.tab_width); move_to(d, p, sh, true); return true; }
    case K_HOME: { Pos p = d->cur; int ind = doc_indent_width(d, p.line); p.col = (p.col == ind || p.col > 0) && p.col != ind ? ind : 0; if (p.col == 0 && d->cur.col == 0) p.col = ind; move_to(d, p, sh, false); return true; }
    case K_END: move_to(d, (Pos){ d->cur.line, d->lines[d->cur.line].len }, sh, false); return true;
    case K_BKSP: if (!d->readonly) backspace(d, false); return true;
    case K_DEL: if (!d->readonly) del_fwd(d, false); return true;
    case K_ENTER: if (!d->readonly) newline(d); return true;
    case K_TAB: {
        if (d->readonly) return true;
        if (sh) { ed_indent(d, true); return true; }
        if (has_sel(d)) { Pos a, b; sel_range(d, &a, &b); if (a.line != b.line) { ed_indent(d, false); return true; } }
        begin_cmd(d, 0);
        Line *l = &d->lines[d->cur.line]; int s = d->cur.col; while (s > 0 && cp_is_word((unsigned char)l->s[s - 1])) s--;
        const char *body = (d->is_kc && s < d->cur.col && !has_sel(d)) ? snippet_lookup(l->s + s, d->cur.col - s) : NULL;
        if (body) { comp_close(&G.comp); G.comp.start = (Pos){ d->cur.line, s }; G.comp.n = 0;
            G.comp.v = xrealloc(G.comp.v, sizeof(CItem) * (size_t)(G.comp.cap > 0 ? G.comp.cap : 1)); if (!G.comp.cap) G.comp.cap = 1;
            G.comp.v[0] = (CItem){ xstrdup("snip"), xstrdup(body), CK_SNIPPET, 0 }; G.comp.n = 1; G.comp.open = true; G.comp.sel = 0; comp_accept(d); return true; }
        if (G.cfg.use_spaces) { int v = doc_vcol(d, d->cur.line, d->cur.col, G.cfg.tab_width); int n = G.cfg.tab_width - v % G.cfg.tab_width; char sp[16]; memset(sp, ' ', (size_t)n); insert_here(d, sp, n); }
        else insert_here(d, "\t", 1);
        doc_break_undo(d); set_want(d); ed_reveal(d); return true; }
    default: break;
    }
    if (k >= 32 && k < 0x200000 && !ct && !al) {
        if (d->readonly) { status(2, "Read-only document"); return true; }
        char b[8]; int n = utf8_encode((uint32_t)((sh && k >= 'a' && k <= 'z') ? k - 32 : k), b);
        /* terminals deliver the shifted character itself for text keys; keep it as-is */
        if (sh && k >= 'a' && k <= 'z') n = utf8_encode((uint32_t)k, b);
        ed_type(d, b, n); return true;
    }
    return false;
}

/* ---------------------------------------------------------------- find / replace */
static bool match_at(const char *s, int len, int i, const char *f, int fl, bool cs, bool whole) {
    if (i + fl > len) return false;
    for (int k = 0; k < fl; k++) { char a = s[i + k], b = f[k]; if (cs ? a != b : tolower((unsigned char)a) != tolower((unsigned char)b)) return false; }
    if (whole) { if (i > 0 && cp_is_word((unsigned char)s[i - 1])) return false; if (i + fl < len && cp_is_word((unsigned char)s[i + fl])) return false; }
    return true;
}
int ed_find_matches_in_line(Doc *d, int line, int *st, int *ln, int max) {
    int fl = (int)strlen(G.fs.find); if (!G.fs.open || fl == 0) return 0;
    Line *l = &d->lines[line]; int n = 0;
    for (int i = 0; i + fl <= l->len && n < max; i++) if (match_at(l->s, l->len, i, G.fs.find, fl, G.fs.case_sens, G.fs.whole)) { st[n] = i; ln[n] = fl; n++; i += fl - 1; }
    return n;
}
void find_recount(void) {
    Doc *d = cur_doc(); G.fs.count = G.fs.index = 0; if (!d || !G.fs.find[0]) return;
    int st[64], ln[64]; Pos a, b; sel_range(d, &a, &b);
    for (int i = 0; i < d->nlines && G.fs.count < 100000; i++) {
        int n = ed_find_matches_in_line(d, i, st, ln, 64);
        for (int k = 0; k < n; k++) { G.fs.count++; if (has_sel(d) && a.line == i && a.col == st[k]) G.fs.index = G.fs.count; }
    }
}
void find_step(int dir, bool from_cursor) {
    Doc *d = cur_doc(); if (!d || !G.fs.find[0]) return;
    int st[64], ln[64]; Pos a, b; sel_range(d, &a, &b);
    Pos start = dir > 0 ? (from_cursor && has_sel(d) ? a : b) : a; if (!has_sel(d)) start = d->cur;
    if (dir > 0 && !from_cursor && !has_sel(d)) start = d->cur;
    for (int pass = 0; pass < 2; pass++) {
        for (int step = 0; step <= d->nlines; step++) {
            int line = dir > 0 ? start.line + step : start.line - step; if (line < 0 || line >= d->nlines) break;
            int n = ed_find_matches_in_line(d, line, st, ln, 64);
            for (int k = dir > 0 ? 0 : n - 1; k >= 0 && k < n; k += dir) {
                Pos p = { line, st[k] };
                bool ok = dir > 0 ? (step > 0 || pos_cmp(p, start) >= 0 || pass == 1) : (step > 0 || pos_cmp(p, start) < 0 || pass == 1);
                if (pass == 1 && ((dir > 0 && p.line > start.line) || (dir < 0 && p.line < start.line))) ok = false;
                if (ok) { d->anchor = p; d->cur = (Pos){ line, st[k] + ln[k] }; d->has_sel = true; set_want(d); ed_reveal(d);
                    if (d->cur.line < d->top + 1 || d->cur.line > d->top + G.text_h - 2) { d->top = d->cur.line - G.text_h / 2; if (d->top < 0) d->top = 0; }
                    find_recount(); return; }
            }
        }
        start = dir > 0 ? (Pos){ 0, 0 } : doc_end(d);
        if (pass == 0) { start = dir > 0 ? (Pos){ 0, 0 } : doc_end(d); }
    }
    status(2, "No matches for \"%s\"", G.fs.find); G.fs.count = G.fs.index = 0;
}
void find_open(bool replace) {
    Doc *d = cur_doc(); if (!d) return;
    if (has_sel(d)) { int n; char *t = ed_selected_text(d, &n); if (!memchr(t, '\n', (size_t)n) && n < 200) { memcpy(G.fs.find, t, (size_t)n); G.fs.find[n] = 0; } free(t); }
    G.fs.open = true; G.fs.replace_mode = replace; G.fs.field = replace && G.fs.find[0] ? 1 : 0; ui_layout(); find_recount();
}
void find_close(void) { G.fs.open = false; G.fs.field = -1; ui_layout(); }
void find_replace_one(void) {
    Doc *d = cur_doc(); if (!d || d->readonly) return;
    int fl = (int)strlen(G.fs.find); if (!fl) return;
    if (has_sel(d)) { Pos a, b; sel_range(d, &a, &b); Line *l = &d->lines[a.line];
        if (a.line == b.line && b.col - a.col == fl && match_at(l->s, l->len, a.col, G.fs.find, fl, G.fs.case_sens, G.fs.whole)) {
            begin_cmd(d, 0); doc_delete(d, a, b); Pos e; doc_insert(d, a, G.fs.repl, (int)strlen(G.fs.repl), &e); d->cur = d->anchor = e; d->has_sel = false; doc_break_undo(d); } }
    find_step(1, false);
}
void find_replace_all(void) {
    Doc *d = cur_doc(); if (!d || d->readonly) return; int fl = (int)strlen(G.fs.find), rl = (int)strlen(G.fs.repl); if (!fl) return;
    begin_cmd(d, 0); int total = 0, st[64], ln[64];
    for (int i = 0; i < d->nlines; i++) {
        int n = ed_find_matches_in_line(d, i, st, ln, 64);
        for (int k = n - 1; k >= 0; k--) { doc_delete(d, (Pos){ i, st[k] }, (Pos){ i, st[k] + fl }); Pos e; doc_insert(d, (Pos){ i, st[k] }, G.fs.repl, rl, &e); total++; }
    }
    doc_break_undo(d); d->cur = doc_clamp(d, d->cur); d->anchor = d->cur; d->has_sel = false; find_recount();
    status(1, "Replaced %d occurrence%s", total, total == 1 ? "" : "s");
}
bool find_key(Event *e) {
    if (!G.fs.open || G.fs.field < 0) return false;
    char *buf = G.fs.field == 0 ? G.fs.find : G.fs.repl; size_t cap = 255; int len = (int)strlen(buf);
    int k = e->key, m = e->mods;
    if (m & MOD_ALT) {
        if (k == 'c') { G.fs.case_sens = !G.fs.case_sens; find_recount(); return true; }
        if (k == 'w') { G.fs.whole = !G.fs.whole; find_recount(); return true; }
        if (k == 'a') { find_replace_all(); return true; }
        return false;
    }
    if (m & MOD_CTRL) return false;
    if (k == K_ESC) { find_close(); return true; }
    if (k == K_TAB) { if (G.fs.replace_mode) G.fs.field = 1 - G.fs.field; return true; }
    if (k == K_ENTER) { if (G.fs.field == 1) find_replace_one(); else find_step(1, false); return true; }
    if (k == K_DOWN) { find_step(1, false); return true; }
    if (k == K_UP) { find_step(-1, false); return true; }
    if (k == K_BKSP) { if (len > 0) { int p = utf8_prev(buf, len); buf[p] = 0; if (G.fs.field == 0) { find_recount(); find_step(1, true); } } return true; }
    if (k >= 32 && k < 0x200000) { char b[8]; int n = utf8_encode((uint32_t)k, b); if ((size_t)(len + n) < cap) { memcpy(buf + len, b, (size_t)n); buf[len + n] = 0; if (G.fs.field == 0) { Doc *d = cur_doc(); if (d && has_sel(d)) { Pos a, bb; sel_range(d, &a, &bb); d->cur = d->anchor = a; d->has_sel = false; } find_step(1, true); } } return true; }
    return k == K_LEFT || k == K_RIGHT || k == K_HOME || k == K_END || k == K_DEL;
}
