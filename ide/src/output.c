#include "output.h"
#include <ctype.h>
#define MAX_LINES 20000

static Color PAL[16] = { 0x3b4252, 0xf47067, 0x56d364, 0xe3b341, 0x58a6ff, 0xbc8cff, 0x39c5cf, 0xb1bac4,
                         0x6e7681, 0xff938a, 0x7ee787, 0xf2cc60, 0x79c0ff, 0xd2a8ff, 0x56d4dd, 0xf0f6fc };

void out_init(Output *o, Color fg) { memset(o, 0, sizeof *o); str_init(&o->esc); o->default_fg = o->fg = fg; o->follow = true; o->lines = xcalloc(64, sizeof(OLine)); o->cap = 64; o->n = 1; o->lines[0].diag = -1; }
static void free_line(OLine *l) { free(l->c); }
void out_clear(Output *o) {
    for (int i = 0; i < o->n; i++) free_line(&o->lines[i]);
    for (int i = 0; i < o->ndiag; i++) { free(o->diags[i].file); free(o->diags[i].msg); }
    o->n = 1; memset(&o->lines[0], 0, sizeof(OLine)); o->lines[0].diag = -1; o->ndiag = 0; o->col = 0; o->st = 0; o->scroll = 0; o->follow = true; o->fg = o->default_fg; o->attr = 0; o->npend = 0;
    str_clear(&o->esc);
}
void out_free(Output *o) { out_clear(o); free(o->lines); free(o->diags); str_free(&o->esc); }
void out_drop_diags_for(Output *o, const char *file) { (void)o; (void)file; }

static void new_line(Output *o) {
    if (o->n == o->cap) { o->cap *= 2; o->lines = xrealloc(o->lines, (size_t)o->cap * sizeof(OLine)); }
    memset(&o->lines[o->n], 0, sizeof(OLine)); o->lines[o->n].diag = -1; o->n++; o->col = 0;
    if (o->n > MAX_LINES) {
        int drop = 2000; for (int i = 0; i < drop; i++) free_line(&o->lines[i]);
        memmove(o->lines, o->lines + drop, (size_t)(o->n - drop) * sizeof(OLine)); o->n -= drop;
        for (int i = 0; i < o->n; i++) if (o->lines[i].diag >= 0) { /* diag indexes stay valid: diags array is independent */ }
        o->scroll = o->scroll > drop ? o->scroll - drop : 0;
    }
}
static void put_cell(Output *o, uint32_t ch) {
    OLine *l = &o->lines[o->n - 1];
    if (o->col >= l->cap) { int nc = l->cap ? l->cap * 2 : 64; while (nc <= o->col) nc *= 2; l->c = xrealloc(l->c, (size_t)nc * sizeof(OCell)); l->cap = nc; }
    while (l->n < o->col) l->c[l->n++] = (OCell){ ' ', o->default_fg, 0 };
    l->c[o->col] = (OCell){ ch, o->fg, o->attr };
    if (o->col >= l->n) l->n = o->col + 1;
    o->col++;
}

/* Recognise "FILE:LINE:COL: error: MSG" in a completed line. */
static void scan_diag(Output *o, OLine *l) {
    Str t; str_init(&t);
    for (int i = 0; i < l->n; i++) { char b[8]; int k = utf8_encode(l->c[i].ch ? l->c[i].ch : ' ', b); str_add(&t, b, (size_t)k); }
    const char *s = t.p; int sev = 0;
    const char *m = strstr(s, ": error: ");
    if (!m) { m = strstr(s, ": warning: "); sev = 1; }
    if (m) {
        const char *q = m;                                    /* walk back over :COL and :LINE */
        const char *c2 = q; while (c2 > s && isdigit((unsigned char)c2[-1])) c2--;
        if (c2 < q && c2 > s && c2[-1] == ':') {
            const char *c1 = c2 - 1; const char *l2 = c1; while (l2 > s && isdigit((unsigned char)l2[-1])) l2--;
            if (l2 < c1 && l2 > s && l2[-1] == ':') {
                if (o->ndiag == o->dcap) { o->dcap = o->dcap ? o->dcap * 2 : 32; o->diags = xrealloc(o->diags, (size_t)o->dcap * sizeof(Diag)); }
                Diag *d = &o->diags[o->ndiag];
                { char *f = xstrndup(s, (size_t)(l2 - 1 - s)); d->file = (path_is_abs(f) || f[0] == '<') ? f : path_abs(f); if (d->file != f) free(f); } d->line = atoi(l2); d->col = atoi(c2); d->sev = sev;
                d->msg = xstrdup(m + (sev ? 11 : 9));
                l->diag = o->ndiag++;
            }
        }
    }
    if (l->diag < 0) {
        const char *k = strstr(s, ".kc:");
        if (k) { const char *q = k + 4; if (isdigit((unsigned char)*q)) { int ln = atoi(q); while (isdigit((unsigned char)*q)) q++; int cl = 0; if (*q == ':' && isdigit((unsigned char)q[1])) cl = atoi(q + 1);
            const char *b = k; while (b > s && !isspace((unsigned char)b[-1]) && b[-1] != '(' && b[-1] != '[') b--;
            if (o->ndiag == o->dcap) { o->dcap = o->dcap ? o->dcap * 2 : 32; o->diags = xrealloc(o->diags, (size_t)o->dcap * sizeof(Diag)); }
            Diag *d = &o->diags[o->ndiag]; char *f = xstrndup(b, (size_t)(k + 3 - b)); d->file = path_is_abs(f) ? f : path_abs(f); if (d->file != f) free(f);
            d->line = ln; d->col = cl; d->sev = 2; d->msg = xstrdup(s); l->diag = o->ndiag++; } }
    }
    str_free(&t);
}
static void end_line(Output *o) { scan_diag(o, &o->lines[o->n - 1]); new_line(o); }

static void do_sgr(Output *o, const char *p) {
    int v[16], n = 0; const char *q = p;
    do { v[n++ < 15 ? n - 1 : 15] = atoi(q); q = strchr(q, ';'); if (q) q++; } while (q && n < 15);
    for (int i = 0; i < n; i++) {
        int c = v[i];
        if (c == 0) { o->fg = o->default_fg; o->attr = 0; }
        else if (c == 1) o->attr |= ATTR_BOLD; else if (c == 2) o->attr |= ATTR_DIM; else if (c == 3) o->attr |= ATTR_ITALIC; else if (c == 4) o->attr |= ATTR_UNDERLINE;
        else if (c == 22) o->attr &= (uint8_t) ~(ATTR_BOLD | ATTR_DIM); else if (c == 23) o->attr &= (uint8_t)~ATTR_ITALIC; else if (c == 24) o->attr &= (uint8_t)~ATTR_UNDERLINE;
        else if (c >= 30 && c <= 37) o->fg = PAL[c - 30]; else if (c >= 90 && c <= 97) o->fg = PAL[c - 90 + 8]; else if (c == 39) o->fg = o->default_fg;
        else if (c == 38 && i + 1 < n) {
            if (v[i + 1] == 5 && i + 2 < n) { int k = v[i + 2]; if (k < 16) o->fg = PAL[k]; else if (k >= 232) { int g = 8 + (k - 232) * 10; o->fg = (Color)(g << 16 | g << 8 | g); } else { k -= 16; int r = k / 36, g = (k / 6) % 6, b = k % 6; o->fg = (Color)(((r ? r * 40 + 55 : 0) << 16) | ((g ? g * 40 + 55 : 0) << 8) | (b ? b * 40 + 55 : 0)); } i += 2; }
            else if (v[i + 1] == 2 && i + 4 < n) { o->fg = (Color)((v[i + 2] & 255) << 16 | (v[i + 3] & 255) << 8 | (v[i + 4] & 255)); i += 4; }
        }
    }
}
static void do_csi(Output *o, const char *p, int len, char f) {
    char buf[64]; int n = len < 63 ? len : 63; memcpy(buf, p, (size_t)n); buf[n] = 0;
    int a = atoi(buf); if (a < 1 && (f == 'C' || f == 'D')) a = 1;
    switch (f) {
    case 'm': do_sgr(o, n ? buf : "0"); break;
    case 'K': { OLine *l = &o->lines[o->n - 1]; if (a == 0 || a == 2) { if (a == 2) o->col = 0, l->n = 0; else if (l->n > o->col) l->n = o->col; } else if (a == 1) { for (int i = 0; i < o->col && i < l->n; i++) l->c[i].ch = ' '; } break; }
    case 'J': if (a == 2 || a == 3) { /* clear screen: keep history, start fresh line */ if (o->lines[o->n - 1].n) new_line(o); } break;
    case 'C': o->col += a; break;
    case 'D': o->col = o->col > a ? o->col - a : 0; break;
    case 'G': o->col = a > 0 ? a - 1 : 0; break;
    default: break;
    }
}

void out_feed(Output *o, const char *d, int n) {
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)d[i];
        if (o->st == 1) {                                       /* after ESC */
            if (c == '[') { o->st = 2; str_clear(&o->esc); } else if (c == ']') { o->st = 3; } else o->st = 0;
            continue;
        }
        if (o->st == 2) {                                       /* CSI */
            if (c >= 0x40 && c <= 0x7e) { do_csi(o, o->esc.p, (int)o->esc.n, (char)c); o->st = 0; }
            else if (o->esc.n < 60) str_addc(&o->esc, (char)c);
            continue;
        }
        if (o->st == 3) { if (c == 7) o->st = 0; else if (c == 27) o->st = 4; continue; }   /* OSC until BEL / ST */
        if (o->st == 4) { o->st = 0; continue; }
        if (c == 27) { o->st = 1; continue; }
        if (c == '\n') { end_line(o); continue; }
        if (c == '\r') { o->col = 0; continue; }
        if (c == '\b') { if (o->col > 0) o->col--; continue; }
        if (c == '\t') { do put_cell(o, ' '); while (o->col % 8); continue; }
        if (c == 7 || c < 32) continue;
        if (c < 0x80) { put_cell(o, c); continue; }
        /* UTF-8: collect */
        o->pend[o->npend++] = c;
        int need = (o->pend[0] >= 0xF0) ? 4 : (o->pend[0] >= 0xE0) ? 3 : 2;
        if (o->npend >= need || (o->npend > 1 && (c & 0xC0) != 0x80)) { uint32_t cp; utf8_decode((const char *)o->pend, (size_t)o->npend, &cp); o->npend = 0; put_cell(o, cp); }
    }
}
void out_add_link(Output *o, const char *file, int line, int col, const char *text, Color fg) {
    if (o->col != 0 || o->lines[o->n - 1].n) end_line(o);
    if (o->ndiag == o->dcap) { o->dcap = o->dcap ? o->dcap * 2 : 32; o->diags = xrealloc(o->diags, (size_t)o->dcap * sizeof(Diag)); }
    Diag *d = &o->diags[o->ndiag]; d->file = xstrdup(file); d->line = line; d->col = col; d->sev = 2; d->msg = xstrdup(text);
    Color sf = o->fg; o->fg = fg; out_feed(o, text, (int)strlen(text)); o->fg = sf;
    o->lines[o->n - 1].diag = o->ndiag++; new_line(o);
}
void out_puts(Output *o, const char *s, Color fg, uint8_t attr) {
    if (o->col != 0 || o->lines[o->n - 1].n) end_line(o);
    Color sf = o->fg; uint8_t sa = o->attr; o->fg = fg; o->attr = attr;
    out_feed(o, s, (int)strlen(s)); o->fg = sf; o->attr = sa;
    end_line(o);
}
