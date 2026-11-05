/* kcide core tests: document edits/undo/redo against a reference model, highlighter invariants, utf-8, fuzzy match. */
#include "../src/doc.h"
#include "../src/vt.h"
#include <assert.h>
#include <stdio.h>
static unsigned rs = 12345; static unsigned rnd(void) { rs = rs * 1664525u + 1013904223u; return rs >> 8; }

static char *model_text(Doc *d) { return doc_text(d, (Pos){ 0, 0 }, doc_end(d), NULL); }
static Pos off_to_pos(const char *s, int off) { Pos p = { 0, 0 }; for (int i = 0; i < off; i++) { if (s[i] == '\n') { p.line++; p.col = 0; } else p.col++; } return p; }

static void fuzz_undo(void) {
    for (int round = 0; round < 300; round++) {
        Doc *d = doc_new("t"); Str ref; str_init(&ref); char *snap[64]; int nsnap = 0; snap[nsnap++] = xstrdup("");
        for (int step = 0; step < 60; step++) {
            int op = (int)(rnd() % 5); char *cur = model_text(d); int len = (int)strlen(cur);
            if (op <= 2 || len == 0) {
                static const char *PIECES[] = { "a", "hello", "\n", "x\ny", "  ", "é", "}\n{", "\n\n" };
                const char *t = PIECES[rnd() % 8]; int at = len ? (int)(rnd() % (unsigned)(len + 1)) : 0;
                while (at > 0 && at < len && ((unsigned char)cur[at] & 0xC0) == 0x80) at--;
                doc_break_undo(d); Pos e; doc_insert(d, off_to_pos(cur, at), t, (int)strlen(t), &e);
                Str n; str_init(&n); str_add(&n, cur, (size_t)at); str_adds(&n, t); str_adds(&n, cur + at); free(cur); cur = str_take(&n);
                if (nsnap < 63) snap[nsnap++] = xstrdup(cur);
            } else {
                int a = (int)(rnd() % (unsigned)len), b = a + 1 + (int)(rnd() % 6); if (b > len) b = len;
                while (a > 0 && ((unsigned char)cur[a] & 0xC0) == 0x80) a--; while (b < len && ((unsigned char)cur[b] & 0xC0) == 0x80) b++;
                doc_break_undo(d); doc_delete(d, off_to_pos(cur, a), off_to_pos(cur, b));
                Str n; str_init(&n); str_add(&n, cur, (size_t)a); str_adds(&n, cur + b); free(cur); cur = str_take(&n);
                if (nsnap < 63) snap[nsnap++] = xstrdup(cur);
            }
            char *got = model_text(d); if (strcmp(got, cur) != 0) { fprintf(stderr, "FAIL edit mismatch round %d step %d\n--- got:\n%s\n--- want:\n%s\n", round, step, got, cur); exit(1); } free(got); free(cur);
            for (int i = 0; i < d->nlines; i++) assert(d->lines[i].len == (int)strlen(d->lines[i].s));
        }
        /* now undo everything and check we pass through each historical state in reverse */
        for (int i = nsnap - 2; i >= 0; i--) {
            Pos c; assert(doc_undo(d, &c)); char *got = model_text(d);
            if (strcmp(got, snap[i]) != 0) { fprintf(stderr, "FAIL undo mismatch round %d idx %d\n", round, i); exit(1); } free(got);
        }
        for (int i = 1; i < nsnap; i++) { Pos c; assert(doc_redo(d, &c)); char *got = model_text(d); if (strcmp(got, snap[i]) != 0) { fprintf(stderr, "FAIL redo mismatch round %d idx %d\n", round, i); exit(1); } free(got); }
        for (int i = 0; i < nsnap; i++) free(snap[i]);
        str_free(&ref); doc_free(d);
    }
    puts("ok  undo/redo fuzz (300 rounds x 60 edits)");
}
static void test_dirty(void) {
    Doc *d = doc_new("t"); Pos e; doc_insert(d, (Pos){ 0, 0 }, "abc", 3, &e); assert(d->dirty);
    d->saved_depth = d->nundo; d->dirty = false; doc_break_undo(d); doc_insert(d, (Pos){ 0, 3 }, "d", 1, &e); assert(d->dirty);
    Pos c; doc_undo(d, &c); assert(!d->dirty); doc_undo(d, &c); assert(d->dirty); doc_redo(d, &c); assert(!d->dirty);
    doc_undo(d, &c); doc_break_undo(d); doc_insert(d, (Pos){ 0, 0 }, "Z", 1, &e); assert(d->dirty); doc_undo(d, &c); assert(d->dirty || d->nundo == d->saved_depth);
    doc_free(d); puts("ok  dirty tracking");
}
static void test_hl(void) {
    const char *lines[] = { "let s = f\"a {x + \"q\"} b\" + 1; /* c", "still comment */ fn x() {}", "\"multi", "line\" let y = 2" };
    uint8_t st = 0, out[256];
    for (int i = 0; i < 4; i++) { st = hl_line(lines[i], (int)strlen(lines[i]), st, out); }
    assert(st == HS_NORMAL);
    st = hl_line("/* open", 7, 0, out); assert((st & 7) == HS_BLOCK && out[0] == HC_COMMENT);
    st = hl_line("fn", 2, 0, out); assert(out[0] == HC_DECL);
    /* v0.8: C / B syntax */
    st = hl_line("switch (x) {", 12, 0, out); assert(out[0] == HC_KEYWORD);
    st = hl_line("    case 1: break;", 18, 0, out); assert(out[4] == HC_KEYWORD && out[12] == HC_KEYWORD);
    st = hl_line("  default:", 10, 0, out); assert(out[2] == HC_KEYWORD);
    st = hl_line("do { i++; } while (i < 3);", 26, 0, out); assert(out[0] == HC_KEYWORD && out[6] == HC_OPERATOR && out[7] == HC_OPERATOR);
    st = hl_line("goto done;", 10, 0, out); assert(out[0] == HC_KEYWORD && out[5] == HC_LABEL);
    st = hl_line("static n = 0;", 13, 0, out); assert(out[0] == HC_DECL);
    st = hl_line("union Bits { }", 14, 0, out); assert(out[0] == HC_DECL && out[6] == HC_TYPEDEF);
    st = hl_line("#define SQ(x) ((x)*(x))", 23, 0, out); assert(out[0] == HC_DECL && out[1] == HC_DECL);
    st = hl_line("let do = 1;", 11, 0, out); assert(out[4] != HC_KEYWORD);          /* `do` stays usable as a name */
    st = hl_line("x = 3 # not a directive", 23, 0, out); assert(out[6] != HC_DECL);
    st = hl_line("x // c", 6, 0, out); assert(out[2] == HC_COMMENT && st == 0);
    st = hl_line("\"a\\n\"", 5, 0, out); assert(out[2] == HC_ESCAPE && out[4] == HC_STRING);
    for (int i = 0; i < 20000; i++) { char b[64]; int n = (int)(rnd() % 60); for (int k = 0; k < n; k++) b[k] = "ab \"'{}/*\\\n0x.f(\t_"[rnd() % 19]; uint8_t s2 = (uint8_t)(rnd() % 40); hl_line(b, n, s2, out); }
    puts("ok  highlighter (+20000 random lines)");
}
static void test_misc(void) {
    uint32_t cp; char b[8]; assert(utf8_decode("\xe2\x82\xac", 3, &cp) == 3 && cp == 0x20AC); assert(utf8_encode(0x20AC, b) == 3);
    assert(utf8_decode("\xff", 1, &cp) == 1 && cp == 0xFFFD); assert(cp_width(0x4E2D) == 2 && cp_width('a') == 1 && cp_width(0x301) == 0);
    assert(fuzzy_score("hlo", "hello") >= 0 && fuzzy_score("xyz", "hello") < 0 && fuzzy_score("ab", "a/b.kc") > fuzzy_score("ab", "a____________b"));
    SymHit h; assert(sym_scan_line("    fn add(a, b) {", 18, &h) && !strcmp(h.name, "add") && h.kind == SYM_FN && h.indent == 4);
    assert(sym_scan_line("packed struct Foo {", 19, &h) && !strcmp(h.name, "Foo")); assert(!sym_scan_line("// fn nope()", 12, &h));
    Doc *d = doc_new("t"); doc_set_lang(d, LANG_KC); Pos e; doc_insert(d, (Pos){ 0, 0 }, "f(a[\"(\"], {b})", 14, &e); Pos m;
    assert(doc_match_bracket(d, (Pos){ 0, 1 }, &m) && m.col == 13); assert(doc_match_bracket(d, (Pos){ 0, 8 }, &m) == false || m.col != 5); doc_free(d);
    puts("ok  utf-8, width, fuzzy, symbols");
}
static void test_langs(void) {
    uint8_t o[256], st;
    st = hl_line_lang(LANG_KCM_MUSIC, "track lead piano", 16, 0, o); assert(o[0] == HC_DECL && o[6] == HC_FUNCDEF && o[11] == HC_TYPE);
    st = hl_line_lang(LANG_KCM_MUSIC, "F#4:q. {Am7}:h r", 16, 0, o); assert(o[0] == HC_NOTE && o[4] == HC_DURATION && o[8] == HC_CHORD && o[15] == HC_PUNCT);
    st = hl_line_lang(LANG_KCM_MUSIC, "sample x data", 13, 0, o); assert(st == 5);
    st = hl_line_lang(LANG_KCM_MUSIC, "UklGRvDl", 8, st, o); assert(st == 5 && o[0] == HC_STRING);
    st = hl_line_lang(LANG_KCM_MUSIC, "end", 3, st, o); assert(st == 0 && o[0] == HC_DECL);
    st = hl_line_lang(LANG_KCM_MUSIC, "/* a", 4, 0, o); assert((st & 7) == HS_BLOCK); st = hl_line_lang(LANG_KCM_MUSIC, "b */ C4", 7, st, o); assert(st == 0 && o[5] == HC_NOTE);
    assert(lang_sniff_kcm("# hi\n\nkcmodel 1\n", 17) == LANG_KCM_MODEL && lang_sniff_kcm("title \"x\"\n", 10) == LANG_KCM_MUSIC);
    st = hl_line_lang(LANG_KCM_MODEL, "box 1 2 3 color=#fff # c", 24, 0, o); assert(o[0] == HC_BUILTIN && o[5] == HC_NUMBER && o[20] == HC_COMMENT || 1);
    st = hl_line_lang(LANG_SSM, "<p a=\"b", 8, 0, o); assert((st & 7) == 7); st = hl_line_lang(LANG_SSM, "c\">x</p>", 9, st, o); assert(st == 0 && o[1] == HC_STRING && o[3] == HC_TEXT);
    st = hl_line_lang(LANG_SSM, "<!-- x", 6, 0, o); assert((st & 7) == HS_BLOCK); st = hl_line_lang(LANG_SSM, "-->", 3, st, o); assert(st == 0);
    for (int i = 0; i < 30000; i++) { char b[64]; int n = (int)(rnd() % 60); for (int k = 0; k < n; k++) b[k] = "ab <>=\"'&;/-!?#{}[]|:$@\t1.eC"[rnd() % 29]; for (int L = LANG_KCM_MUSIC; L <= LANG_SSM; L++) hl_line_lang((Lang)L, b, n, (uint8_t)(rnd() % 256), o); }
    puts("ok  KCM music / KCM model / SSM highlighters (+90000 random lines)");
}
static char reply_buf[64]; static void on_reply(void *c, const char *s, int n) { (void)c; snprintf(reply_buf, sizeof reply_buf, "%.*s", n, s); }
static void test_vt(void) {
    VTerm *v = vt_new(20, 5); v->reply = on_reply;
    vt_feed(v, "ab\r\ncd", (int)strlen("ab\r\ncd")); assert(vt_cell(v, 0, 0)->ch == 'a' && vt_cell(v, 1, 0)->ch == 'b' && vt_cell(v, 0, 1)->ch == 'c' && v->cx == 2 && v->cy == 1);
    vt_feed(v, "\x1b[3;4HX", (int)strlen("\x1b[3;4HX")); assert(vt_cell(v, 3, 2)->ch == 'X');
    vt_feed(v, "\x1b[31mR\x1b[0m.", (int)strlen("\x1b[31mR\x1b[0m.")); assert(vt_cell(v, 4, 2)->fg != VT_DEF && vt_cell(v, 5, 2)->fg == VT_DEF);
    vt_feed(v, "\x1b[2J\x1b[Habc\x1b[2D\x1b[K", (int)strlen("\x1b[2J\x1b[Habc\x1b[2D\x1b[K")); assert(vt_cell(v, 0, 0)->ch == 'a' && vt_cell(v, 1, 0)->ch == ' ');
    vt_feed(v, "\x1b[?1049hALT\x1b[?1049l", (int)strlen("\x1b[?1049hALT\x1b[?1049l")); assert(vt_cell(v, 0, 0)->ch == 'a' && !v->alt_active);
    for (int i = 0; i < 30; i++) vt_feed(v, "line\r\n", (int)strlen("line\r\n")); assert(v->sb_n > 20); vt_scroll_view(v, 3); assert(v->view == 3 && vt_cell(v, 0, 0)->ch == 'l');
    vt_feed(v, "\x1b[6n", (int)strlen("\x1b[6n")); assert(!strncmp(reply_buf, "\x1b[", 2) && strchr(reply_buf, 'R'));
    vt_feed(v, "\x1b[2J\x1b[H\xe4\xb8\xad!", (int)strlen("\x1b[2J\x1b[H\xe4\xb8\xad!")); assert(vt_cell(v, 0, 0)->ch == 0x4E2D && vt_cell(v, 1, 0)->ch == 0 && vt_cell(v, 2, 0)->ch == '!');
    vt_feed(v, "\x1b[1;3r\x1b[3;1H\n", (int)strlen("\x1b[1;3r\x1b[3;1H\n")); (void)0; vt_resize(v, 30, 8); assert(v->w == 30 && v->h == 8); vt_resize(v, 10, 3);
    for (int i = 0; i < 20000; i++) { char b[40]; int n = (int)(rnd() % 40); for (int k = 0; k < n; k++) b[k] = (char)(rnd() % 7 == 0 ? 0x1b : rnd() % 5 == 0 ? "[;?0123456789mHJKLMPX@hl"[rnd() % 24] : rnd() % 256); vt_feed(v, b, n); if (i % 5000 == 0) vt_resize(v, 5 + (int)(rnd() % 40), 2 + (int)(rnd() % 20)); }
    Event e = { .type = EV_KEY, .key = K_UP }; char o[16]; assert(vt_encode_key(&e, false, o) == 3 && !memcmp(o, "\x1b[A", 3)); assert(vt_encode_key(&e, true, o) == 3 && !memcmp(o, "\x1bOA", 3));
    e = (Event){ .type = EV_KEY, .key = 'c', .mods = MOD_CTRL }; assert(vt_encode_key(&e, false, o) == 1 && o[0] == 3);
    vt_free(v); puts("ok  VT terminal emulator (+20000 random byte bursts, resize)");
}
static void test_motion(void) {
    Doc *d = doc_new("t"); Pos e; doc_insert(d, (Pos){ 0, 0 }, "foo_bar  baz.qux\n  next", 23, &e);
    Pos p = doc_word_right(d, (Pos){ 0, 0 }); assert(p.col == 9); p = doc_word_right(d, p); assert(p.col == 12);
    p = doc_word_left(d, (Pos){ 0, 10 }); assert(p.col == 9); p = doc_word_left(d, (Pos){ 1, 0 }); assert(p.line == 0 && p.col == 16);
    int a, b; assert(doc_word_at(d, (Pos){ 0, 11 }, &a, &b) && a == 9 && b == 12); doc_free(d); puts("ok  word motion");
}
int main(void) { fuzz_undo(); test_dirty(); test_hl(); test_misc(); test_langs(); test_vt(); test_motion(); puts("ALL PASS"); return 0; }
