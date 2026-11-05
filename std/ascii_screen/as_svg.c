/* std.ascii_screen — SVG -> RGBA rasteriser (feeds the image -> ASCII renderers).
 *
 * Supported: svg/g/use/defs, rect (rx/ry), circle, ellipse, line, polyline, polygon, path (M L H V C S Q T A Z, absolute+relative),
 * fill / stroke / stroke-width / opacity / fill-opacity / stroke-opacity / fill-rule / stroke-linecap, transform lists,
 * viewBox + preserveAspectRatio(meet), linear + radial gradients (with stops, href inheritance), <style> rules
 * (tag, .class, #id selectors), named / hex / rgb() colours, inline style="".
 * Not supported: <text>, filters, masks, clip paths, patterns, markers (they are skipped, never an error).
 */
#include "as_internal.h"

typedef struct Node { char *name; char **ak, **av; int na; struct Node **kids; int nk, capk; struct Node *parent; char *text; } Node;
typedef struct { double a, b, c, d, e, f; } Mat;
typedef struct { char *sel; char *body; } CssRule;
typedef struct { Node *root; CssRule *rules; int nrules; Node **ids; char **idn; int nids; } Doc;

static Mat m_ident(void) { Mat m = { 1, 0, 0, 1, 0, 0 }; return m; }
static Mat m_mul(Mat p, Mat q) { Mat r = { p.a * q.a + p.c * q.b, p.b * q.a + p.d * q.b, p.a * q.c + p.c * q.d, p.b * q.c + p.d * q.d, p.a * q.e + p.c * q.f + p.e, p.b * q.e + p.d * q.f + p.f }; return r; }
static void m_apply(Mat m, double x, double y, double *ox, double *oy) { *ox = m.a * x + m.c * y + m.e; *oy = m.b * x + m.d * y + m.f; }
static bool m_inv(Mat m, Mat *o) { double det = m.a * m.d - m.b * m.c; if (fabs(det) < 1e-12) return false; o->a = m.d / det; o->b = -m.b / det; o->c = -m.c / det; o->d = m.a / det; o->e = (m.c * m.f - m.d * m.e) / det; o->f = (m.b * m.e - m.a * m.f) / det; return true; }

/* ------------------------------------------------------------ XML */
static Node *node_new(const char *n, size_t l, Node *parent) { Node *x = kc_malloc(sizeof(Node)); memset(x, 0, sizeof *x); x->name = kc_malloc(l + 1); memcpy(x->name, n, l); x->name[l] = 0; x->parent = parent;
    if (parent) { if (parent->nk >= parent->capk) { parent->capk = parent->capk ? parent->capk * 2 : 4; parent->kids = kc_realloc(parent->kids, (size_t)parent->capk * sizeof(Node *)); } parent->kids[parent->nk++] = x; } return x; }
static void node_attr(Node *n, const char *k, size_t kl, const char *v, size_t vl) {
    n->ak = kc_realloc(n->ak, (size_t)(n->na + 1) * sizeof(char *)); n->av = kc_realloc(n->av, (size_t)(n->na + 1) * sizeof(char *));
    n->ak[n->na] = kc_malloc(kl + 1); memcpy(n->ak[n->na], k, kl); n->ak[n->na][kl] = 0; n->av[n->na] = kc_malloc(vl + 1); memcpy(n->av[n->na], v, vl); n->av[n->na][vl] = 0; n->na++;
}
static const char *attr(const Node *n, const char *k) { for (int i = 0; i < n->na; i++) if (!strcmp(n->ak[i], k)) return n->av[i]; return NULL; }
static const char *local(const char *s) { const char *c = strchr(s, ':'); return c ? c + 1 : s; }
static void node_free(Node *n) { for (int i = 0; i < n->nk; i++) node_free(n->kids[i]); for (int i = 0; i < n->na; i++) { free(n->ak[i]); free(n->av[i]); } free(n->ak); free(n->av); free(n->kids); free(n->name); free(n->text); free(n); }
static Node *xml_parse(const char *s, size_t n, char *err) {
    Node *root = node_new("#root", 5, NULL), *cur = root; size_t i = 0;
    while (i < n) {
        if (s[i] != '<') { size_t j = i; while (j < n && s[j] != '<') j++; if (cur && !strcmp(cur->name, "style")) { size_t l = j - i; cur->text = kc_realloc(cur->text, (cur->text ? strlen(cur->text) : 0) + l + 1); size_t o = cur->text[0] ? strlen(cur->text) : 0; memcpy(cur->text + o, s + i, l); cur->text[o + l] = 0; } i = j; continue; }
        if (i + 3 < n && !strncmp(s + i, "<!--", 4)) { const char *e = strstr(s + i + 4, "-->"); if (!e) break; i = (size_t)(e - s) + 3; continue; }
        if (i + 8 < n && !strncmp(s + i, "<![CDATA[", 9)) { const char *e = strstr(s + i, "]]>"); if (!e) break; if (cur && !strcmp(cur->name, "style")) { size_t l = (size_t)(e - s) - (i + 9); cur->text = kc_realloc(cur->text, (cur->text ? strlen(cur->text) : 0) + l + 1); size_t o = cur->text[0] ? strlen(cur->text) : 0; memcpy(cur->text + o, s + i + 9, l); cur->text[o + l] = 0; } i = (size_t)(e - s) + 3; continue; }
        if (s[i + 1] == '?' || s[i + 1] == '!') { int depth = 0; while (i < n) { if (s[i] == '<') depth++; else if (s[i] == '>') { depth--; if (depth <= 0) { i++; break; } } i++; } continue; }
        if (s[i + 1] == '/') { size_t j = i; while (j < n && s[j] != '>') j++; if (cur->parent) cur = cur->parent; i = j + 1; continue; }
        size_t j = i + 1, ns = j; while (j < n && !isspace((unsigned char)s[j]) && s[j] != '>' && s[j] != '/') j++;
        Node *nd = node_new(s + ns, j - ns, cur); bool selfclose = false;
        while (j < n && s[j] != '>') {
            if (isspace((unsigned char)s[j])) { j++; continue; }
            if (s[j] == '/') { selfclose = true; j++; continue; }
            size_t ks = j; while (j < n && s[j] != '=' && !isspace((unsigned char)s[j]) && s[j] != '>' && s[j] != '/') j++;
            size_t ke = j; while (j < n && isspace((unsigned char)s[j])) j++;
            if (j < n && s[j] == '=') { j++; while (j < n && isspace((unsigned char)s[j])) j++; if (j < n && (s[j] == '"' || s[j] == '\'')) { char q = s[j++]; size_t vs = j; while (j < n && s[j] != q) j++; node_attr(nd, s + ks, ke - ks, s + vs, j - vs); j++; } }
            else if (ke == ks) j++;
        }
        i = j + 1; if (!selfclose) cur = nd;
    }
    if (root->nk == 0) { snprintf(err, 200, "SVG: no elements found"); node_free(root); return NULL; }
    return root;
}
/* ----------------------------------------------------------- CSS + props */
static void css_parse(Doc *d, const char *t) {
    while (*t) {
        const char *b = strchr(t, '{'); if (!b) break; const char *e = strchr(b, '}'); if (!e) break;
        size_t sl = (size_t)(b - t); char *sel = kc_malloc(sl + 1); memcpy(sel, t, sl); sel[sl] = 0; char *body = kc_malloc((size_t)(e - b)); memcpy(body, b + 1, (size_t)(e - b - 1)); body[e - b - 1] = 0;
        char *c = strtok(sel, ",");
        while (c) { while (isspace((unsigned char)*c)) c++; char *te = c + strlen(c); while (te > c && isspace((unsigned char)te[-1])) *--te = 0;
            d->rules = kc_realloc(d->rules, (size_t)(d->nrules + 1) * sizeof(CssRule)); d->rules[d->nrules].sel = kc_malloc(strlen(c) + 1); strcpy(d->rules[d->nrules].sel, c); d->rules[d->nrules].body = kc_malloc(strlen(body) + 1); strcpy(d->rules[d->nrules].body, body); d->nrules++; c = strtok(NULL, ","); }
        free(sel); free(body); t = e + 1;
    }
}
static bool decl_get(const char *decls, const char *prop, char *out, size_t cap) {
    size_t pl = strlen(prop); const char *p = decls;
    while (*p) {
        while (*p == ' ' || *p == ';' || *p == '\n' || *p == '\t' || *p == '\r') p++;
        const char *colon = strchr(p, ':'); if (!colon) return false; const char *semi = strchr(colon, ';'); if (!semi) semi = p + strlen(p);
        const char *ke = colon; while (ke > p && isspace((unsigned char)ke[-1])) ke--;
        if ((size_t)(ke - p) == pl && !strncmp(p, prop, pl)) { const char *vs = colon + 1; while (isspace((unsigned char)*vs)) vs++; const char *ve = semi; while (ve > vs && isspace((unsigned char)ve[-1])) ve--; size_t l = (size_t)(ve - vs); if (l >= cap) l = cap - 1; memcpy(out, vs, l); out[l] = 0; return true; }
        p = *semi ? semi + 1 : semi;
    }
    return false;
}
static bool class_has(const char *classes, const char *c) { size_t l = strlen(c); const char *p = classes; while (*p) { while (isspace((unsigned char)*p)) p++; const char *e = p; while (*e && !isspace((unsigned char)*e)) e++; if ((size_t)(e - p) == l && !strncmp(p, c, l)) return true; p = e; } return false; }
static bool get_prop(Doc *d, const Node *n, const char *prop, char *out, size_t cap) {
    const char *st = attr(n, "style"); if (st && decl_get(st, prop, out, cap)) return true;
    const char *cls = attr(n, "class"), *id = attr(n, "id");
    for (int i = d->nrules - 1; i >= 0; i--) { const char *s = d->rules[i].sel; bool m = false;
        if (s[0] == '.') m = cls && class_has(cls, s + 1); else if (s[0] == '#') m = id && !strcmp(id, s + 1); else m = !strcmp(s, local(n->name)) || !strcmp(s, "*");
        if (m && decl_get(d->rules[i].body, prop, out, cap)) return true; }
    const char *a = attr(n, prop); if (a) { snprintf(out, cap, "%s", a); return true; }
    return false;
}
/* ------------------------------------------------------------ paint */
static const struct { const char *n; uint32_t c; } SVGC[] = { {"black",0x000000},{"white",0xFFFFFF},{"red",0xFF0000},{"green",0x008000},{"blue",0x0000FF},{"yellow",0xFFFF00},{"cyan",0x00FFFF},{"aqua",0x00FFFF},{"magenta",0xFF00FF},{"fuchsia",0xFF00FF},
    {"gray",0x808080},{"grey",0x808080},{"silver",0xC0C0C0},{"maroon",0x800000},{"olive",0x808000},{"lime",0x00FF00},{"teal",0x008080},{"navy",0x000080},{"purple",0x800080},{"orange",0xFFA500},{"pink",0xFFC0CB},{"brown",0xA52A2A},
    {"gold",0xFFD700},{"darkred",0x8B0000},{"darkgreen",0x006400},{"darkblue",0x00008B},{"lightblue",0xADD8E6},{"lightgreen",0x90EE90},{"lightgray",0xD3D3D3},{"lightgrey",0xD3D3D3},{"darkgray",0xA9A9A9},{"darkgrey",0xA9A9A9},{"skyblue",0x87CEEB},
    {"coral",0xFF7F50},{"crimson",0xDC143C},{"indigo",0x4B0082},{"violet",0xEE82EE},{"tan",0xD2B48C},{"beige",0xF5F5DC},{"ivory",0xFFFFF0},{"khaki",0xF0E68C},{"salmon",0xFA8072},{"turquoise",0x40E0D0},{"orchid",0xDA70D6},{"plum",0xDDA0DD},{"tomato",0xFF6347},
    {"steelblue",0x4682B4},{"royalblue",0x4169E1},{"forestgreen",0x228B22},{"seagreen",0x2E8B57},{"chocolate",0xD2691E},{"sienna",0xA0522D},{"slategray",0x708090},{"slategrey",0x708090},{"orangered",0xFF4500},{"hotpink",0xFF69B4},{"deeppink",0xFF1493},
    {"dodgerblue",0x1E90FF},{"firebrick",0xB22222},{"goldenrod",0xDAA520},{"limegreen",0x32CD32},{"midnightblue",0x191970},{"darkorange",0xFF8C00},{"whitesmoke",0xF5F5F5},{"gainsboro",0xDCDCDC},{"lavender",0xE6E6FA},{"mintcream",0xF5FFFA},{"cornflowerblue",0x6495ED} };
typedef struct { int type; Col c; char id[64]; } Paint;      /* 0 none, 1 colour, 2 gradient */
static bool parse_svg_color(const char *s, Col *out, float *alpha) {
    while (isspace((unsigned char)*s)) s++;
    if (!strncmp(s, "rgb", 3)) { const char *p = strchr(s, '('); if (!p) return false; double v[4] = { 0, 0, 0, 1 }; int k = 0; p++; while (*p && *p != ')' && k < 4) { char *e; double x = strtod(p, &e); if (e == p) { p++; continue; } if (*e == '%') { x = x * 2.55; e++; } v[k++] = x; p = e; while (*p == ',' || *p == ' ' || *p == '/') p++; } *out = col_rgb((int)v[0], (int)v[1], (int)v[2]); if (alpha && k >= 4) *alpha = (float)v[3]; return true; }
    if (s[0] != '#') { char low[32]; size_t i = 0; for (; s[i] && !isspace((unsigned char)s[i]) && i < 31; i++) low[i] = (char)tolower((unsigned char)s[i]); low[i] = 0;
        for (size_t k = 0; k < sizeof SVGC / sizeof SVGC[0]; k++) if (!strcmp(SVGC[k].n, low)) { *out = COL_SET | SVGC[k].c; return true; } }
    return col_parse(s, out) && *out != 0;
}
static Paint parse_paint(const char *s, Col inherit_cc) {
    Paint p; memset(&p, 0, sizeof p);
    while (isspace((unsigned char)*s)) s++;
    if (!*s || !strncmp(s, "none", 4)) return p;
    if (!strncmp(s, "url(", 4)) { const char *h = strchr(s, '#'); if (h) { size_t l = 0; h++; while (h[l] && h[l] != ')' && h[l] != '\'' && h[l] != '"' && l < 63) l++; memcpy(p.id, h, l); p.id[l] = 0; p.type = 2; } return p; }
    if (!strcmp(s, "currentColor")) { p.type = 1; p.c = inherit_cc ? inherit_cc : col_rgb(0, 0, 0); return p; }
    Col c; if (parse_svg_color(s, &c, NULL)) { p.type = 1; p.c = c; }
    return p;
}
/* ------------------------------------------------------------ geometry */
typedef struct { double *pts; int n, cap; bool closed; } Sub;
typedef struct { Sub *s; int n, cap; } Path;
static void path_free(Path *p) { for (int i = 0; i < p->n; i++) free(p->s[i].pts); free(p->s); p->s = NULL; p->n = p->cap = 0; }
static Sub *path_begin(Path *p) { if (p->n >= p->cap) { p->cap = p->cap ? p->cap * 2 : 4; p->s = kc_realloc(p->s, (size_t)p->cap * sizeof(Sub)); } Sub *s = &p->s[p->n++]; memset(s, 0, sizeof *s); return s; }
static void sub_add(Sub *s, double x, double y) { if (s->n >= s->cap) { s->cap = s->cap ? s->cap * 2 : 16; s->pts = kc_realloc(s->pts, (size_t)s->cap * 2 * sizeof(double)); } s->pts[s->n * 2] = x; s->pts[s->n * 2 + 1] = y; s->n++; }
static int flat_n(double len, double scale) { int n = (int)ceil(len * scale / 3.0); return as_clampi(n, 6, 80); }
static void cubic(Sub *s, double x0, double y0, double x1, double y1, double x2, double y2, double x3, double y3, double sc) {
    double len = hypot(x1 - x0, y1 - y0) + hypot(x2 - x1, y2 - y1) + hypot(x3 - x2, y3 - y2); int n = flat_n(len, sc);
    for (int i = 1; i <= n; i++) { double t = (double)i / n, u = 1 - t; sub_add(s, u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3, u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3); }
}
static void arc_to(Sub *s, double x0, double y0, double rx, double ry, double phi, int fa, int fs, double x1, double y1, double sc) {
    if (rx == 0 || ry == 0 || (x0 == x1 && y0 == y1)) { sub_add(s, x1, y1); return; }
    rx = fabs(rx); ry = fabs(ry); double cp = cos(phi * M_PI / 180), sp = sin(phi * M_PI / 180), dx = (x0 - x1) / 2, dy = (y0 - y1) / 2, x1p = cp * dx + sp * dy, y1p = -sp * dx + cp * dy;
    double lam = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry); if (lam > 1) { rx *= sqrt(lam); ry *= sqrt(lam); }
    double num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p, den = rx * rx * y1p * y1p + ry * ry * x1p * x1p, co = den == 0 ? 0 : sqrt(fmax(0, num / den)); if (fa == fs) co = -co;
    double cxp = co * rx * y1p / ry, cyp = -co * ry * x1p / rx, cx = cp * cxp - sp * cyp + (x0 + x1) / 2, cy = sp * cxp + cp * cyp + (y0 + y1) / 2;
    double a1 = atan2((y1p - cyp) / ry, (x1p - cxp) / rx), a2 = atan2((-y1p - cyp) / ry, (-x1p - cxp) / rx), da = a2 - a1;
    if (!fs && da > 0) da -= 2 * M_PI; else if (fs && da < 0) da += 2 * M_PI;
    int n = flat_n(fabs(da) * fmax(rx, ry), sc); if (n < 8) n = 8;
    for (int i = 1; i <= n; i++) { double a = a1 + da * i / n, ex = rx * cos(a), ey = ry * sin(a); sub_add(s, cp * ex - sp * ey + cx, sp * ex + cp * ey + cy); }
}
static const char *skipws(const char *p) { while (*p && (isspace((unsigned char)*p) || *p == ',')) p++; return p; }
static bool num_next(const char **pp, double *v) { const char *p = skipws(*pp); char *e; if (!*p) return false; *v = strtod(p, &e); if (e == p) return false; *pp = e; return true; }
static void path_parse(Path *out, const char *d, double sc) {
    double cx = 0, cy = 0, sx = 0, sy = 0, lcx = 0, lcy = 0; char cmd = 0, prev = 0; Sub *cur = NULL; const char *p = d;
    while (1) {
        p = skipws(p); if (!*p) break;
        if (isalpha((unsigned char)*p)) { cmd = *p++; } else if (!cmd) break;
        bool rel = islower((unsigned char)cmd); char C = (char)toupper((unsigned char)cmd); double v[7];
        if (C == 'Z') { if (cur) { cur->closed = true; cx = sx; cy = sy; } prev = 'Z'; continue; }
        int need = C == 'M' || C == 'L' || C == 'T' ? 2 : C == 'H' || C == 'V' ? 1 : C == 'C' ? 6 : C == 'S' || C == 'Q' ? 4 : C == 'A' ? 7 : 0; if (!need) break;
        for (int i = 0; i < need; i++) {
            if (C == 'A' && (i == 3 || i == 4)) { p = skipws(p); if (*p != '0' && *p != '1') return; v[i] = *p - '0'; p++; }
            else if (!num_next(&p, &v[i])) return;
        }
        double ox = rel ? cx : 0, oy = rel ? cy : 0;
        switch (C) {
            case 'M': cx = v[0] + ox; cy = v[1] + oy; sx = cx; sy = cy; cur = path_begin(out); sub_add(cur, cx, cy); cmd = rel ? 'l' : 'L'; lcx = cx; lcy = cy; break;
            case 'L': if (!cur || prev == 'Z') { cur = path_begin(out); sub_add(cur, cx, cy); } cx = v[0] + ox; cy = v[1] + oy; sub_add(cur, cx, cy); lcx = cx; lcy = cy; break;
            case 'H': if (!cur || prev == 'Z') { cur = path_begin(out); sub_add(cur, cx, cy); } cx = v[0] + ox; sub_add(cur, cx, cy); lcx = cx; lcy = cy; break;
            case 'V': if (!cur || prev == 'Z') { cur = path_begin(out); sub_add(cur, cx, cy); } cy = v[0] + oy; sub_add(cur, cx, cy); lcx = cx; lcy = cy; break;
            case 'C': { if (!cur || prev == 'Z') { cur = path_begin(out); sub_add(cur, cx, cy); } double x1 = v[0] + ox, y1 = v[1] + oy, x2 = v[2] + ox, y2 = v[3] + oy, x3 = v[4] + ox, y3 = v[5] + oy; cubic(cur, cx, cy, x1, y1, x2, y2, x3, y3, sc); lcx = x2; lcy = y2; cx = x3; cy = y3; break; }
            case 'S': { if (!cur || prev == 'Z') { cur = path_begin(out); sub_add(cur, cx, cy); } double x1 = (prev == 'C' || prev == 'S') ? 2 * cx - lcx : cx, y1 = (prev == 'C' || prev == 'S') ? 2 * cy - lcy : cy, x2 = v[0] + ox, y2 = v[1] + oy, x3 = v[2] + ox, y3 = v[3] + oy; cubic(cur, cx, cy, x1, y1, x2, y2, x3, y3, sc); lcx = x2; lcy = y2; cx = x3; cy = y3; break; }
            case 'Q': { if (!cur || prev == 'Z') { cur = path_begin(out); sub_add(cur, cx, cy); } double qx = v[0] + ox, qy = v[1] + oy, x3 = v[2] + ox, y3 = v[3] + oy; cubic(cur, cx, cy, cx + 2.0 / 3 * (qx - cx), cy + 2.0 / 3 * (qy - cy), x3 + 2.0 / 3 * (qx - x3), y3 + 2.0 / 3 * (qy - y3), x3, y3, sc); lcx = qx; lcy = qy; cx = x3; cy = y3; break; }
            case 'T': { if (!cur || prev == 'Z') { cur = path_begin(out); sub_add(cur, cx, cy); } double qx = (prev == 'Q' || prev == 'T') ? 2 * cx - lcx : cx, qy = (prev == 'Q' || prev == 'T') ? 2 * cy - lcy : cy, x3 = v[0] + ox, y3 = v[1] + oy; cubic(cur, cx, cy, cx + 2.0 / 3 * (qx - cx), cy + 2.0 / 3 * (qy - cy), x3 + 2.0 / 3 * (qx - x3), y3 + 2.0 / 3 * (qy - y3), x3, y3, sc); lcx = qx; lcy = qy; cx = x3; cy = y3; break; }
            case 'A': { if (!cur || prev == 'Z') { cur = path_begin(out); sub_add(cur, cx, cy); } double x1 = v[5] + ox, y1 = v[6] + oy; arc_to(cur, cx, cy, v[0], v[1], v[2], (int)v[3], (int)v[4], x1, y1, sc); cx = x1; cy = y1; lcx = cx; lcy = cy; break; }
        }
        prev = C;
    }
}
/* ------------------------------------------------------------ raster */
typedef struct { int w, h; float *px; } Surf;                    /* premultiplied RGBA 0..1 */
typedef struct { double x0, y0, x1, y1; int dir; } Edge;
static int cmp_cross(const void *a, const void *b) { double x = ((const double *)a)[0], y = ((const double *)b)[0]; return x < y ? -1 : x > y; }
/* cov: w*h floats (0..1) for the filled region of the transformed path */
static void fill_cov(const Path *p, Mat m, bool evenodd, int W, int H, float *cov, int *bx0, int *by0, int *bx1, int *by1) {
    int ne = 0, cap = 64; Edge *E = kc_malloc((size_t)cap * sizeof(Edge)); double miny = 1e30, maxy = -1e30, minx = 1e30, maxx = -1e30;
    for (int i = 0; i < p->n; i++) {
        const Sub *s = &p->s[i]; if (s->n < 2) continue;
        for (int k = 0; k < s->n; k++) {
            double ax, ay, bx, by; m_apply(m, s->pts[k * 2], s->pts[k * 2 + 1], &ax, &ay); int k2 = (k + 1) % s->n; m_apply(m, s->pts[k2 * 2], s->pts[k2 * 2 + 1], &bx, &by);
            if (ax < minx) minx = ax; if (ax > maxx) maxx = ax; if (ay < miny) miny = ay; if (ay > maxy) maxy = ay;
            if (ay == by) continue; if (ne >= cap) { cap *= 2; E = kc_realloc(E, (size_t)cap * sizeof(Edge)); }
            if (ay < by) E[ne++] = (Edge){ ax, ay, bx, by, 1 }; else E[ne++] = (Edge){ bx, by, ax, ay, -1 };
        }
    }
    *bx0 = (int)floor(minx); *bx1 = (int)ceil(maxx); *by0 = (int)floor(miny); *by1 = (int)ceil(maxy);
    if (ne == 0) { free(E); *bx1 = *bx0 - 1; return; }
    int y0 = as_clampi(*by0, 0, H - 1), y1 = as_clampi(*by1, 0, H - 1); double *cr = kc_malloc((size_t)(ne * 2 + 2) * sizeof(double) * 2);
    const int SS = 4;
    for (int y = y0; y <= y1; y++) for (int s = 0; s < SS; s++) {
        double sy = y + (s + 0.5) / SS; int nc = 0;
        for (int e = 0; e < ne; e++) if (sy >= E[e].y0 && sy < E[e].y1) { double t = (sy - E[e].y0) / (E[e].y1 - E[e].y0); cr[nc * 2] = E[e].x0 + (E[e].x1 - E[e].x0) * t; cr[nc * 2 + 1] = E[e].dir; nc++; }
        if (nc < 2) continue; qsort(cr, (size_t)nc, 2 * sizeof(double), cmp_cross);
        int wind = 0;
        for (int k = 0; k + 1 < nc; k++) {
            wind += (int)cr[k * 2 + 1]; bool in = evenodd ? ((k + 1) & 1) : wind != 0; if (!in) continue;
            double xa = cr[k * 2], xb = cr[(k + 1) * 2]; if (xb <= xa) continue; if (xa < 0) xa = 0; if (xb > W) xb = W; if (xb <= xa) continue;
            int ia = (int)floor(xa), ib = (int)floor(xb); float *row = cov + (size_t)y * W;
            if (ia == ib) row[ia] += (float)((xb - xa) / SS);
            else { row[ia] += (float)((ia + 1 - xa) / SS); for (int x = ia + 1; x < ib; x++) row[x] += 1.0f / SS; if (ib < W) row[ib] += (float)((xb - ib) / SS); }
        }
    }
    free(cr); free(E);
}
static void stroke_cov(const Path *p, Mat m, double w, int cap_style, int W, int H, float *cov, int *bx0, int *by0, int *bx1, int *by1) {
    double hw = w / 2; *bx0 = W; *by0 = H; *bx1 = -1; *by1 = -1;
    for (int i = 0; i < p->n; i++) {
        const Sub *s = &p->s[i]; if (s->n < 1) continue; int segs = s->closed ? s->n : s->n - 1; if (s->n == 1) segs = 1;
        for (int k = 0; k < segs; k++) {
            double ax, ay, bx, by; m_apply(m, s->pts[k * 2], s->pts[k * 2 + 1], &ax, &ay); int k2 = (k + 1) % s->n; m_apply(m, s->pts[k2 * 2], s->pts[k2 * 2 + 1], &bx, &by);
            double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy, len = sqrt(len2);
            bool start_cap = !s->closed && k == 0, end_cap = !s->closed && k == segs - 1;
            double ext = (cap_style == 2) ? hw : 0;                                    /* square caps extend */
            int x0 = (int)floor(fmin(ax, bx) - hw - 1 - ext), x1 = (int)ceil(fmax(ax, bx) + hw + 1 + ext), y0 = (int)floor(fmin(ay, by) - hw - 1 - ext), y1 = (int)ceil(fmax(ay, by) + hw + 1 + ext);
            x0 = as_clampi(x0, 0, W - 1); x1 = as_clampi(x1, 0, W - 1); y0 = as_clampi(y0, 0, H - 1); y1 = as_clampi(y1, 0, H - 1);
            if (x0 < *bx0) *bx0 = x0; if (x1 > *bx1) *bx1 = x1; if (y0 < *by0) *by0 = y0; if (y1 > *by1) *by1 = y1;
            for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
                double px = x + 0.5 - ax, py = y + 0.5 - ay, t = len2 > 0 ? (px * dx + py * dy) / len2 : 0, d;
                if (len2 > 0 && ((start_cap && t < 0 && cap_style != 1) || (end_cap && t > 1 && cap_style != 1))) { double tt = t < 0 ? t * len : (t - 1) * len; if (cap_style == 0 || fabs(tt) > ext) { if (cap_style == 0) continue; } }
                double tc = t < 0 ? 0 : t > 1 ? 1 : t; if (len2 == 0) tc = 0;
                double qx = px - dx * tc, qy = py - dy * tc; d = sqrt(qx * qx + qy * qy);
                if (cap_style == 2 && len2 > 0 && ((start_cap && t < 0) || (end_cap && t > 1))) { double across = fabs(px * dy - py * dx) / len, along = (t < 0 ? -t : t - 1) * len; d = fmax(across, along); }
                float c = (float)as_clampf((float)(hw - d + 0.5), 0, 1); size_t o = (size_t)y * W + x; if (c > cov[o]) cov[o] = c;
            }
        }
    }
}
/* gradients */
typedef struct { float off; Col c; float a; } Stop;
typedef struct { bool radial, bbox; double x1, y1, x2, y2, cx, cy, r, fx, fy; Stop *st; int nst; } Grad;
static double len_attr(const char *s, double def, double ref, bool *pct) { if (!s) return def; char *e; double v = strtod(s, &e); if (*e == '%') { if (pct) *pct = true; return v / 100.0 * (ref ? ref : 1); } return v; }
static Node *find_id(Doc *d, const char *id) { for (int i = 0; i < d->nids; i++) if (!strcmp(d->idn[i], id)) return d->ids[i]; return NULL; }
static bool grad_load(Doc *d, const char *id, Grad *g) {
    Node *n = find_id(d, id); if (!n) return false; const char *nm = local(n->name);
    if (strcmp(nm, "linearGradient") && strcmp(nm, "radialGradient")) return false;
    memset(g, 0, sizeof *g); g->radial = !strcmp(nm, "radialGradient"); g->bbox = true;
    Node *chain[8]; int nc = 0; Node *c = n; while (c && nc < 8) { chain[nc++] = c; const char *h = attr(c, "href"); if (!h) h = attr(c, "xlink:href"); c = (h && h[0] == '#') ? find_id(d, h + 1) : NULL; }
    const char *u = NULL, *x1 = NULL, *y1 = NULL, *x2 = NULL, *y2 = NULL, *cx = NULL, *cy = NULL, *r = NULL, *fx = NULL, *fy = NULL;
    for (int i = nc - 1; i >= 0; i--) { const char *t; if ((t = attr(chain[i], "gradientUnits"))) u = t; if ((t = attr(chain[i], "x1"))) x1 = t; if ((t = attr(chain[i], "y1"))) y1 = t; if ((t = attr(chain[i], "x2"))) x2 = t; if ((t = attr(chain[i], "y2"))) y2 = t; if ((t = attr(chain[i], "cx"))) cx = t; if ((t = attr(chain[i], "cy"))) cy = t; if ((t = attr(chain[i], "r"))) r = t; if ((t = attr(chain[i], "fx"))) fx = t; if ((t = attr(chain[i], "fy"))) fy = t; }
    g->bbox = !(u && !strcmp(u, "userSpaceOnUse")); double ref = g->bbox ? 1 : 100;
    g->x1 = len_attr(x1, 0, ref, NULL); g->y1 = len_attr(y1, 0, ref, NULL); g->x2 = len_attr(x2, g->bbox ? 1 : 100, ref, NULL); g->y2 = len_attr(y2, 0, ref, NULL);
    g->cx = len_attr(cx, g->bbox ? 0.5 : 50, ref, NULL); g->cy = len_attr(cy, g->bbox ? 0.5 : 50, ref, NULL); g->r = len_attr(r, g->bbox ? 0.5 : 50, ref, NULL); g->fx = len_attr(fx, g->cx, ref, NULL); g->fy = len_attr(fy, g->cy, ref, NULL);
    for (int i = 0; i < nc && g->nst == 0; i++) for (int k = 0; k < chain[i]->nk; k++) { Node *s = chain[i]->kids[k]; if (strcmp(local(s->name), "stop")) continue;
        char buf[128]; Stop st = { 0, col_rgb(0, 0, 0), 1 }; const char *o = attr(s, "offset"); if (o) { char *e; st.off = (float)strtod(o, &e); if (*e == '%') st.off /= 100; }
        if (get_prop(d, s, "stop-color", buf, sizeof buf)) { float al = 1; parse_svg_color(buf, &st.c, &al); st.a = al; } if (get_prop(d, s, "stop-opacity", buf, sizeof buf)) st.a *= (float)atof(buf);
        g->st = kc_realloc(g->st, (size_t)(g->nst + 1) * sizeof(Stop)); g->st[g->nst++] = st; }
    return g->nst > 0;
}
static void grad_color(const Grad *g, double t, float *r, float *gg, float *b, float *a) {
    t = t < 0 ? 0 : t > 1 ? 1 : t; int i = 0; while (i + 1 < g->nst && t > g->st[i + 1].off) i++;
    const Stop *s0 = &g->st[i], *s1 = i + 1 < g->nst ? &g->st[i + 1] : s0; float f = s1->off > s0->off ? (float)((t - s0->off) / (s1->off - s0->off)) : 0; if (f < 0) f = 0; if (f > 1) f = 1; if (t <= g->st[0].off) f = 0;
    *r = ((1 - f) * COL_R(s0->c) + f * COL_R(s1->c)) / 255; *gg = ((1 - f) * COL_G(s0->c) + f * COL_G(s1->c)) / 255; *b = ((1 - f) * COL_B(s0->c) + f * COL_B(s1->c)) / 255; *a = (1 - f) * s0->a + f * s1->a;
}
static void composite(Surf *S, const float *cov, int x0, int y0, int x1, int y1, const Paint *pt, Doc *d, double opacity, Mat m, const Path *path) {
    if (x1 < x0 || y1 < y0) return;
    Grad g; memset(&g, 0, sizeof g); bool have_g = false; Mat inv = m_ident(); double bx0 = 0, by0 = 0, bw = 1, bh = 1;
    if (pt->type == 2) {
        have_g = grad_load(d, pt->id, &g); if (!have_g) return; if (!m_inv(m, &inv)) { free(g.st); return; }
        double mnx = 1e30, mny = 1e30, mxx = -1e30, mxy = -1e30; for (int i = 0; i < path->n; i++) for (int k = 0; k < path->s[i].n; k++) { double x = path->s[i].pts[k * 2], y = path->s[i].pts[k * 2 + 1]; if (x < mnx) mnx = x; if (x > mxx) mxx = x; if (y < mny) mny = y; if (y > mxy) mxy = y; }
        bx0 = mnx; by0 = mny; bw = mxx - mnx; bh = mxy - mny; if (bw <= 0) bw = 1; if (bh <= 0) bh = 1;
    }
    for (int y = y0; y <= y1 && y < S->h; y++) for (int x = x0; x <= x1 && x < S->w; x++) {
        float c = cov[(size_t)y * S->w + x]; if (c <= 0.001f) continue; if (c > 1) c = 1;
        float r, gg, b, a = 1;
        if (pt->type == 1) { r = (float)COL_R(pt->c) / 255; gg = (float)COL_G(pt->c) / 255; b = (float)COL_B(pt->c) / 255; }
        else { double ux, uy; m_apply(inv, x + 0.5, y + 0.5, &ux, &uy); if (g.bbox) { ux = (ux - bx0) / bw; uy = (uy - by0) / bh; } double t;
            if (!g.radial) { double dx = g.x2 - g.x1, dy = g.y2 - g.y1, l2 = dx * dx + dy * dy; t = l2 > 0 ? ((ux - g.x1) * dx + (uy - g.y1) * dy) / l2 : 0; } else { double dxx = ux - g.cx, dyy = uy - g.cy; t = g.r > 0 ? sqrt(dxx * dxx + dyy * dyy) / g.r : 0; }
            grad_color(&g, t, &r, &gg, &b, &a); }
        float al = c * a * (float)opacity; float *o = S->px + ((size_t)y * S->w + x) * 4;
        o[0] = r * al + o[0] * (1 - al); o[1] = gg * al + o[1] * (1 - al); o[2] = b * al + o[2] * (1 - al); o[3] = al + o[3] * (1 - al);
    }
    if (have_g) free(g.st);
}
/* ------------------------------------------------------------ render tree */
typedef struct { Mat m; Paint fill, stroke; double sw, fop, sop, op; bool evenodd; int cap; Col cc; bool hidden; } St;
typedef struct { Doc *d; Surf *s; double scale; int depth; } Ctx;
static void draw_path(Ctx *c, const Path *p, const St *st) {
    if (p->n == 0) return; int W = c->s->w, H = c->s->h; float *cov = kc_malloc((size_t)W * H * sizeof(float)); int bx0, by0, bx1, by1;
    if (st->fill.type) { memset(cov, 0, (size_t)W * H * sizeof(float)); fill_cov(p, st->m, st->evenodd, W, H, cov, &bx0, &by0, &bx1, &by1); bx0 = as_clampi(bx0, 0, W - 1); bx1 = as_clampi(bx1, 0, W - 1); by0 = as_clampi(by0, 0, H - 1); by1 = as_clampi(by1, 0, H - 1); composite(c->s, cov, bx0, by0, bx1, by1, &st->fill, c->d, st->fop * st->op, st->m, p); }
    if (st->stroke.type && st->sw > 0) { memset(cov, 0, (size_t)W * H * sizeof(float)); double sc = sqrt(fabs(st->m.a * st->m.d - st->m.b * st->m.c)); stroke_cov(p, st->m, fmax(st->sw * sc, 0.6), st->cap, W, H, cov, &bx0, &by0, &bx1, &by1); composite(c->s, cov, bx0, by0, bx1, by1, &st->stroke, c->d, st->sop * st->op, st->m, p); }
    free(cov);
}
static bool parse_transform(const char *s, Mat *out) {
    Mat m = m_ident(); const char *p = s;
    while (*p) { p = skipws(p); if (!*p) break; const char *par = strchr(p, '('); if (!par) break; char name[16]; size_t nl = (size_t)(par - p); if (nl > 15) nl = 15; memcpy(name, p, nl); name[nl] = 0; while (nl && isspace((unsigned char)name[nl - 1])) name[--nl] = 0;
        double v[6] = { 0 }; int k = 0; const char *q = par + 1; while (k < 6 && num_next(&q, &v[k])) k++; const char *e = strchr(q, ')'); p = e ? e + 1 : q + strlen(q);
        Mat t = m_ident();
        if (!strcmp(name, "translate")) { t.e = v[0]; t.f = k > 1 ? v[1] : 0; } else if (!strcmp(name, "scale")) { t.a = v[0]; t.d = k > 1 ? v[1] : v[0]; }
        else if (!strcmp(name, "rotate")) { double a = v[0] * M_PI / 180, cs = cos(a), sn = sin(a); t.a = cs; t.b = sn; t.c = -sn; t.d = cs; if (k >= 3) { Mat t1 = m_ident(), t2 = m_ident(); t1.e = v[1]; t1.f = v[2]; t2.e = -v[1]; t2.f = -v[2]; t = m_mul(t1, m_mul(t, t2)); } }
        else if (!strcmp(name, "skewX")) t.c = tan(v[0] * M_PI / 180); else if (!strcmp(name, "skewY")) t.b = tan(v[0] * M_PI / 180);
        else if (!strcmp(name, "matrix") && k == 6) { t.a = v[0]; t.b = v[1]; t.c = v[2]; t.d = v[3]; t.e = v[4]; t.f = v[5]; }
        m = m_mul(m, t); }
    *out = m; return true;
}
static void render_node(Ctx *c, Node *n, St st);
static void style_of(Ctx *c, Node *n, St *st) {
    char b[256]; Doc *d = c->d;
    const char *t = attr(n, "transform"); if (t) { Mat m; parse_transform(t, &m); st->m = m_mul(st->m, m); }
    if (get_prop(d, n, "color", b, sizeof b)) { Col cc; if (parse_svg_color(b, &cc, NULL)) st->cc = cc; }
    if (get_prop(d, n, "fill", b, sizeof b)) st->fill = parse_paint(b, st->cc);
    if (get_prop(d, n, "stroke", b, sizeof b)) st->stroke = parse_paint(b, st->cc);
    if (get_prop(d, n, "stroke-width", b, sizeof b)) st->sw = atof(b);
    if (get_prop(d, n, "fill-opacity", b, sizeof b)) st->fop = atof(b); if (get_prop(d, n, "stroke-opacity", b, sizeof b)) st->sop = atof(b);
    if (get_prop(d, n, "opacity", b, sizeof b)) st->op *= atof(b);
    if (get_prop(d, n, "fill-rule", b, sizeof b)) st->evenodd = !strcmp(b, "evenodd");
    if (get_prop(d, n, "stroke-linecap", b, sizeof b)) st->cap = !strcmp(b, "round") ? 1 : !strcmp(b, "square") ? 2 : 0;
    if (get_prop(d, n, "display", b, sizeof b) && !strcmp(b, "none")) st->hidden = true;
}
static double nattr(Node *n, const char *k, double def) { const char *a = attr(n, k); return a ? atof(a) : def; }
static void render_node(Ctx *c, Node *n, St st) {
    if (c->depth > 24) return; const char *nm = local(n->name); style_of(c, n, &st); if (st.hidden) return;
    Path p = { 0 };
    if (!strcmp(nm, "g") || !strcmp(nm, "svg") || !strcmp(nm, "a") || !strcmp(nm, "switch")) { c->depth++; for (int i = 0; i < n->nk; i++) render_node(c, n->kids[i], st); c->depth--; return; }
    if (!strcmp(nm, "use")) { const char *h = attr(n, "href"); if (!h) h = attr(n, "xlink:href"); if (h && h[0] == '#') { Node *t = find_id(c->d, h + 1); if (t) { Mat tr = m_ident(); tr.e = nattr(n, "x", 0); tr.f = nattr(n, "y", 0); st.m = m_mul(st.m, tr); c->depth++; render_node(c, t, st); c->depth--; } } return; }
    if (!strcmp(nm, "rect")) { double x = nattr(n, "x", 0), y = nattr(n, "y", 0), w = nattr(n, "width", 0), h = nattr(n, "height", 0), rx = nattr(n, "rx", -1), ry = nattr(n, "ry", -1); if (w <= 0 || h <= 0) return;
        if (rx < 0 && ry >= 0) rx = ry; if (ry < 0 && rx >= 0) ry = rx; if (rx < 0) rx = 0; if (ry < 0) ry = 0; rx = fmin(rx, w / 2); ry = fmin(ry, h / 2); Sub *s = path_begin(&p); s->closed = true;
        if (rx <= 0 || ry <= 0) { sub_add(s, x, y); sub_add(s, x + w, y); sub_add(s, x + w, y + h); sub_add(s, x, y + h); }
        else { int N = 12; for (int q = 0; q < 4; q++) { double cx = (q == 0 || q == 3) ? x + w - rx : x + rx, cy = (q < 2) ? y + ry : y + h - ry, a0 = (q == 0 ? -90 : q == 1 ? 180 : q == 2 ? 90 : 0); if (q == 0) { cx = x + w - rx; cy = y + ry; a0 = -90; } else if (q == 1) { cx = x + w - rx; cy = y + h - ry; a0 = 0; } else if (q == 2) { cx = x + rx; cy = y + h - ry; a0 = 90; } else { cx = x + rx; cy = y + ry; a0 = 180; }
            for (int i = 0; i <= N; i++) { double a = (a0 + 90.0 * i / N) * M_PI / 180; sub_add(s, cx + rx * cos(a), cy + ry * sin(a)); } } } }
    else if (!strcmp(nm, "circle") || !strcmp(nm, "ellipse")) { double cx = nattr(n, "cx", 0), cy = nattr(n, "cy", 0), rx = !strcmp(nm, "circle") ? nattr(n, "r", 0) : nattr(n, "rx", 0), ry = !strcmp(nm, "circle") ? rx : nattr(n, "ry", 0); if (rx <= 0 || ry <= 0) return;
        int N = flat_n(2 * M_PI * fmax(rx, ry), c->scale * sqrt(fabs(st.m.a * st.m.d - st.m.b * st.m.c))) * 2; Sub *s = path_begin(&p); s->closed = true; for (int i = 0; i < N; i++) { double a = 2 * M_PI * i / N; sub_add(s, cx + rx * cos(a), cy + ry * sin(a)); } }
    else if (!strcmp(nm, "line")) { Sub *s = path_begin(&p); sub_add(s, nattr(n, "x1", 0), nattr(n, "y1", 0)); sub_add(s, nattr(n, "x2", 0), nattr(n, "y2", 0)); Paint none; memset(&none, 0, sizeof none); st.fill = none; }
    else if (!strcmp(nm, "polyline") || !strcmp(nm, "polygon")) { const char *pt = attr(n, "points"); if (!pt) return; Sub *s = path_begin(&p); s->closed = !strcmp(nm, "polygon"); double x, y; const char *q = pt; while (num_next(&q, &x) && num_next(&q, &y)) sub_add(s, x, y); if (!strcmp(nm, "polyline")) s->closed = false; }
    else if (!strcmp(nm, "path")) { const char *d = attr(n, "d"); if (!d) return; path_parse(&p, d, c->scale * sqrt(fabs(st.m.a * st.m.d - st.m.b * st.m.c))); }
    else return;
    if (!strcmp(nm, "polyline") || !strcmp(nm, "path") || !strcmp(nm, "line")) { /* open subpaths must not be implicitly closed for fills' edges only; fill closes them */ }
    draw_path(c, &p, &st); path_free(&p);
}
static void collect_ids(Doc *d, Node *n) { const char *id = attr(n, "id"); if (id) { d->ids = kc_realloc(d->ids, (size_t)(d->nids + 1) * sizeof(Node *)); d->idn = kc_realloc(d->idn, (size_t)(d->nids + 1) * sizeof(char *)); d->ids[d->nids] = n; d->idn[d->nids] = (char *)id; d->nids++; } for (int i = 0; i < n->nk; i++) collect_ids(d, n->kids[i]); }
static void collect_css(Doc *d, Node *n) { if (!strcmp(local(n->name), "style") && n->text) css_parse(d, n->text); for (int i = 0; i < n->nk; i++) collect_css(d, n->kids[i]); }
static double unit_len(const char *s, double def) { if (!s) return def; char *e; double v = strtod(s, &e); if (!strncmp(e, "pt", 2)) v *= 4.0 / 3; else if (!strncmp(e, "mm", 2)) v *= 3.7795; else if (!strncmp(e, "cm", 2)) v *= 37.795; else if (!strncmp(e, "in", 2)) v *= 96; else if (*e == '%') return def; return v; }
bool svg_render(const char *src, size_t n, int w, int h, Image *out, char *err) {
    Node *root = xml_parse(src, n, err); if (!root) return false;
    Node *svg = NULL; for (int i = 0; i < root->nk; i++) if (!strcmp(local(root->kids[i]->name), "svg")) svg = root->kids[i];
    if (!svg) { snprintf(err, 200, "SVG: no <svg> root element"); node_free(root); return false; }
    Doc d; memset(&d, 0, sizeof d); d.root = root; collect_ids(&d, root); collect_css(&d, root);
    double vx = 0, vy = 0, vw = 0, vh = 0; const char *vb = attr(svg, "viewBox");
    if (vb) { const char *q = vb; double a[4]; int k = 0; while (k < 4 && num_next(&q, &a[k])) k++; if (k == 4) { vx = a[0]; vy = a[1]; vw = a[2]; vh = a[3]; } }
    double aw = unit_len(attr(svg, "width"), 0), ah = unit_len(attr(svg, "height"), 0);
    if (vw <= 0 || vh <= 0) { vw = aw > 0 ? aw : 300; vh = ah > 0 ? ah : 150; vx = vy = 0; }
    double nw = aw > 0 ? aw : vw, nh = ah > 0 ? ah : vh;
    if (w <= 0 && h <= 0) { double mx = fmax(nw, nh), k = mx < 512 ? 512 / mx : mx > 1600 ? 1600 / mx : 1; w = (int)ceil(nw * k); h = (int)ceil(nh * k); }
    else if (w > 0 && h <= 0) h = (int)ceil(w * vh / vw); else if (h > 0 && w <= 0) w = (int)ceil(h * vw / vh);
    w = as_clampi(w, 1, 4096); h = as_clampi(h, 1, 4096);
    double sc = fmin(w / vw, h / vh); Mat base = m_ident(); base.a = sc; base.d = sc; base.e = (w - vw * sc) / 2 - vx * sc; base.f = (h - vh * sc) / 2 - vy * sc;
    Surf S = { w, h, kc_malloc((size_t)w * h * 4 * sizeof(float)) }; memset(S.px, 0, (size_t)w * h * 4 * sizeof(float));
    St st; memset(&st, 0, sizeof st); st.m = base; st.fill.type = 1; st.fill.c = col_rgb(0, 0, 0); st.sw = 1; st.fop = st.sop = st.op = 1; st.cc = col_rgb(0, 0, 0);
    Ctx c = { &d, &S, 1.0, 0 };
    for (int i = 0; i < svg->nk; i++) { const char *nm = local(svg->kids[i]->name); if (!strcmp(nm, "defs") || !strcmp(nm, "style") || !strcmp(nm, "symbol")) continue; render_node(&c, svg->kids[i], st); }
    out->w = w; out->h = h; out->px = kc_malloc((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++) { float a = S.px[i * 4 + 3], k = a > 0 ? 1.0f / a : 0; out->px[i] = (uint32_t)as_clampi((int)(a * 255 + 0.5f), 0, 255) << 24 | (uint32_t)as_clampi((int)(S.px[i * 4] * k * 255 + 0.5f), 0, 255) << 16 | (uint32_t)as_clampi((int)(S.px[i * 4 + 1] * k * 255 + 0.5f), 0, 255) << 8 | (uint32_t)as_clampi((int)(S.px[i * 4 + 2] * k * 255 + 0.5f), 0, 255); }
    free(S.px); for (int i = 0; i < d.nrules; i++) { free(d.rules[i].sel); free(d.rules[i].body); } free(d.rules); free(d.ids); free(d.idn); node_free(root);
    return true;
}
