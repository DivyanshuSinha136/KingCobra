/* kcide — editor rendering (gutter, syntax colours, selection, diagnostics, scrollbar, completion popup,
 * find bar) and mouse handling. */
#include "ide.h"
#include <ctype.h>

bool diag_for_line(Doc *d, int line, int *sev, const char **msg, int *col) {
    bool found = false;
    if (!d->path) return false;
    for (int i = 0; i < G.out.ndiag; i++) {
        Diag *g = &G.out.diags[i];
        if (g->sev > 1 || g->line - 1 != line || !path_eq(d->path, g->file)) continue;
        if (!found || g->sev < *sev) { *sev = g->sev; *msg = g->msg; *col = g->col; found = true; }
    }
    return found;
}
int ed_line_diag_cols(Doc *d, int line, int *cols, int *sevs, int max) {
    int n = 0; if (!d->path) return 0;
    for (int i = 0; i < G.out.ndiag && n < max; i++) {
        Diag *g = &G.out.diags[i];
        if (g->sev < 2 && g->line - 1 == line && path_eq(d->path, g->file)) { cols[n] = g->col > 0 ? g->col - 1 : 0; sevs[n] = g->sev; n++; }
    }
    return n;
}

static int digits(int n) { int d = 1; while (n >= 10) { n /= 10; d++; } return d; }

static void draw_find_bar(void) {
    Rect r = G.r_find; if (!G.fs.open || r.h <= 0) return;
    Style bg = ST(TH.ui_fg, TH.menu_bg), dim = ST(TH.ui_dim, TH.menu_bg);
    scr_fill(r.x, r.y, r.w, r.h, bg);
    int rows = G.fs.replace_mode ? 2 : 1;
    for (int row = 0; row < rows; row++) {
        int y = r.y + row; int x = r.x + 1;
        x += scr_text(x, y, row == 0 ? "Find:    " : "Replace: ", 12, ST(TH.accent, TH.menu_bg));
        int w = r.w - 34 > 10 ? r.w - 34 : 10; if (w > 60) w = 60;
        const char *txt = row == 0 ? G.fs.find : G.fs.repl;
        bool active = G.fs.field == row;
        scr_fill(x, y, w, 1, ST(TH.fg, active ? TH.bg : TH.line_bg));
        int tl = (int)strlen(txt), shown = tl; const char *p = txt;
        while (shown > 0) { int cols = 0, i = 0; while (i < shown) { uint32_t cp; i += utf8_decode(p + i, (size_t)(shown - i), &cp); cols += cp_width(cp); } if (cols < w - 1) break; int k = 1; while (((unsigned char)p[k] & 0xC0) == 0x80) k++; p += k; shown -= k; }
        int used = scr_textn(x, y, p, shown, w - 1, ST(TH.fg, active ? TH.bg : TH.line_bg));
        if (active) scr_cursor(x + used, y, true);
        if (row == 0) {
            char info[96]; snprintf(info, sizeof info, " %s%s%s  %s", G.fs.case_sens ? "[Aa]" : " Aa ", G.fs.whole ? "[W]" : " W ", "", G.fs.count ? "" : (G.fs.find[0] ? "no results" : ""));
            int ix = x + w + 1; ix += scr_text(ix, y, info, r.w, dim);
            if (G.fs.count) { char c[40]; snprintf(c, sizeof c, "%d of %d", G.fs.index, G.fs.count); scr_text(ix, y, c, 20, ST(TH.info, TH.menu_bg)); }
        } else scr_text(x + w + 2, y, "Enter: replace   Alt+A: all", 40, dim);
    }
}

static void draw_popup(Doc *d, int sx, int sy) {
    Comp *c = &G.comp; if (!c->open || c->n == 0) return;
    int rows = c->n < 10 ? c->n : 10, w = 20;
    for (int i = 0; i < c->n && i < 200; i++) { int l = (int)strlen(c->v[i].label) + 8; if (l > w) w = l; }
    if (w > 54) w = 54;
    if (c->sel < c->scroll) c->scroll = c->sel; if (c->sel >= c->scroll + rows) c->scroll = c->sel - rows + 1;
    int x = sx, y = sy + 1; if (x + w > term_w) x = term_w - w; if (x < 0) x = 0;
    if (y + rows > G.r_edit.y + G.r_edit.h) y = sy - rows; if (y < 0) y = 0;
    (void)d;
    for (int i = 0; i < rows; i++) {
        CItem *it = &c->v[c->scroll + i]; bool sel = c->scroll + i == c->sel;
        Color bg = sel ? TH.popup_sel : TH.popup_bg;
        scr_fill(x, y + i, w, 1, ST(TH.fg, bg));
        Color tag = it->kind == CK_KEYWORD ? TH.hl[HC_KEYWORD] : it->kind == CK_TYPE ? TH.hl[HC_TYPE] : it->kind == CK_BUILTIN || it->kind == CK_FUNC ? TH.hl[HC_BUILTIN] :
                    it->kind == CK_MODULE ? TH.hl[HC_MODULE] : it->kind == CK_SNIPPET ? TH.warn : it->kind == CK_MEMBER ? TH.hl[HC_FUNC] : it->kind == CK_SYMBOL ? TH.hl[HC_FUNCDEF] : TH.ui_dim;
        scr_text(x + 1, y + i, comp_kind_tag(it->kind), 5, ST(tag, bg));
        scr_text(x + 7, y + i, it->label, w - 8, STA(TH.fg, bg, sel ? ATTR_BOLD : 0));
    }
    if (c->n > rows) { char b[24]; snprintf(b, sizeof b, "%d/%d", c->sel + 1, c->n); scr_text(x + w - (int)strlen(b) - 1, y + rows - 1, b, 10, ST(TH.ui_dim, c->sel == c->scroll + rows - 1 ? TH.popup_sel : TH.popup_bg)); }
}

void ed_draw(Doc *d) {
    Rect r = G.r_edit; int tw = G.cfg.tab_width;
    if (!d) {
        scr_fill(r.x, r.y, r.w, r.h, ST(TH.fg, TH.bg));
        int cy = r.y + r.h / 2 - 4; const char *L[] = { "KingCobra IDE", "", "Ctrl+O  open file        Ctrl+P  quick open", "Ctrl+N  new file         F1      command palette", "F5      run              F6      check", "Ctrl+B  sidebar          Ctrl+Q  quit" };
        for (int i = 0; i < 6; i++) { int len = (int)strlen(L[i]); scr_text(r.x + (r.w - len) / 2, cy + i, L[i], r.w, i == 0 ? STA(TH.accent, TH.bg, ATTR_BOLD) : ST(TH.ui_dim, TH.bg)); }
        return;
    }
    int nd = digits(d->nlines); if (nd < 3) nd = 3;
    G.gutter_w = G.cfg.line_numbers ? nd + 3 : 2; G.text_x = r.x + G.gutter_w; G.text_w = r.w - G.gutter_w - 1; G.text_h = r.h;
    if (G.text_w < 4) G.text_w = 4;
    scr_fill(r.x, r.y, r.w, r.h, ST(TH.fg, TH.bg));
    Pos sa, sb; bool sel = d->has_sel && pos_cmp(d->anchor, d->cur) != 0;
    if (sel) { if (pos_cmp(d->anchor, d->cur) <= 0) { sa = d->anchor; sb = d->cur; } else { sa = d->cur; sb = d->anchor; } } else sa = sb = (Pos){ -1, -1 };
    Pos m1 = { -1, -1 }, m2 = { -1, -1 };
    if (doc_match_bracket(d, d->cur, &m2)) { Line *cl = &d->lines[d->cur.line]; m1 = d->cur; if (!(d->cur.col < cl->len && strchr("()[]{}", cl->s[d->cur.col]) && hl_is_code(doc_hl(d, d->cur.line)[d->cur.col]))) m1.col--; }
    char word[96] = ""; { int a, b; if (!sel && doc_word_at(d, d->cur, &a, &b) && b - a >= 2 && b - a < 95 && !isdigit((unsigned char)d->lines[d->cur.line].s[a])) { memcpy(word, d->lines[d->cur.line].s + a, (size_t)(b - a)); word[b - a] = 0; } }
    int wl = (int)strlen(word);
    doc_ensure_hl(d, d->top + r.h);
    int cursor_sx = -1, cursor_sy = -1;
    for (int row = 0; row < r.h; row++) {
        int ln = d->top + row, y = r.y + row; if (ln >= d->nlines) break;
        Line *L = &d->lines[ln]; bool curline = ln == d->cur.line;
        int exl = -1; bool executing = dbg_exec_line(d, &exl) && exl == ln;
        Color base = executing ? (!strcmp(G.dbg.reason, "exception") ? 0x4d2626 : 0x4a4416) : (curline && !sel ? TH.line_bg : TH.bg);
        if (executing && TH.bg > 0xc00000) base = !strcmp(G.dbg.reason, "exception") ? 0xf6c9c9 : 0xfff0a0;
        scr_fill(G.text_x, y, G.text_w, 1, ST(TH.fg, base));
        /* gutter */
        int dsev = 0, dcol = 0; const char *dmsg = NULL; bool hasd = diag_for_line(d, ln, &dsev, &dmsg, &dcol);
        scr_fill(r.x, y, G.gutter_w, 1, ST(TH.gutter_fg, TH.gutter_bg));
        if (hasd) scr_putc(r.x, y, 0x25CF, ST(dsev == 0 ? TH.error : TH.warn, TH.gutter_bg));
        if (L->flags & LF_BREAK) scr_putc(r.x, y, 0x25CF, STA(0xff4d4d, TH.gutter_bg, ATTR_BOLD));
        if (executing) scr_putc(r.x, y, 0x25B6, STA(TH.warn, TH.gutter_bg, ATTR_BOLD));
        if (G.cfg.line_numbers) { char nb[16]; snprintf(nb, sizeof nb, "%*d", nd, ln + 1); scr_text(r.x + 1, y, nb, nd, STA(curline ? TH.gutter_cur : TH.gutter_fg, TH.gutter_bg, curline ? ATTR_BOLD : 0)); }
        if (L->flags & LF_BREAK) { for (int q = 1; q <= nd; q++) { Cell *c = scr_cell(r.x + q, y); if (c) { c->bg = 0x8c1f1f; c->fg = 0xffffff; } } }
        if (L->flags & LF_CHANGED) scr_putc(r.x + G.gutter_w - 1, y, 0x258E, ST(TH.warn, TH.gutter_bg));
        /* text */
        const uint8_t *hl = L->hl; int fm_s[64], fm_l[64]; int nfm = ed_find_matches_in_line(d, ln, fm_s, fm_l, 64);
        int dc[8], ds[8], ndc = ed_line_diag_cols(d, ln, dc, ds, 8);
        int ind = doc_indent_width(d, ln), vx = 0, i = 0;
        Pos cp_cursor = d->cur;
        while (i <= L->len) {
            if (ln == cp_cursor.line && i == cp_cursor.col) { cursor_sx = G.text_x + vx - d->left; cursor_sy = y; }
            if (i >= L->len) break;
            uint32_t cp; int k = utf8_decode(L->s + i, (size_t)(L->len - i), &cp);
            int w = cp == '\t' ? tw - vx % tw : cp < 32 ? 1 : cp_width(cp);
            uint8_t cls = hl[i];
            Color fg = TH.hl[cls], bg = base; uint8_t at = TH.hl_attr[cls];
            Pos here = { ln, i };
            bool insel = sel && pos_cmp(here, sa) >= 0 && pos_cmp(here, sb) < 0;
            if (insel) bg = TH.sel_bg;
            else {
                for (int f = 0; f < nfm; f++) if (i >= fm_s[f] && i < fm_s[f] + fm_l[f]) { bg = (sel == false && false) ? TH.find_cur_bg : TH.find_bg; break; }
                if (bg == base && ((here.line == m1.line && here.col == m1.col) || (here.line == m2.line && here.col == m2.col)) && m1.line >= 0) { bg = TH.bracket_bg; at |= ATTR_BOLD; }
                if (bg == base && wl && i + wl <= L->len && hl_is_code(cls) && !memcmp(L->s + i, word, (size_t)wl) && (i == 0 || !cp_is_word((unsigned char)L->s[i - 1])) && (i + wl == L->len || !cp_is_word((unsigned char)L->s[i + wl]))) bg = TH.occurrence_bg;
                if (bg == base && wl && i > 0 && i < L->len && cp_is_word((unsigned char)L->s[i]) && i < L->len) { /* continue occurrence highlight over the rest of the word */
                    int s0 = i; while (s0 > 0 && cp_is_word((unsigned char)L->s[s0 - 1])) s0--;
                    if (s0 + wl <= L->len && !memcmp(L->s + s0, word, (size_t)wl) && (s0 + wl == L->len || !cp_is_word((unsigned char)L->s[s0 + wl])) && i < s0 + wl && hl_is_code(cls)) bg = TH.occurrence_bg; }
            }
            for (int q = 0; q < ndc; q++) { int e = dc[q]; int ee = e; while (ee < L->len && cp_is_word((unsigned char)L->s[ee])) ee++; if (ee == e) ee = e + 1; if (i >= e && i < ee) { at |= ATTR_UNDERLINE; fg = ds[q] == 0 ? TH.error : TH.warn; } }
            int sx = vx - d->left;
            if (cp == '\t') {
                for (int q = 0; q < w; q++) { int x = sx + q; if (x < 0 || x >= G.text_w) continue; bool guide = (vx + q) % tw == 0 && vx + q > 0 && i < ind;
                    if (G.cfg.show_ws && q == 0) scr_putc(G.text_x + x, y, 0x2192, ST(TH.guide, bg)); else if (guide) scr_putc(G.text_x + x, y, 0x2502, ST(TH.guide, bg)); else scr_putc(G.text_x + x, y, ' ', ST(fg, bg)); }
            } else if (w > 0) {
                if (sx >= 0 && sx + w <= G.text_w) {
                    uint32_t ch = cp;
                    if (cp < 32) ch = 0xB7;
                    Color cfg = fg;
                    if (cp == ' ') {
                        if (i < ind && vx % tw == 0 && vx > 0 && !insel) { ch = 0x2502; cfg = TH.guide; }
                        else if (G.cfg.show_ws) { ch = 0xB7; cfg = TH.guide; }
                    }
                    scr_putc(G.text_x + sx, y, ch, STA(cfg, bg, at)); if (w == 2) scr_putc(G.text_x + sx + 1, y, 0, ST(cfg, bg));
                }
            }
            vx += w; i += k;
        }
        if (sel && ((ln >= sa.line && ln < sb.line))) { int x = vx - d->left; if (x >= 0 && x < G.text_w) scr_putc(G.text_x + x, y, ' ', ST(TH.fg, TH.sel_bg)); }
        /* inline diagnostic message at end of line */
        if (hasd && dmsg) { int x = vx - d->left + 3; if (x < 0) x = 0; if (x < G.text_w - 6) { char b[200]; snprintf(b, sizeof b, "\xe2\x96\xb8 %s", dmsg); scr_text(G.text_x + x, y, b, G.text_w - x, STA(dsev == 0 ? TH.error : TH.warn, base, ATTR_ITALIC)); } }
    }
    /* scrollbar */
    {
        int x = r.x + r.w - 1; scr_fill(x, r.y, 1, r.h, ST(TH.border, TH.bg));
        if (d->nlines > r.h) {
            int th = r.h * r.h / d->nlines; if (th < 1) th = 1; int ty = (int)((long)d->top * (r.h - th) / (d->nlines - r.h > 0 ? d->nlines - r.h : 1)); if (ty > r.h - th) ty = r.h - th;
            for (int i = 0; i < th; i++) scr_putc(x, r.y + ty + i, 0x2588, ST(TH.ui_dim, TH.bg));
        }
        for (int i = 0; i < G.out.ndiag && d->path; i++) { Diag *g = &G.out.diags[i]; if (g->sev > 1 || !path_eq(d->path, g->file)) continue; int yy = d->nlines > 1 ? (int)((long)(g->line - 1) * (r.h - 1) / (d->nlines - 1)) : 0; if (yy >= 0 && yy < r.h) scr_putc(x, r.y + yy, 0x25AA, ST(g->sev == 0 ? TH.error : TH.warn, TH.bg)); }
    }
    draw_find_bar();
    if (cursor_sx >= G.text_x && cursor_sx < G.text_x + G.text_w && G.focus == FOCUS_EDITOR && !(G.fs.open && G.fs.field >= 0) && G.ov.kind == OV_NONE) scr_cursor(cursor_sx, cursor_sy, true);
    if (cursor_sx >= 0) draw_popup(d, cursor_sx - (d->cur.col - G.comp.start.col > 0 ? doc_vcol(d, d->cur.line, d->cur.col, tw) - doc_vcol(d, d->cur.line, G.comp.start.col, tw) : 0), cursor_sy);
}

/* ---------------------------------------------------------------- mouse */
static Pos mouse_pos(Doc *d, int mx, int my) {
    int line = d->top + (my - G.r_edit.y); if (line < 0) line = 0; if (line >= d->nlines) line = d->nlines - 1;
    int vcol = mx - G.text_x + d->left; if (vcol < 0) vcol = 0;
    return (Pos){ line, doc_col_at_vcol(d, line, vcol, G.cfg.tab_width) };
}
void ed_mouse(Doc *d, Event *e) {
    if (!d) return;
    if (e->btn == 4 || e->btn == 5) {
        int dl = e->btn == 4 ? -3 : 3;
        if (e->mods & MOD_SHIFT) { d->left += dl * 2; if (d->left < 0) d->left = 0; }
        else { d->top += dl; if (d->top < 0) d->top = 0; if (d->top > d->nlines - 1) d->top = d->nlines - 1; }
        return;
    }
    if (e->btn != 0 && !(e->drag && e->btn == 3)) return;
    int sbx = G.r_edit.x + G.r_edit.w - 1;
    if (e->press && !e->drag && e->mx == sbx && d->nlines > G.r_edit.h) { G.drag_scroll = true; }
    if (G.drag_scroll) {
        if (e->release) { G.drag_scroll = false; return; }
        int rel = e->my - G.r_edit.y; if (rel < 0) rel = 0; if (rel >= G.r_edit.h) rel = G.r_edit.h - 1;
        d->top = (int)((long)rel * (d->nlines - G.r_edit.h) / (G.r_edit.h > 1 ? G.r_edit.h - 1 : 1)); if (d->top < 0) d->top = 0; return;
    }
    if (e->press && !e->drag) {
        G.focus = FOCUS_EDITOR; if (G.fs.open) G.fs.field = -1; comp_close(&G.comp);
        uint64_t t = now_ms();
        if (t - G.last_click < 450 && G.last_cx == e->mx && G.last_cy == e->my) G.click_count++; else G.click_count = 1;
        G.last_click = t; G.last_cx = e->mx; G.last_cy = e->my;
        if (e->mx < G.text_x) {                                   /* gutter */
            int line = d->top + (e->my - G.r_edit.y); if (line >= d->nlines) return;
            int sev, col; const char *msg;
            if (e->mx > G.r_edit.x && e->mx < G.text_x - 1 && G.cfg.line_numbers) { dbg_toggle_bp(d, line); return; }     /* click the line number: breakpoint */
            if (e->mx == G.r_edit.x && diag_for_line(d, line, &sev, &msg, &col)) status(sev == 0 ? 3 : 2, "%s", msg);
            d->anchor = (Pos){ line, 0 }; d->cur = line + 1 < d->nlines ? (Pos){ line + 1, 0 } : (Pos){ line, d->lines[line].len }; d->has_sel = true; G.mouse_down = 2; G.mouse_anchor = (Pos){ line, 0 }; return;
        }
        Pos p = mouse_pos(d, e->mx, e->my);
        if (G.click_count >= 3) { d->anchor = (Pos){ p.line, 0 }; d->cur = p.line + 1 < d->nlines ? (Pos){ p.line + 1, 0 } : (Pos){ p.line, d->lines[p.line].len }; d->has_sel = true; G.click_count = 3; }
        else if (G.click_count == 2) { int a, b; if (doc_word_at(d, p, &a, &b)) { d->anchor = (Pos){ p.line, a }; d->cur = (Pos){ p.line, b }; d->has_sel = true; } }
        else { d->has_sel = false; if (e->mods & MOD_SHIFT) { if (!d->has_sel) d->anchor = d->cur; d->has_sel = true; d->cur = p; } else { d->cur = d->anchor = p; } }
        G.mouse_down = 1; G.mouse_anchor = d->anchor;
        d->want_vcol = doc_vcol(d, d->cur.line, d->cur.col, G.cfg.tab_width); doc_break_undo(d); return;
    }
    if (e->drag && G.mouse_down) {
        Pos p = mouse_pos(d, e->mx, e->my);
        if (G.mouse_down == 2) { d->has_sel = true; if (pos_cmp(p, G.mouse_anchor) >= 0) { d->anchor = G.mouse_anchor; d->cur = p.line + 1 < d->nlines ? (Pos){ p.line + 1, 0 } : (Pos){ p.line, d->lines[p.line].len }; } else { d->anchor = (Pos){ G.mouse_anchor.line + 1, 0 }; d->cur = (Pos){ p.line, 0 }; if (G.mouse_anchor.line + 1 >= d->nlines) d->anchor = (Pos){ G.mouse_anchor.line, d->lines[G.mouse_anchor.line].len }; } }
        else { d->has_sel = true; d->cur = p; }
        if (e->my < G.r_edit.y) { if (d->top > 0) d->top--; } if (e->my >= G.r_edit.y + G.r_edit.h) { if (d->top + 1 < d->nlines) d->top++; }
        d->want_vcol = doc_vcol(d, d->cur.line, d->cur.col, G.cfg.tab_width); ed_reveal(d); return;
    }
    if (e->release) G.mouse_down = 0;
}
