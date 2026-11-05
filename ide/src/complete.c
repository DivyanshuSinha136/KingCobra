#include "complete.h"
#include <ctype.h>

typedef struct { char *name; char **m; int n, cap; } StdMod;
static StdMod *g_mods; static int g_nmods, g_total_members;

static void mod_add(StdMod *m, const char *n, int len) {
    for (int i = 0; i < m->n; i++) if ((int)strlen(m->m[i]) == len && !memcmp(m->m[i], n, (size_t)len)) return;
    if (m->n == m->cap) { m->cap = m->cap ? m->cap * 2 : 32; m->m = xrealloc(m->m, (size_t)m->cap * sizeof(char *)); }
    m->m[m->n++] = xstrndup(n, (size_t)len); g_total_members++;
}
static void scan_c(StdMod *m, const char *src) {
    for (const char *p = src; (p = strstr(p, "{\"")); p += 2) {
        const char *s = p + 2, *e = s;
        while (isalnum((unsigned char)*e) || *e == '_') e++;
        if (e == s || *e != '"' || e - s < 2 || e - s > 60) continue;
        const char *q = e + 1; if (*q != ',') continue; q++; while (*q == ' ' || *q == '\t' || *q == '\n') q++;
        const char *f = q; while (isalnum((unsigned char)*q) || *q == '_') q++;
        if (q == f) continue;
        while (*q == ' ') q++;
        if (*q != '}' && *q != ',') continue;
        if (isupper((unsigned char)s[0])) continue;
        mod_add(m, s, (int)(e - s));
    }
}
void std_api_load(const char *root) {
    for (int i = 0; i < KC_N_STD_MODULES; i++) {
        g_mods = xrealloc(g_mods, (size_t)(g_nmods + 1) * sizeof *g_mods);
        StdMod *m = &g_mods[g_nmods++]; memset(m, 0, sizeof *m); m->name = xstrdup(KC_STD_MODULES[i]);
        if (!root) continue;
        char *rel = xfmt("%s%cstd%c%s", root, PATH_SEP, PATH_SEP, m->name);
        DirList dl;
        if (dir_list(rel, &dl)) {
            for (int k = 0; k < dl.n; k++) {
                const char *ext = path_ext(dl.v[k].name);
                if (dl.v[k].dir || strcmp(ext, ".c")) continue;
                char *fp = path_join(rel, dl.v[k].name); char *src = read_file(fp, NULL);
                if (src) { scan_c(m, src); free(src); }
                free(fp);
            }
            dir_list_free(&dl);
        }
        free(rel);
    }
}
int std_api_member_count(void) { return g_total_members; }

static const struct { const char *trig, *body, *desc; } SNIP[] = {
    { "fn", "fn $0() {\n\t\n}", "function" }, { "main", "fn main() {\n\t$0\n}", "main function" },
    { "if", "if ($0) {\n\t\n}", "if" }, { "ifelse", "if ($0) {\n\t\n} else {\n\t\n}", "if / else" },
    { "while", "while ($0) {\n\t\n}", "while loop" }, { "for", "for ($0 in ) {\n\t\n}", "for-in loop" },
    { "forr", "for (i in 0..$0) {\n\t\n}", "range loop" }, { "match", "match ($0) {\n\t_ => ,\n}", "match" },
    { "struct", "struct $0 {\n\t\n}", "struct" }, { "enum", "enum $0 {\n\t\n}", "enum" }, { "impl", "impl $0 {\n\t\n}", "impl block" },
    { "try", "try {\n\t$0\n} catch (e) {\n\tprint(e.message);\n}", "try / catch" }, { "test", "fn test_$0() {\n\tassert_eq(, );\n}", "test function" },
    { "import", "import std.$0;", "import std module" }, { "defer", "defer $0;", "defer" }, { "extern", "extern fn $0() -> int from \"\";", "extern fn" },
    { "closure", "fn($0) -> int {\n\treturn ;\n}", "closure" }, { "let", "let $0 = ;", "let" },
    { "switch", "switch ($0) {\n\tcase 1:\n\t\tbreak;\n\tdefault:\n\t\tbreak;\n}", "switch (C fall-through)" },
    { "do", "do {\n\t$0\n} while ();", "do / while" }, { "cfor", "for (var i = 0; i < $0; i++) {\n\t\n}", "C-style for" },
    { "static", "static $0 = 0;", "static local" }, { "union", "union $0 {\n\t\n}", "union" }, { "typedef", "typedef $0 ;", "typedef" },
    { "goto", "goto $0;", "goto" }, { "auto", "auto $0;", "auto (B)" }, { "bmain", "main() {\n\t$0\n}", "B-style main" },
    { "printf", "printf(\"$0\\n\");", "printf" }, { "#define", "#define $0 ", "preprocessor define" }, { "#include", "#include \"$0\"", "preprocessor include" },
    { "#ifdef", "#ifdef $0\n\n#endif", "#ifdef ... #endif" },
};
const char *snippet_lookup(const char *w, int n) {
    for (size_t i = 0; i < sizeof SNIP / sizeof *SNIP; i++) if ((int)strlen(SNIP[i].trig) == n && !memcmp(SNIP[i].trig, w, (size_t)n)) return SNIP[i].body;
    return NULL;
}
const char *comp_kind_tag(CKind k) {
    static const char *T[] = { "kw", "type", "fn", "fn", "mod", "snip", "txt", "mbr", "sym" };
    return T[k];
}
static void add(Comp *c, const char *label, const char *ins, CKind k, int rank) {
    for (int i = 0; i < c->n; i++) if (!strcmp(c->v[i].label, label) && c->v[i].kind != CK_SNIPPET && k != CK_SNIPPET) return;
    if (c->n == c->cap) { c->cap = c->cap ? c->cap * 2 : 64; c->v = xrealloc(c->v, (size_t)c->cap * sizeof(CItem)); }
    c->v[c->n++] = (CItem){ xstrdup(label), xstrdup(ins ? ins : label), k, rank };
}
void comp_close(Comp *c) {
    for (int i = 0; i < c->n; i++) { free(c->v[i].label); free(c->v[i].insert); }
    c->n = 0; c->open = false; c->sel = c->scroll = 0;
}
static bool pre_match(const char *label, const char *pre, int pn) {
    for (int i = 0; i < pn; i++) if (!label[i] || tolower((unsigned char)label[i]) != tolower((unsigned char)pre[i])) return false;
    return true;
}
static int cmp_items(const void *a, const void *b) {
    const CItem *x = a, *y = b;
    if (x->rank != y->rank) return x->rank - y->rank;
    return strcasecmp(x->label, y->label);
}
static void consider(Comp *c, const char *label, const char *ins, CKind k, int rank, const char *pre, int pn, bool contains) {
    if (!strcmp(label, pre)) return;
    if (pre_match(label, pre, pn)) add(c, label, ins, k, rank);
    else if (contains && pn >= 2) { char *lo = xstrdup(label), *pl = xstrndup(pre, (size_t)pn); for (char *p = lo; *p; p++) *p = (char)tolower((unsigned char)*p); for (char *p = pl; *p; p++) *p = (char)tolower((unsigned char)*p);
        if (strstr(lo, pl)) { add(c, label, ins, k, rank + 100); }
        free(lo); free(pl); }
}


static const struct { Lang lang; const char *trig, *body, *desc; } LSNIP[] = {
    { LANG_KCM_MUSIC, "track", "track $0 piano vol=0.8", "track" }, { LANG_KCM_MUSIC, "pattern", "pattern $0\n\t\nend", "pattern block" },
    { LANG_KCM_MUSIC, "sample", "sample $0 \"file.wav\" gain=0", "audio sample" }, { LANG_KCM_MUSIC, "include", "include \"$0.kcm\"", "include file" },
    { LANG_KCM_MUSIC, "tempo", "tempo=$0", "tempo change" }, { LANG_KCM_MUSIC, "repeat", "|: $0 :|x2", "repeat" },
    { LANG_KCM_MODEL, "part", "part color=#$0", "material part" }, { LANG_KCM_MODEL, "box", "box $0 1 1 color=#ffffff", "box primitive" },
    { LANG_KCM_MODEL, "sphere", "sphere $0 1 1 seg=16", "sphere primitive" },
    { LANG_SSM, "break", "break time=\"$0\"/>", "pause" }, { LANG_SSM, "prosody", "prosody rate=\"$0\"></prosody>", "rate/pitch/volume" },
    { LANG_SSM, "emphasis", "emphasis level=\"$0\"></emphasis>", "emphasis" }, { LANG_SSM, "phoneme", "phoneme alphabet=\"ipa\" ph=\"$0\"></phoneme>", "pronunciation" },
    { LANG_SSM, "sub", "sub alias=\"$0\"></sub>", "substitution" }, { LANG_SSM, "say-as", "say-as interpret-as=\"$0\"></say-as>", "say-as" },
    { LANG_SSM, "voice", "voice name=\"$0\"></voice>", "voice" }, { LANG_SSM, "mark", "mark name=\"$0\"/>", "marker" },
    { LANG_SSM, "p", "p>$0</p>", "paragraph" }, { LANG_SSM, "s", "s>$0</s>", "sentence" }, { LANG_SSM, "speak", "speak>\n\t$0\n</speak>", "document root" },
};
static void comp_lang(Comp *c, Doc *d, bool manual) {
    Line *L = &d->lines[d->cur.line]; int s = d->cur.col;
    while (s > 0 && (isalnum((unsigned char)L->s[s - 1]) || L->s[s - 1] == '_' || (d->lang == LANG_SSM && L->s[s - 1] == '-') || (d->lang == LANG_KCM_MODEL && L->s[s - 1] == '='))) s--;
    int pn = d->cur.col - s; const char *pre = L->s + s;
    if (pn > 0 && isdigit((unsigned char)pre[0])) return;
    if (!manual && pn < 2 && !(d->lang == LANG_SSM && s > 0 && L->s[s - 1] == '<')) return;
    doc_ensure_hl(d, d->cur.line); if (L->len && s > 0 && (L->hl[s - 1] == HC_COMMENT || L->hl[s - 1] == HC_STRING) && d->lang != LANG_KCM_MUSIC) return;
    c->start = (Pos){ d->cur.line, s };
    bool tag = d->lang == LANG_SSM && s > 0 && L->s[s - 1] == '<';
    const char *const *w = lang_words(d->lang);
    if (d->lang != LANG_SSM || tag) for (int i = 0; w && w[i]; i++) if (d->lang != LANG_SSM || tag) consider(c, w[i], NULL, CK_KEYWORD, 10, pre, pn, false);
    for (size_t i = 0; i < sizeof LSNIP / sizeof *LSNIP; i++) if (LSNIP[i].lang == d->lang && pn > 0 && pre_match(LSNIP[i].trig, pre, pn)) {
        char lab[96]; snprintf(lab, sizeof lab, "%s  \xe2\x80\xa2 %s", LSNIP[i].trig, LSNIP[i].desc); add(c, lab, LSNIP[i].body, CK_SNIPPET, 5); }
    if (d->lang == LANG_SSM && !tag) for (int i = 0; w && w[i]; i++) consider(c, w[i], NULL, CK_KEYWORD, 10, pre, pn, false);   /* attribute names */
    if (d->lang == LANG_KCM_MUSIC)                                    /* track / pattern / sample names from this score */
        for (int i = 0; i < d->nlines && c->n < 300; i++) { Line *l = &d->lines[i]; int a = 0; while (a < l->len && l->s[a] == ' ') a++;
            int kl = !strncmp(l->s + a, "track ", 6) ? 6 : !strncmp(l->s + a, "pattern ", 8) ? 8 : !strncmp(l->s + a, "sample ", 7) ? 7 : 0;
            if (!kl) continue; int b = a + kl; while (b < l->len && l->s[b] == ' ') b++; int e = b; while (e < l->len && (isalnum((unsigned char)l->s[e]) || l->s[e] == '_' || l->s[e] == '-')) e++;
            if (e > b && e - b < 60) { char nm[64]; memcpy(nm, l->s + b, (size_t)(e - b)); nm[e - b] = 0; consider(c, nm, NULL, CK_SYMBOL, 4, pre, pn, false); if (kl == 8) { char pr[70]; snprintf(pr, sizeof pr, "$%s", nm); (void)pr; } } }
    if (c->n > 1) qsort(c->v, (size_t)c->n, sizeof(CItem), cmp_items);
    c->open = c->n > 0; c->sel = c->scroll = 0;
}

void comp_build(Comp *c, Doc *d, Doc **all, int nall, bool manual) {
    comp_close(c);
    if (d->lang == LANG_KCM_MUSIC || d->lang == LANG_KCM_MODEL || d->lang == LANG_SSM) { comp_lang(c, d, manual); return; }
    Line *L = &d->lines[d->cur.line]; int col = d->cur.col, s = col;
    while (s > 0) { int p = utf8_prev(L->s, s); uint32_t cp; utf8_decode(L->s + p, (size_t)(L->len - p), &cp); if (!cp_is_word(cp)) break; s = p; }
    int pn = col - s; const char *pre = L->s + s;
    if (pn > 0 && isdigit((unsigned char)pre[0])) return;
    if (d->is_kc && d->cur.col > 0 && d->cur.col <= L->len) { doc_ensure_hl(d, d->cur.line); int hc = s > 0 ? L->hl[s - 1] : 0; if (!hl_is_code(hc) && s > 0) return; }
    char qual[64] = ""; bool dot = s > 0 && L->s[s - 1] == '.';
    if (dot) {
        int e = s - 1, b = e; while (b > 0 && (isalnum((unsigned char)L->s[b - 1]) || L->s[b - 1] == '_')) b--;
        if (e - b > 0 && e - b < 63) { memcpy(qual, L->s + b, (size_t)(e - b)); qual[e - b] = 0; }
    }
    if (!manual && !dot && pn < 2) return;
    c->start = (Pos){ d->cur.line, s };
    if (dot) {
        bool imp = false; { int i = 0; while (i < L->len && (L->s[i] == ' ' || L->s[i] == '\t')) i++; imp = !strncmp(L->s + i, "import", 6); }
        if (imp && !strcmp(qual, "std")) { for (int i = 0; i < g_nmods; i++) consider(c, g_mods[i].name, NULL, CK_MODULE, 0, pre, pn, false); }
        else {
            /* resolve alias: import std.X as qual  /  module name itself */
            const char *modname = qual;
            for (int i = 0; i < d->nlines; i++) {
                Line *l = &d->lines[i]; if (l->len < 12 || strncmp(l->s, "import std.", 11)) continue;
                char tmp[256]; int n = l->len < 255 ? l->len : 255; memcpy(tmp, l->s, (size_t)n); tmp[n] = 0;
                char *as = strstr(tmp, " as ");
                if (as) { char *a = as + 4; char *e = a; while (isalnum((unsigned char)*e) || *e == '_') e++; *e = 0;
                    if (!strcmp(a, qual)) { char *m = tmp + 11; char *me = m; while (isalnum((unsigned char)*me) || *me == '_') me++; *me = 0; static char keep[64]; snprintf(keep, sizeof keep, "%s", m); modname = keep; break; } }
            }
            bool found = false;
            for (int i = 0; i < g_nmods; i++) if (!strcmp(g_mods[i].name, modname)) { found = true; for (int k = 0; k < g_mods[i].n; k++) consider(c, g_mods[i].m[k], NULL, CK_MEMBER, 0, pre, pn, true); }
            if (!found || c->n == 0) for (int i = 0; i < KC_N_METHODS; i++) consider(c, KC_METHODS[i], NULL, CK_MEMBER, 10, pre, pn, true);
        }
    } else {
        for (int i = 0; i < KC_N_KEYWORDS; i++) consider(c, KC_KEYWORDS[i], NULL, CK_KEYWORD, 20, pre, pn, false);
        for (int i = 0; i < KC_N_TYPES; i++) consider(c, KC_TYPES[i], NULL, CK_TYPE, 30, pre, pn, false);
        for (int i = 0; i < KC_N_BUILTINS; i++) { char ins[80]; snprintf(ins, sizeof ins, "%s", KC_BUILTINS[i]); consider(c, KC_BUILTINS[i], ins, CK_BUILTIN, 15, pre, pn, true); }
        for (int i = 0; i < g_nmods; i++) consider(c, g_mods[i].name, NULL, CK_MODULE, 40, pre, pn, false);
        for (size_t i = 0; i < sizeof SNIP / sizeof *SNIP; i++) if (pre_match(SNIP[i].trig, pre, pn) && pn > 0) { char lab[96]; snprintf(lab, sizeof lab, "%s  \xe2\x80\xa2 %s", SNIP[i].trig, SNIP[i].desc); add(c, lab, SNIP[i].body, CK_SNIPPET, 50); }
        for (int di = 0; di < nall; di++) {
            Doc *o = all[di];
            for (int i = 0; i < o->nlines && c->n < 600; i++) {
                SymHit h; if (sym_scan_line(o->lines[i].s, o->lines[i].len, &h) && h.kind != SYM_IMPL) consider(c, h.name, NULL, CK_SYMBOL, 5, pre, pn, true);
                Line *l = &o->lines[i];
                for (int j = 0; j < l->len;) {
                    if (!(isalpha((unsigned char)l->s[j]) || l->s[j] == '_')) { j++; continue; }
                    int e = j; while (e < l->len && (isalnum((unsigned char)l->s[e]) || l->s[e] == '_')) e++;
                    if (e - j >= 3 && e - j < 60 && !(o == d && i == d->cur.line && j == s)) { char w[64]; memcpy(w, l->s + j, (size_t)(e - j)); w[e - j] = 0; consider(c, w, NULL, CK_WORD, 60, pre, pn, false); }
                    j = e;
                }
            }
        }
    }
    if (c->n > 1) qsort(c->v, (size_t)c->n, sizeof(CItem), cmp_items);
    if (c->n == 1 && c->v[0].kind != CK_SNIPPET && pn > 0 && !strcmp(c->v[0].label, pre)) { comp_close(c); return; }
    c->open = c->n > 0; c->sel = c->scroll = 0;
}
