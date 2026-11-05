/* std.game — model file loaders.
 *
 *   OBJ (+MTL)         text, polygons, materials, textures
 *   STL                binary + ASCII
 *   PLY                ascii + binary (little/big endian), vertex colours
 *   glTF 2.0 / GLB     node hierarchy, PBR base colour, embedded or external images
 *   FBX                binary (v7.x, zlib arrays) and ASCII; axis + unit conversion read from the file
 *   PSK / PSKX         Unreal Engine ActorX static/skeletal mesh (points, wedges, faces, materials)
 *
 * Conventions of the engine: right-handed, +Y up, metres, UV origin top-left.
 * Cooked .uasset/.umap packages are not parsed (see model_load for the message).
 */
#include "game_internal.h"
#include <ctype.h>
#if defined(_WIN32)
#  define strcasecmp _stricmp
#else
#  include <strings.h>
#endif

#define FAIL(...) do { snprintf(err, 256, __VA_ARGS__); return false; } while (0)

static const char *ext_of(const char *p) { const char *d = strrchr(p, '.'); return d ? d : ""; }
static uint32_t rd32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static float rdf(const uint8_t *p) { uint32_t u = rd32(p); float f; memcpy(&f, &u, 4); return f; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static double rdd(const uint8_t *p) { uint64_t u = rd64(p); double f; memcpy(&f, &u, 8); return f; }

/* ---- Unreal (Z-up, X-forward, left-handed, cm) -> engine (Y-up, -Z-forward, right-handed, m) ---- */
typedef struct { bool ue; float s; bool flip; } Cv;
static Cv cv_make(const LoadOpts *lo) {
    Cv c; c.ue = lo->ue; c.s = lo->scale > 0 ? lo->scale : 1.0f; if (lo->ue && lo->meters) c.s *= 0.01f;
    c.flip = lo->ue ^ lo->flip_winding;                                  /* the UE basis change is a reflection */
    return c;
}
static V3 cv_pos(const Cv *c, V3 p) { if (c->ue) p = v3(p.y, p.z, -p.x); return v3mul(p, c->s); }
static V3 cv_nrm(const Cv *c, V3 n) { if (c->ue) n = v3(n.y, n.z, -n.x); return n; }
static void tri_f(int a, int b, int c, bool flip) { if (flip) mb_tri(a, c, b); else mb_tri(a, b, c); }

static int tex_resolve(const char *dir, const char *rel) {
    if (!rel || !*rel) return -1;
    char norm[512]; snprintf(norm, sizeof norm, "%s", rel);
    for (char *q = norm; *q; q++) if (*q == '\\') *q = '/';
    char *p = path_join(dir, norm);
    if (!file_exists(p)) {                                              /* fall back to the bare file name next to the model */
        free(p); const char *b = strrchr(norm, '/'); p = path_join(dir, b ? b + 1 : norm);
        if (!file_exists(p)) { free(p); return -1; }
    }
    int id = tex_file(p); free(p); return id;
}

/* ================================================================== OBJ */
typedef struct { char name[64]; uint32_t col; int tex; bool trans; } OMat;
typedef struct { int v, t, n, out; } VK;
typedef struct { VK *tab; int cap, cnt; } VMap;
static int vmap_get(VMap *m, int v, int t, int n) {
    if (m->cnt * 2 >= m->cap) {
        int nc = m->cap ? m->cap * 2 : 1024; VK *nt = kc_malloc(sizeof(VK) * (size_t)nc); for (int i = 0; i < nc; i++) nt[i].out = -1;
        for (int i = 0; i < m->cap; i++) if (m->tab[i].out >= 0) {
            uint32_t h = hash3(m->tab[i].v, m->tab[i].t, m->tab[i].n, 3) & (uint32_t)(nc - 1);
            while (nt[h].out >= 0) h = (h + 1) & (uint32_t)(nc - 1); nt[h] = m->tab[i];
        }
        free(m->tab); m->tab = nt; m->cap = nc;
    }
    uint32_t h = hash3(v, t, n, 3) & (uint32_t)(m->cap - 1);
    while (m->tab[h].out >= 0) { if (m->tab[h].v == v && m->tab[h].t == t && m->tab[h].n == n) return m->tab[h].out; h = (h + 1) & (uint32_t)(m->cap - 1); }
    m->tab[h].v = v; m->tab[h].t = t; m->tab[h].n = n; m->tab[h].out = -2; m->cnt++;
    return -2 - (int)h;                                                 /* caller creates the vertex then calls vmap_set */
}

static void obj_mtl(const char *path, const char *dir, OMat **mats, int *nmat, int *cmat) {
    size_t n; char *t = read_file_all(path, &n); if (!t) return;
    OMat *cur = NULL; char *save = NULL;
    for (char *line = strtok_r(t, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        while (*line == ' ' || *line == '\t') line++;
        size_t l = strlen(line); while (l && (line[l - 1] == '\r' || line[l - 1] == ' ')) line[--l] = 0;
        if (!strncmp(line, "newmtl ", 7)) {
            if (*nmat == *cmat) { *cmat = *cmat ? *cmat * 2 : 8; *mats = kc_realloc(*mats, sizeof(OMat) * (size_t)*cmat); }
            cur = &(*mats)[(*nmat)++]; memset(cur, 0, sizeof *cur); snprintf(cur->name, sizeof cur->name, "%s", line + 7); cur->col = 0xFFFFFFFFu; cur->tex = -1;
        } else if (cur && !strncmp(line, "Kd ", 3)) { float r, g, b; if (sscanf(line + 3, "%f %f %f", &r, &g, &b) == 3) cur->col = col_pack((int)(r * 255), (int)(g * 255), (int)(b * 255), CA(cur->col)); }
        else if (cur && !strncmp(line, "d ", 2)) { float d = (float)atof(line + 2); if (d < 0.99f) { cur->trans = true; cur->col = (cur->col & 0x00FFFFFFu) | ((uint32_t)iclampi((int)(d * 255), 0, 255) << 24); } }
        else if (cur && !strncmp(line, "Tr ", 3)) { float d = 1 - (float)atof(line + 3); if (d < 0.99f) { cur->trans = true; cur->col = (cur->col & 0x00FFFFFFu) | ((uint32_t)iclampi((int)(d * 255), 0, 255) << 24); } }
        else if (cur && !strncmp(line, "map_Kd ", 7)) { char *p = line + 7; char *last = strrchr(p, ' '); if (last && p[0] == '-') p = last + 1; cur->tex = tex_resolve(dir, p); }
    }
    free(t);
}

static bool load_obj(const char *path, const uint8_t *d, size_t n, const LoadOpts *lo, char *err) {
    Cv cv = cv_make(lo); char *dir = path_dir(path);
    char *text = kc_malloc(n + 1); memcpy(text, d, n); text[n] = 0;
    V3 *P = NULL, *N = NULL; float *UV = NULL; int np = 0, nn = 0, nu = 0, cp = 0, cn = 0, cu = 0;
    OMat *mats = NULL; int nmat = 0, cmat = 0; VMap vm; memset(&vm, 0, sizeof vm);
    bool part_set = false, flip = cv.flip; char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        while (*line == ' ' || *line == '\t') line++;
        if (line[0] == 'v' && line[1] == ' ') {
            if (np == cp) { cp = cp ? cp * 2 : 1024; P = kc_realloc(P, sizeof(V3) * (size_t)cp); }
            float x = 0, y = 0, z = 0; sscanf(line + 2, "%f %f %f", &x, &y, &z); P[np++] = cv_pos(&cv, v3(x, y, z));
        } else if (line[0] == 'v' && line[1] == 'n') {
            if (nn == cn) { cn = cn ? cn * 2 : 1024; N = kc_realloc(N, sizeof(V3) * (size_t)cn); }
            float x = 0, y = 0, z = 0; sscanf(line + 3, "%f %f %f", &x, &y, &z); N[nn++] = v3norm(cv_nrm(&cv, v3(x, y, z)));
        } else if (line[0] == 'v' && line[1] == 't') {
            if (nu == cu) { cu = cu ? cu * 2 : 1024; UV = kc_realloc(UV, sizeof(float) * 2 * (size_t)cu); }
            float u = 0, v = 0; sscanf(line + 3, "%f %f", &u, &v); UV[nu * 2] = u; UV[nu * 2 + 1] = lo->flip_uv ? v : 1.0f - v; nu++;
        } else if (!strncmp(line, "mtllib ", 7)) {
            char *f = line + 7; size_t l = strlen(f); while (l && (f[l - 1] == '\r' || f[l - 1] == ' ')) f[--l] = 0;
            char *mp = path_join(dir, f); obj_mtl(mp, dir, &mats, &nmat, &cmat); free(mp);
        } else if (!strncmp(line, "usemtl ", 7)) {
            char *f = line + 7; size_t l = strlen(f); while (l && (f[l - 1] == '\r' || f[l - 1] == ' ')) f[--l] = 0;
            OMat *m = NULL; for (int i = 0; i < nmat; i++) if (!strcmp(mats[i].name, f)) m = &mats[i];
            if (m) mb_part(m->tex, m->col, false, m->trans, false); else mb_part(-1, 0xFFFFFFFFu, false, false, false);
            part_set = true;
        } else if (line[0] == 'f' && line[1] == ' ') {
            if (!part_set) { mb_part(-1, 0xFFFFFFFFu, false, false, false); part_set = true; }
            int vi[64], cnt = 0; char *p = line + 2;
            while (*p && cnt < 64) {
                while (*p == ' ' || *p == '\t') p++; if (!*p || *p == '\r') break;
                int a = 0, b = 0, c = 0; char *q = p; a = (int)strtol(q, &q, 10);
                if (*q == '/') { q++; if (*q != '/') b = (int)strtol(q, &q, 10); if (*q == '/') { q++; c = (int)strtol(q, &q, 10); } }
                if (a < 0) a = np + a + 1; if (b < 0) b = nu + b + 1; if (c < 0) c = nn + c + 1;
                if (a < 1 || a > np) { free(text); free(P); free(N); free(UV); free(mats); free(vm.tab); free(dir); FAIL("OBJ: face references vertex %d (only %d defined)", a, np); }
                int r = vmap_get(&vm, a, b, c);
                if (r <= -2) {
                    uint32_t h = (uint32_t)(-2 - r);
                    V3 nrm = (c >= 1 && c <= nn) ? N[c - 1] : v3(0, 0, 0);
                    int idx = mb_vertex(P[a - 1], nrm, (b >= 1 && b <= nu) ? UV[(b - 1) * 2] : 0, (b >= 1 && b <= nu) ? UV[(b - 1) * 2 + 1] : 0, 0xFFFFFFFFu, c >= 1 && c <= nn);
                    vm.tab[h].out = idx; r = idx;
                }
                vi[cnt++] = r; p = q;
                while (*p && *p != ' ' && *p != '\t') p++;
            }
            for (int i = 1; i + 1 < cnt; i++) tri_f(vi[0], vi[i], vi[i + 1], flip);
        }
    }
    free(text); free(P); free(N); free(UV); free(mats); free(vm.tab); free(dir);
    return true;
}

/* ================================================================== STL */
static bool load_stl(const uint8_t *d, size_t n, const LoadOpts *lo, char *err) {
    Cv cv = cv_make(lo);
    bool bin = false;
    if (n >= 84) { uint32_t cnt = rd32(d + 80); if ((size_t)84 + (size_t)cnt * 50 == n) bin = true; }
    mb_part(-1, 0xFFC8C8D0u, true, false, false);
    if (bin) {
        uint32_t cnt = rd32(d + 80);
        for (uint32_t i = 0; i < cnt; i++) {
            const uint8_t *r = d + 84 + (size_t)i * 50; V3 nn = cv_nrm(&cv, v3(rdf(r), rdf(r + 4), rdf(r + 8)));
            int idx[3]; for (int k = 0; k < 3; k++) idx[k] = mb_vertex(cv_pos(&cv, v3(rdf(r + 12 + k * 12), rdf(r + 16 + k * 12), rdf(r + 20 + k * 12))), nn, 0, 0, 0xFFFFFFFFu, nn.x || nn.y || nn.z);
            tri_f(idx[0], idx[1], idx[2], cv.flip);
        }
        return true;
    }
    char *t = kc_malloc(n + 1); memcpy(t, d, n); t[n] = 0; char *save = NULL; V3 nn = v3(0, 0, 0); int idx[3], k = 0;
    for (char *line = strtok_r(t, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        while (*line == ' ' || *line == '\t') line++;
        if (!strncmp(line, "facet normal", 12)) { float x, y, z; if (sscanf(line + 12, "%f %f %f", &x, &y, &z) == 3) nn = cv_nrm(&cv, v3(x, y, z)); k = 0; }
        else if (!strncmp(line, "vertex", 6)) { float x, y, z; if (sscanf(line + 6, "%f %f %f", &x, &y, &z) == 3 && k < 3) { idx[k++] = mb_vertex(cv_pos(&cv, v3(x, y, z)), nn, 0, 0, 0xFFFFFFFFu, nn.x || nn.y || nn.z); if (k == 3) tri_f(idx[0], idx[1], idx[2], cv.flip); } }
    }
    free(t);
    if (!mb_vcount()) FAIL("STL: no triangles found");
    return true;
}

/* ================================================================== PLY */
typedef struct { char name[32]; int type; bool list; int ctype; } PProp;
static int ply_type(const char *s) {
    if (!strcmp(s, "char") || !strcmp(s, "int8")) return 1; if (!strcmp(s, "uchar") || !strcmp(s, "uint8")) return 2;
    if (!strcmp(s, "short") || !strcmp(s, "int16")) return 3; if (!strcmp(s, "ushort") || !strcmp(s, "uint16")) return 4;
    if (!strcmp(s, "int") || !strcmp(s, "int32")) return 5; if (!strcmp(s, "uint") || !strcmp(s, "uint32")) return 6;
    if (!strcmp(s, "float") || !strcmp(s, "float32")) return 7; if (!strcmp(s, "double") || !strcmp(s, "float64")) return 8; return 0;
}
static int ply_size(int t) { static const int s[] = { 0, 1, 1, 2, 2, 4, 4, 4, 8 }; return s[t]; }
static double ply_read(const uint8_t *p, int t, bool be) {
    uint8_t b[8]; int sz = ply_size(t); for (int i = 0; i < sz; i++) b[i] = be ? p[sz - 1 - i] : p[i];
    switch (t) { case 1: return (int8_t)b[0]; case 2: return b[0]; case 3: return (int16_t)rd16(b); case 4: return rd16(b); case 5: return (int32_t)rd32(b); case 6: return rd32(b); case 7: return rdf(b); case 8: return rdd(b); }
    return 0;
}
static bool load_ply(const uint8_t *d, size_t n, const LoadOpts *lo, char *err) {
    Cv cv = cv_make(lo); size_t pos = 0; int fmt = -1; PProp vprops[32], fprops[8]; int nvp = 0, nfp = 0, nv = 0, nf = 0, cur = 0;
    for (;;) {
        size_t e = pos; while (e < n && d[e] != '\n') e++;
        if (e >= n) FAIL("PLY: bad header");
        char line[256]; size_t l = e - pos < 255 ? e - pos : 255; memcpy(line, d + pos, l); line[l] = 0; if (l && line[l - 1] == '\r') line[l - 1] = 0; pos = e + 1;
        if (!strncmp(line, "format ", 7)) fmt = !strncmp(line + 7, "ascii", 5) ? 0 : !strncmp(line + 7, "binary_little_endian", 20) ? 1 : !strncmp(line + 7, "binary_big_endian", 17) ? 2 : -1;
        else if (!strncmp(line, "element vertex", 14)) { nv = atoi(line + 14); cur = 1; }
        else if (!strncmp(line, "element face", 12)) { nf = atoi(line + 12); cur = 2; }
        else if (!strncmp(line, "element ", 8)) cur = 3;
        else if (!strncmp(line, "property ", 9)) {
            char a[32], b[32], c[32], dn[32]; PProp pp; memset(&pp, 0, sizeof pp);
            if (!strncmp(line + 9, "list ", 5)) { if (sscanf(line + 14, "%31s %31s %31s", a, b, c) == 3) { pp.list = true; pp.ctype = ply_type(a); pp.type = ply_type(b); snprintf(pp.name, 32, "%s", c); } }
            else if (sscanf(line + 9, "%31s %31s", a, dn) == 2) { pp.type = ply_type(a); snprintf(pp.name, 32, "%s", dn); }
            if (cur == 1 && nvp < 32) vprops[nvp++] = pp; else if (cur == 2 && nfp < 8) fprops[nfp++] = pp;
        } else if (!strcmp(line, "end_header")) break;
    }
    if (fmt < 0) FAIL("PLY: unsupported format"); if (nv <= 0) FAIL("PLY: no vertices");
    bool be = fmt == 2; mb_part(-1, 0xFFFFFFFFu, true, false, false);
    int ix = -1, iy = -1, iz = -1, inx = -1, iny = -1, inz = -1, iu = -1, iv = -1, ir = -1, ig = -1, ib = -1, ia = -1;
    for (int i = 0; i < nvp; i++) { const char *s = vprops[i].name;
        if (!strcmp(s, "x")) ix = i; else if (!strcmp(s, "y")) iy = i; else if (!strcmp(s, "z")) iz = i; else if (!strcmp(s, "nx")) inx = i; else if (!strcmp(s, "ny")) iny = i; else if (!strcmp(s, "nz")) inz = i;
        else if (!strcmp(s, "u") || !strcmp(s, "s") || !strcmp(s, "texture_u")) iu = i; else if (!strcmp(s, "v") || !strcmp(s, "t") || !strcmp(s, "texture_v")) iv = i;
        else if (!strcmp(s, "red")) ir = i; else if (!strcmp(s, "green")) ig = i; else if (!strcmp(s, "blue")) ib = i; else if (!strcmp(s, "alpha")) ia = i; }
    if (ix < 0 || iy < 0 || iz < 0) FAIL("PLY: vertex x/y/z properties missing");
    int base = mb_vcount(); const uint8_t *p = d + pos; char *ascii = NULL, *asave = NULL;
    if (fmt == 0) { ascii = kc_malloc(n - pos + 1); memcpy(ascii, d + pos, n - pos); ascii[n - pos] = 0; }
    char *atok = ascii ? strtok_r(ascii, " \t\r\n", &asave) : NULL;
    #define NEXTV(t, out) do { if (fmt == 0) { if (!atok) { free(ascii); FAIL("PLY: data truncated"); } (out) = atof(atok); atok = strtok_r(NULL, " \t\r\n", &asave); } \
        else { if (p + ply_size(t) > d + n) FAIL("PLY: data truncated"); (out) = ply_read(p, t, be); p += ply_size(t); } } while (0)
    for (int i = 0; i < nv; i++) {
        double val[32]; for (int k = 0; k < nvp; k++) { if (vprops[k].list) { double c; NEXTV(vprops[k].ctype, c); for (int q = 0; q < (int)c; q++) { double t; NEXTV(vprops[k].type, t); (void)t; } val[k] = 0; } else NEXTV(vprops[k].type, val[k]); }
        V3 nrm = inx >= 0 ? cv_nrm(&cv, v3((float)val[inx], (float)val[iny], (float)val[inz])) : v3(0, 0, 0);
        uint32_t col = 0xFFFFFFFFu;
        if (ir >= 0 && ig >= 0 && ib >= 0) { double sc = vprops[ir].type == 7 || vprops[ir].type == 8 ? 255.0 : 1.0; col = col_pack((int)(val[ir] * sc), (int)(val[ig] * sc), (int)(val[ib] * sc), ia >= 0 ? (int)(val[ia] * sc) : 255); }
        mb_vertex(cv_pos(&cv, v3((float)val[ix], (float)val[iy], (float)val[iz])), nrm, iu >= 0 ? (float)val[iu] : 0, iv >= 0 ? (lo->flip_uv ? (float)val[iv] : 1.0f - (float)val[iv]) : 0, col, inx >= 0);
    }
    for (int i = 0; i < nf; i++) {
        int idx[64], cnt = 0;
        for (int k = 0; k < nfp; k++) {
            if (fprops[k].list) { double c; NEXTV(fprops[k].ctype, c); bool vi = !strcmp(fprops[k].name, "vertex_indices") || !strcmp(fprops[k].name, "vertex_index");
                for (int q = 0; q < (int)c; q++) { double t; NEXTV(fprops[k].type, t); if (vi && cnt < 64) idx[cnt++] = base + (int)t; } }
            else { double t; NEXTV(fprops[k].type, t); (void)t; }
        }
        for (int q = 1; q + 1 < cnt; q++) tri_f(idx[0], idx[q], idx[q + 1], cv.flip);
    }
    free(ascii);
    return true;
}

/* ================================================================= JSON */
enum { JNULL, JBOOL, JNUM, JSTR, JARR, JOBJ };
typedef struct J { int t; double n; char *s; struct J **a; char **k; int c, cap; } J;
static J *jnew(int t) { J *j = kc_malloc(sizeof *j); memset(j, 0, sizeof *j); j->t = t; return j; }
static void jadd(J *p, const char *key, J *v) {
    if (p->c == p->cap) { p->cap = p->cap ? p->cap * 2 : 4; p->a = kc_realloc(p->a, sizeof(J *) * (size_t)p->cap); if (p->t == JOBJ) p->k = kc_realloc(p->k, sizeof(char *) * (size_t)p->cap); }
    p->a[p->c] = v; if (p->t == JOBJ) p->k[p->c] = kc_strdup(key); p->c++;
}
static void jfree(J *j) { if (!j) return; for (int i = 0; i < j->c; i++) { jfree(j->a[i]); if (j->k) free(j->k[i]); } free(j->a); free(j->k); free(j->s); free(j); }
static void jws(const char **p, const char *e) { while (*p < e && (**p == ' ' || **p == '\n' || **p == '\r' || **p == '\t')) (*p)++; }
static J *jval(const char **p, const char *e, int depth, char *err) {
    if (depth > 64) { snprintf(err, 256, "JSON nested too deeply"); return NULL; }
    jws(p, e); if (*p >= e) { snprintf(err, 256, "JSON: unexpected end"); return NULL; }
    char c = **p;
    if (c == '{' || c == '[') {
        J *o = jnew(c == '{' ? JOBJ : JARR); (*p)++; jws(p, e);
        if (*p < e && **p == (c == '{' ? '}' : ']')) { (*p)++; return o; }
        for (;;) {
            char *key = NULL; jws(p, e);
            if (c == '{') {
                if (*p >= e || **p != '"') { snprintf(err, 256, "JSON: expected key"); jfree(o); return NULL; }
                J *ks = jval(p, e, depth + 1, err); if (!ks) { jfree(o); return NULL; } key = ks->s; ks->s = NULL; jfree(ks);
                jws(p, e); if (*p >= e || **p != ':') { snprintf(err, 256, "JSON: expected ':'"); free(key); jfree(o); return NULL; } (*p)++;
            }
            J *v = jval(p, e, depth + 1, err); if (!v) { free(key); jfree(o); return NULL; }
            jadd(o, key ? key : "", v); free(key); jws(p, e);
            if (*p < e && **p == ',') { (*p)++; continue; }
            if (*p < e && **p == (c == '{' ? '}' : ']')) { (*p)++; return o; }
            snprintf(err, 256, "JSON: expected ',' or closing bracket"); jfree(o); return NULL;
        }
    }
    if (c == '"') {
        (*p)++; size_t cap = 32, l = 0; char *s = kc_malloc(cap);
        while (*p < e && **p != '"') {
            char ch = *(*p)++;
            if (ch == '\\' && *p < e) { char x = *(*p)++; ch = x == 'n' ? '\n' : x == 't' ? '\t' : x == 'r' ? '\r' : x == 'b' ? '\b' : x == 'f' ? '\f' : x;
                if (x == 'u' && *p + 4 <= e) { unsigned u = (unsigned)strtoul(*p > e ? "0" : (char[5]){ (*p)[0], (*p)[1], (*p)[2], (*p)[3], 0 }, NULL, 16); *p += 4; ch = u < 128 ? (char)u : '?'; } }
            if (l + 2 > cap) { cap *= 2; s = kc_realloc(s, cap); } s[l++] = ch;
        }
        s[l] = 0; if (*p < e) (*p)++;
        J *j = jnew(JSTR); j->s = s; return j;
    }
    if (c == 't' && e - *p >= 4 && !strncmp(*p, "true", 4)) { *p += 4; J *j = jnew(JBOOL); j->n = 1; return j; }
    if (c == 'f' && e - *p >= 5 && !strncmp(*p, "false", 5)) { *p += 5; return jnew(JBOOL); }
    if (c == 'n' && e - *p >= 4 && !strncmp(*p, "null", 4)) { *p += 4; return jnew(JNULL); }
    if (c == '-' || (c >= '0' && c <= '9')) { char *q; double v = strtod(*p, &q); if (q == *p) { snprintf(err, 256, "JSON: bad number"); return NULL; } *p = q; J *j = jnew(JNUM); j->n = v; return j; }
    snprintf(err, 256, "JSON: unexpected character '%c'", c); return NULL;
}
static J *jget(const J *o, const char *key) { if (!o || o->t != JOBJ) return NULL; for (int i = 0; i < o->c; i++) if (!strcmp(o->k[i], key)) return o->a[i]; return NULL; }
static J *jat(const J *a, int i) { return (a && a->t == JARR && i >= 0 && i < a->c) ? a->a[i] : NULL; }
static double jnum(const J *j, double def) { return (j && (j->t == JNUM || j->t == JBOOL)) ? j->n : def; }
static const char *jstr(const J *j) { return (j && j->t == JSTR) ? j->s : NULL; }
static int jlen(const J *j) { return (j && (j->t == JARR || j->t == JOBJ)) ? j->c : 0; }

static int b64v(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' || c == '-' ? 62 : c == '/' || c == '_' ? 63 : -1; }
static uint8_t *b64_decode(const char *s, size_t *outn) {
    size_t l = strlen(s); uint8_t *o = kc_malloc(l / 4 * 3 + 4); size_t n = 0; uint32_t acc = 0; int bits = 0;
    for (size_t i = 0; i < l; i++) { int v = b64v((unsigned char)s[i]); if (v < 0) continue; acc = acc << 6 | (uint32_t)v; bits += 6; if (bits >= 8) { bits -= 8; o[n++] = (uint8_t)(acc >> bits); } }
    *outn = n; return o;
}

/* ================================================================= glTF */
typedef struct { const uint8_t *base; size_t stride; int count, comps, ctype; bool norm, ok; } Acc;
typedef struct { uint8_t **buf; size_t *len; int n; } GBufs;
typedef struct { uint32_t col; int tex; bool two, trans, unlit; } GMat;
typedef struct { J *root; GBufs bufs; const char *dir; Cv cv; GMat *mats; int nmat; int *imgtex; int nimg; } GCtx;

static int g_csize(int t) { return t == 5120 || t == 5121 ? 1 : t == 5122 || t == 5123 ? 2 : 4; }
static bool acc_open(GCtx *g, int ai, Acc *a) {
    memset(a, 0, sizeof *a); J *ac = jat(jget(g->root, "accessors"), ai); if (!ac) return false;
    const char *ty = jstr(jget(ac, "type")); a->comps = !ty ? 1 : !strcmp(ty, "SCALAR") ? 1 : !strcmp(ty, "VEC2") ? 2 : !strcmp(ty, "VEC3") ? 3 : !strcmp(ty, "VEC4") ? 4 : !strcmp(ty, "MAT4") ? 16 : !strcmp(ty, "MAT3") ? 9 : 4;
    a->ctype = (int)jnum(jget(ac, "componentType"), 5126); a->count = (int)jnum(jget(ac, "count"), 0); a->norm = jnum(jget(ac, "normalized"), 0) != 0;
    J *bvi = jget(ac, "bufferView"); if (!bvi) return false;
    J *bv = jat(jget(g->root, "bufferViews"), (int)bvi->n); if (!bv) return false;
    int bi = (int)jnum(jget(bv, "buffer"), 0); if (bi < 0 || bi >= g->bufs.n || !g->bufs.buf[bi]) return false;
    size_t off = (size_t)jnum(jget(bv, "byteOffset"), 0) + (size_t)jnum(jget(ac, "byteOffset"), 0), esz = (size_t)g_csize(a->ctype) * (size_t)a->comps;
    a->stride = (size_t)jnum(jget(bv, "byteStride"), 0); if (!a->stride) a->stride = esz;
    if (a->count > 0 && off + a->stride * (size_t)(a->count - 1) + esz > g->bufs.len[bi]) return false;
    a->base = g->bufs.buf[bi] + off; a->ok = true; return true;
}
static float acc_f(const Acc *a, int i, int c) {
    const uint8_t *p = a->base + (size_t)i * a->stride + (size_t)c * (size_t)g_csize(a->ctype);
    switch (a->ctype) {
        case 5126: return rdf(p);
        case 5120: { int v = (int8_t)p[0]; return a->norm ? fmaxf(v / 127.0f, -1.0f) : (float)v; }
        case 5121: return a->norm ? p[0] / 255.0f : (float)p[0];
        case 5122: { int v = (int16_t)rd16(p); return a->norm ? fmaxf(v / 32767.0f, -1.0f) : (float)v; }
        case 5123: return a->norm ? rd16(p) / 65535.0f : (float)rd16(p);
        case 5125: return (float)rd32(p);
    }
    return 0;
}
static uint32_t acc_u(const Acc *a, int i) {
    const uint8_t *p = a->base + (size_t)i * a->stride;
    return a->ctype == 5121 ? p[0] : a->ctype == 5123 ? rd16(p) : rd32(p);
}
static M4 quat_m4(float x, float y, float z, float w) {
    M4 m = m4_identity(); float xx = x * x, yy = y * y, zz = z * z, xy = x * y, xz = x * z, yz = y * z, wx = w * x, wy = w * y, wz = w * z;
    m.m[0] = 1 - 2 * (yy + zz); m.m[1] = 2 * (xy + wz); m.m[2] = 2 * (xz - wy);
    m.m[4] = 2 * (xy - wz); m.m[5] = 1 - 2 * (xx + zz); m.m[6] = 2 * (yz + wx);
    m.m[8] = 2 * (xz + wy); m.m[9] = 2 * (yz - wx); m.m[10] = 1 - 2 * (xx + yy);
    return m;
}
static float m3det(const M4 *m) {
    return m->m[0] * (m->m[5] * m->m[10] - m->m[9] * m->m[6]) - m->m[4] * (m->m[1] * m->m[10] - m->m[9] * m->m[2]) + m->m[8] * (m->m[1] * m->m[6] - m->m[5] * m->m[2]);
}

static int gltf_image(GCtx *g, int ii) {
    if (ii < 0 || ii >= g->nimg) return -1;
    if (g->imgtex[ii] != -2) return g->imgtex[ii];
    J *im = jat(jget(g->root, "images"), ii); int tex = -1; char e2[256];
    const char *uri = jstr(jget(im, "uri"));
    if (uri && !strncmp(uri, "data:", 5)) {
        const char *comma = strchr(uri, ','); if (comma) { size_t n; uint8_t *b = b64_decode(comma + 1, &n); tex = tex_mem(b, n, "gltf-embedded", e2); free(b); if (tex >= 0) mb_own_tex(tex); else gm_warn("glTF image: %s", e2); }
    } else if (uri) {
        char dec[512]; size_t o = 0; for (const char *q = uri; *q && o < 510; q++) { if (*q == '%' && q[1] && q[2]) { char h[3] = { q[1], q[2], 0 }; dec[o++] = (char)strtol(h, NULL, 16); q += 2; } else dec[o++] = *q; } dec[o] = 0;
        tex = tex_resolve(g->dir, dec);
    } else if (jget(im, "bufferView")) {
        J *bv = jat(jget(g->root, "bufferViews"), (int)jnum(jget(im, "bufferView"), 0)); int bi = (int)jnum(jget(bv, "buffer"), 0);
        if (bv && bi >= 0 && bi < g->bufs.n && g->bufs.buf[bi]) { size_t off = (size_t)jnum(jget(bv, "byteOffset"), 0), len = (size_t)jnum(jget(bv, "byteLength"), 0);
            if (off + len <= g->bufs.len[bi]) { tex = tex_mem(g->bufs.buf[bi] + off, len, "glb-embedded", e2); if (tex >= 0) mb_own_tex(tex); else gm_warn("GLB image: %s", e2); } }
    }
    g->imgtex[ii] = tex; return tex;
}

static void gltf_emit_mesh(GCtx *g, J *mesh, const M4 *W) {
    bool flip = g->cv.flip ^ (m3det(W) < 0);
    J *prims = jget(mesh, "primitives");
    for (int pi = 0; pi < jlen(prims); pi++) {
        J *pr = prims->a[pi]; J *attr = jget(pr, "attributes"); int mode = (int)jnum(jget(pr, "mode"), 4);
        if (mode < 4 || mode > 6 || !attr) continue;
        if (jget(jget(pr, "extensions"), "KHR_draco_mesh_compression")) { gm_warn("glTF: Draco-compressed primitive skipped (decompress with gltf-transform first)"); continue; }
        Acc ap, an, au, ac; if (!acc_open(g, (int)jnum(jget(attr, "POSITION"), -1), &ap)) continue;
        bool hn = acc_open(g, (int)jnum(jget(attr, "NORMAL"), -1), &an), hu = acc_open(g, (int)jnum(jget(attr, "TEXCOORD_0"), -1), &au), hc = acc_open(g, (int)jnum(jget(attr, "COLOR_0"), -1), &ac);
        GMat gm = { 0xFFFFFFFFu, -1, false, false, false }; int mi = (int)jnum(jget(pr, "material"), -1); if (mi >= 0 && mi < g->nmat) gm = g->mats[mi];
        mb_part(gm.tex, gm.col, gm.two, gm.trans, gm.unlit);
        int base = mb_vcount();
        for (int i = 0; i < ap.count; i++) {
            V3 p = m4_point(W, v3(acc_f(&ap, i, 0), acc_f(&ap, i, 1), acc_f(&ap, i, 2))); p = v3mul(p, g->cv.s);
            V3 n = hn ? v3norm(m4_dir(W, v3(acc_f(&an, i, 0), acc_f(&an, i, 1), acc_f(&an, i, 2)))) : v3(0, 0, 0);
            uint32_t c = 0xFFFFFFFFu; if (hc) c = col_pack((int)(acc_f(&ac, i, 0) * 255), (int)(acc_f(&ac, i, 1) * 255), (int)(acc_f(&ac, i, 2) * 255), ac.comps > 3 ? (int)(acc_f(&ac, i, 3) * 255) : 255);
            mb_vertex(p, n, hu ? acc_f(&au, i, 0) : 0, hu ? acc_f(&au, i, 1) : 0, c, hn);
        }
        Acc ai; bool hi = acc_open(g, (int)jnum(jget(pr, "indices"), -1), &ai); int ni = hi ? ai.count : ap.count;
        #define GI(k) (base + (int)(hi ? acc_u(&ai, (k)) : (uint32_t)(k)))
        if (mode == 4) for (int k = 0; k + 2 < ni; k += 3) tri_f(GI(k), GI(k + 1), GI(k + 2), flip);
        else if (mode == 5) for (int k = 0; k + 2 < ni; k++) { if (k & 1) tri_f(GI(k + 1), GI(k), GI(k + 2), flip); else tri_f(GI(k), GI(k + 1), GI(k + 2), flip); }
        else for (int k = 1; k + 1 < ni; k++) tri_f(GI(0), GI(k), GI(k + 1), flip);
    }
}
static void gltf_walk(GCtx *g, int ni, const M4 *parent, int depth) {
    J *node = jat(jget(g->root, "nodes"), ni); if (!node || depth > 64) return;
    M4 local = m4_identity(); J *mat = jget(node, "matrix");
    if (jlen(mat) == 16) for (int i = 0; i < 16; i++) local.m[i] = (float)jnum(mat->a[i], 0);
    else {
        J *t = jget(node, "translation"), *r = jget(node, "rotation"), *s = jget(node, "scale");
        M4 R = jlen(r) == 4 ? quat_m4((float)jnum(r->a[0], 0), (float)jnum(r->a[1], 0), (float)jnum(r->a[2], 0), (float)jnum(r->a[3], 1)) : m4_identity();
        float sx = jlen(s) == 3 ? (float)jnum(s->a[0], 1) : 1, sy = jlen(s) == 3 ? (float)jnum(s->a[1], 1) : 1, sz = jlen(s) == 3 ? (float)jnum(s->a[2], 1) : 1;
        for (int c = 0; c < 3; c++) { float k = c == 0 ? sx : c == 1 ? sy : sz; for (int rr = 0; rr < 3; rr++) R.m[c * 4 + rr] *= k; }
        if (jlen(t) == 3) { R.m[12] = (float)jnum(t->a[0], 0); R.m[13] = (float)jnum(t->a[1], 0); R.m[14] = (float)jnum(t->a[2], 0); }
        local = R;
    }
    M4 world = m4_mul(parent, &local);
    int mi = (int)jnum(jget(node, "mesh"), -1); if (mi >= 0) { J *m = jat(jget(g->root, "meshes"), mi); if (m) gltf_emit_mesh(g, m, &world); }
    J *kids = jget(node, "children"); for (int i = 0; i < jlen(kids); i++) gltf_walk(g, (int)jnum(kids->a[i], -1), &world, depth + 1);
}

static bool load_gltf(const char *path, const uint8_t *d, size_t n, const LoadOpts *lo, char *err) {
    GCtx g; memset(&g, 0, sizeof g); g.cv = cv_make(lo); g.cv.ue = false; g.cv.s = lo->scale > 0 ? lo->scale : 1.0f; g.cv.flip = lo->flip_winding;
    char *dir = path_dir(path); g.dir = dir;
    const uint8_t *json = d; size_t jn = n; const uint8_t *bin = NULL; size_t binn = 0;
    if (n >= 12 && !memcmp(d, "glTF", 4)) {
        size_t p = 12; json = NULL; jn = 0;
        while (p + 8 <= n) { uint32_t cl = rd32(d + p), ct = rd32(d + p + 4); if (p + 8 + cl > n) break; if (ct == 0x4E4F534Au) { json = d + p + 8; jn = cl; } else if (ct == 0x004E4942u && !bin) { bin = d + p + 8; binn = cl; } p += 8 + ((cl + 3) & ~3u); }
        if (!json) { free(dir); FAIL("GLB: missing JSON chunk"); }
    }
    const char *pp = (const char *)json; char jerr[256] = "";
    g.root = jval(&pp, (const char *)json + jn, 0, jerr);
    if (!g.root) { free(dir); FAIL("glTF: %.200s", jerr); }
    J *ver = jget(jget(g.root, "asset"), "version"); if (!ver || !jstr(ver) || jstr(ver)[0] != '2') { jfree(g.root); free(dir); FAIL("glTF: only version 2.0 is supported"); }
    J *rq = jget(g.root, "extensionsRequired");
    for (int i = 0; i < jlen(rq); i++) { const char *e = jstr(rq->a[i]); if (e && (!strcmp(e, "KHR_draco_mesh_compression") || !strcmp(e, "EXT_meshopt_compression"))) { jfree(g.root); free(dir); FAIL("glTF: %s compression is not supported", e); } }
    J *bufs = jget(g.root, "buffers"); g.bufs.n = jlen(bufs);
    g.bufs.buf = kc_malloc(sizeof(uint8_t *) * (size_t)(g.bufs.n + 1)); g.bufs.len = kc_malloc(sizeof(size_t) * (size_t)(g.bufs.n + 1));
    bool *owned = kc_malloc((size_t)g.bufs.n + 1);
    for (int i = 0; i < g.bufs.n; i++) {
        const char *uri = jstr(jget(bufs->a[i], "uri")); g.bufs.buf[i] = NULL; g.bufs.len[i] = 0; owned[i] = false;
        if (!uri) { if (bin) { g.bufs.buf[i] = (uint8_t *)bin; g.bufs.len[i] = binn; } }
        else if (!strncmp(uri, "data:", 5)) { const char *c = strchr(uri, ','); if (c) { g.bufs.buf[i] = b64_decode(c + 1, &g.bufs.len[i]); owned[i] = true; } }
        else { char dec[512]; size_t o = 0; for (const char *q = uri; *q && o < 510; q++) { if (*q == '%' && q[1] && q[2]) { char h[3] = { q[1], q[2], 0 }; dec[o++] = (char)strtol(h, NULL, 16); q += 2; } else dec[o++] = *q; } dec[o] = 0;
               char *bp = path_join(dir, dec); g.bufs.buf[i] = (uint8_t *)read_file_all(bp, &g.bufs.len[i]); owned[i] = true; if (!g.bufs.buf[i]) gm_warn("glTF: cannot read buffer '%s'", bp); free(bp); }
    }
    g.nimg = jlen(jget(g.root, "images")); g.imgtex = kc_malloc(sizeof(int) * (size_t)(g.nimg + 1)); for (int i = 0; i < g.nimg; i++) g.imgtex[i] = -2;
    J *mats = jget(g.root, "materials"); g.nmat = jlen(mats); g.mats = kc_malloc(sizeof(GMat) * (size_t)(g.nmat + 1));
    for (int i = 0; i < g.nmat; i++) {
        J *m = mats->a[i], *pbr = jget(m, "pbrMetallicRoughness"); GMat *o = &g.mats[i]; o->col = 0xFFFFFFFFu; o->tex = -1; o->two = jnum(jget(m, "doubleSided"), 0) != 0;
        J *bc = jget(pbr, "baseColorFactor"); if (jlen(bc) == 4) o->col = col_pack((int)(jnum(bc->a[0], 1) * 255), (int)(jnum(bc->a[1], 1) * 255), (int)(jnum(bc->a[2], 1) * 255), (int)(jnum(bc->a[3], 1) * 255));
        const char *am = jstr(jget(m, "alphaMode")); o->trans = (am && !strcmp(am, "BLEND")) || CA(o->col) < 250;
        o->unlit = jget(jget(m, "extensions"), "KHR_materials_unlit") != NULL;
        J *bt = jget(pbr, "baseColorTexture"); if (bt) { J *t = jat(jget(g.root, "textures"), (int)jnum(jget(bt, "index"), -1)); if (t) o->tex = gltf_image(&g, (int)jnum(jget(t, "source"), -1)); }
    }
    M4 I = m4_identity();
    J *scenes = jget(g.root, "scenes"); J *sc = jat(scenes, (int)jnum(jget(g.root, "scene"), 0));
    if (sc && jlen(jget(sc, "nodes"))) { J *rn = jget(sc, "nodes"); for (int i = 0; i < rn->c; i++) gltf_walk(&g, (int)jnum(rn->a[i], -1), &I, 0); }
    else {                                                              /* no scene: every node that is nobody's child */
        int nn = jlen(jget(g.root, "nodes")); bool *isk = kc_malloc((size_t)nn + 1); memset(isk, 0, (size_t)nn + 1);
        for (int i = 0; i < nn; i++) { J *k = jget(jat(jget(g.root, "nodes"), i), "children"); for (int q = 0; q < jlen(k); q++) { int c = (int)jnum(k->a[q], -1); if (c >= 0 && c < nn) isk[c] = true; } }
        for (int i = 0; i < nn; i++) if (!isk[i]) gltf_walk(&g, i, &I, 0);
        free(isk);
        if (!mb_vcount()) { J *ms = jget(g.root, "meshes"); for (int i = 0; i < jlen(ms); i++) gltf_emit_mesh(&g, ms->a[i], &I); }
    }
    for (int i = 0; i < g.bufs.n; i++) if (owned[i]) free(g.bufs.buf[i]);
    free(owned); free(g.bufs.buf); free(g.bufs.len); free(g.mats); free(g.imgtex); jfree(g.root); free(dir);
    if (!mb_vcount()) FAIL("glTF: no triangle meshes found");
    return true;
}

/* ================================================================== FBX */
typedef struct { char t; int64_t i; double d; char *s; size_t sn; double *a; int an; } FP;
typedef struct FN { char *name; FP *p; int np, cp; struct FN **k; int nk, ck; } FN;
static FN *fn_new(const char *name, size_t nl) { FN *n = kc_malloc(sizeof *n); memset(n, 0, sizeof *n); n->name = kc_malloc(nl + 1); memcpy(n->name, name, nl); n->name[nl] = 0; return n; }
static FP *fn_addp(FN *n) { if (n->np == n->cp) { n->cp = n->cp ? n->cp * 2 : 6; n->p = kc_realloc(n->p, sizeof(FP) * (size_t)n->cp); } FP *p = &n->p[n->np++]; memset(p, 0, sizeof *p); return p; }
static void fn_addk(FN *n, FN *k) { if (n->nk == n->ck) { n->ck = n->ck ? n->ck * 2 : 4; n->k = kc_realloc(n->k, sizeof(FN *) * (size_t)n->ck); } n->k[n->nk++] = k; }
static void fn_free(FN *n) { if (!n) return; for (int i = 0; i < n->np; i++) { free(n->p[i].s); free(n->p[i].a); } for (int i = 0; i < n->nk; i++) fn_free(n->k[i]); free(n->p); free(n->k); free(n->name); free(n); }
static FN *fn_find(const FN *n, const char *name) { if (!n) return NULL; for (int i = 0; i < n->nk; i++) if (!strcmp(n->k[i]->name, name)) return n->k[i]; return NULL; }
static double fp_num(const FP *p) { return p->t == 'a' || p->t == 'S' || p->t == 'R' ? 0 : (p->t == 'D' || p->t == 'F') ? p->d : (double)p->i; }
static const char *fp_str(const FP *p) { return (p->t == 'S' && p->s) ? p->s : ""; }
static const double *fn_arr(const FN *n, int *cnt) {            /* first array property, or the numeric properties of a short node */
    if (!n) { *cnt = 0; return NULL; }
    for (int i = 0; i < n->np; i++) if (n->p[i].t == 'a') { *cnt = n->p[i].an; return n->p[i].a; }
    *cnt = 0; return NULL;
}

/* binary parser */
static bool fbx_bin_node(const uint8_t *d, size_t n, size_t *pos, int ver, FN *parent, int depth, char *err) {
    size_t hs = ver >= 7500 ? 25 : 13, p = *pos;
    if (depth > 64 || p + hs > n) FAIL("FBX: truncated or too deeply nested");
    uint64_t end, np, pl; uint8_t nl;
    if (ver >= 7500) { end = rd64(d + p); np = rd64(d + p + 8); pl = rd64(d + p + 16); nl = d[p + 24]; } else { end = rd32(d + p); np = rd32(d + p + 4); pl = rd32(d + p + 8); nl = d[p + 12]; }
    if (end == 0) { *pos = p + hs; return true; }                      /* null record: end of siblings */
    if (end > n || p + hs + nl > n) FAIL("FBX: bad node record");
    FN *node = fn_new((const char *)d + p + hs, nl); fn_addk(parent, node);
    size_t q = p + hs + nl, pend = q + (size_t)pl; if (pend > end) FAIL("FBX: bad property list");
    for (uint64_t i = 0; i < np; i++) {
        if (q >= pend) FAIL("FBX: property overrun"); FP *pr = fn_addp(node); char t = (char)d[q++]; pr->t = t;
        switch (t) {
            case 'Y': pr->t = 'I'; pr->i = (int16_t)rd16(d + q); q += 2; break;
            case 'C': pr->t = 'I'; pr->i = d[q]; q += 1; break;
            case 'I': pr->i = (int32_t)rd32(d + q); q += 4; break;
            case 'F': pr->t = 'D'; pr->d = rdf(d + q); q += 4; break;
            case 'D': pr->d = rdd(d + q); q += 8; break;
            case 'L': pr->i = (int64_t)rd64(d + q); q += 8; break;
            case 'S': case 'R': { uint32_t l = rd32(d + q); q += 4; if (q + l > pend) FAIL("FBX: string overrun"); pr->s = kc_malloc((size_t)l + 1); memcpy(pr->s, d + q, l); pr->s[l] = 0; pr->sn = l; q += l; break; }
            case 'f': case 'd': case 'l': case 'i': case 'b': {
                uint32_t cnt = rd32(d + q), enc = rd32(d + q + 4), cl = rd32(d + q + 8); q += 12; if (q + cl > pend) FAIL("FBX: array overrun");
                int es = (t == 'd' || t == 'l') ? 8 : (t == 'b' ? 1 : 4); const uint8_t *src = d + q; uint8_t *tmp = NULL; size_t sl = cl;
                if (enc == 1) { size_t ol; char e2[256]; tmp = gm_inflate(d + q, cl, 1, &ol, e2); if (!tmp) FAIL("FBX: %.200s", e2); src = tmp; sl = ol; }
                if (sl < (size_t)cnt * (size_t)es) { free(tmp); FAIL("FBX: array data too short"); }
                pr->t = 'a'; pr->an = (int)cnt; pr->a = kc_malloc(sizeof(double) * (size_t)(cnt ? cnt : 1));
                for (uint32_t k = 0; k < cnt; k++) pr->a[k] = t == 'f' ? rdf(src + (size_t)k * 4) : t == 'd' ? rdd(src + (size_t)k * 8) : t == 'l' ? (double)(int64_t)rd64(src + (size_t)k * 8) : t == 'i' ? (double)(int32_t)rd32(src + (size_t)k * 4) : (double)src[k];
                free(tmp); q += cl; break; }
            default: FAIL("FBX: unknown property type '%c'", t);
        }
    }
    q = pend;
    while (q < end) { size_t cq = q; if (!fbx_bin_node(d, n, &cq, ver, node, depth + 1, err)) return false; if (cq == q + hs && fn_find(node, "") == NULL && cq <= end && d[q] == 0 && (ver >= 7500 ? rd64(d + q) == 0 : rd32(d + q) == 0)) { q = cq; break; } q = cq; }
    *pos = end; return true;
}

/* ascii parser */
static void fa_ws(const char **p, const char *e) { for (;;) { while (*p < e && isspace((unsigned char)**p)) (*p)++; if (*p < e && **p == ';') { while (*p < e && **p != '\n') (*p)++; continue; } break; } }
static bool fbx_ascii_nodes(const char **p, const char *e, FN *parent, int depth, char *err) {
    if (depth > 64) FAIL("FBX: nested too deeply");
    for (;;) {
        fa_ws(p, e); if (*p >= e) return true; if (**p == '}') return true;
        const char *s = *p; while (*p < e && **p != ':' && **p != '\n') (*p)++;
        if (*p >= e || **p != ':') FAIL("FBX ascii: expected ':' after node name");
        FN *node = fn_new(s, (size_t)(*p - s)); fn_addk(parent, node); (*p)++;
        for (;;) {
            fa_ws(p, e); if (*p >= e) return true; char c = **p;
            if (c == '{' || c == '}' || isalpha((unsigned char)c) || c == '_') {
                if (c == '{' || c == '}') break;
                /* bare identifier: a property token (T, W, ...) or the next node's name */
                const char *q = *p; while (q < e && (isalnum((unsigned char)*q) || *q == '_')) q++;
                const char *r = q; while (r < e && (*r == ' ' || *r == '\t')) r++;
                if (r < e && *r == ':') break;                                  /* next node */
                FP *pr = fn_addp(node); pr->t = 'S'; pr->sn = (size_t)(q - *p); pr->s = kc_malloc(pr->sn + 1); memcpy(pr->s, *p, pr->sn); pr->s[pr->sn] = 0; *p = q;
            } else if (c == '*') {
                (*p)++; long cnt = strtol(*p, (char **)p, 10); fa_ws(p, e);
                if (*p < e && **p == '{') {
                    (*p)++; fa_ws(p, e); if (e - *p >= 2 && (*p)[0] == 'a' && (*p)[1] == ':') *p += 2;
                    FP *pr = fn_addp(node); pr->t = 'a'; pr->a = kc_malloc(sizeof(double) * (size_t)(cnt > 0 ? cnt : 1));
                    int k = 0; for (;;) { fa_ws(p, e); if (*p >= e || **p == '}') break; if (**p == ',') { (*p)++; continue; } char *q; double v = strtod(*p, &q); if (q == *p) FAIL("FBX ascii: bad array value"); *p = q; if (k < cnt) pr->a[k++] = v; }
                    pr->an = k; if (*p < e) (*p)++;
                }
            } else if (c == '"') {
                (*p)++; const char *s2 = *p; while (*p < e && **p != '"') (*p)++;
                FP *pr = fn_addp(node); pr->t = 'S'; pr->sn = (size_t)(*p - s2); pr->s = kc_malloc(pr->sn + 1); memcpy(pr->s, s2, pr->sn); pr->s[pr->sn] = 0; if (*p < e) (*p)++;
            } else if (c == '-' || c == '+' || c == '.' || isdigit((unsigned char)c)) {
                char *q; const char *st = *p; double v = strtod(*p, &q); if (q == *p) FAIL("FBX ascii: bad number"); *p = q;
                bool isf = false; for (const char *x = st; x < q; x++) if (*x == '.' || *x == 'e' || *x == 'E') isf = true;
                FP *pr = fn_addp(node); if (isf) { pr->t = 'D'; pr->d = v; } else { pr->t = 'L'; pr->i = (int64_t)v; }
            } else FAIL("FBX ascii: unexpected '%c'", c);
            fa_ws(p, e); if (*p < e && **p == ',') (*p)++;
        }
        fa_ws(p, e);
        if (*p < e && **p == '{') { (*p)++; if (!fbx_ascii_nodes(p, e, node, depth + 1, err)) return false; fa_ws(p, e); if (*p < e && **p == '}') (*p)++; }
        if (depth == 0 && *p >= e) return true;
    }
}

static M4 fbx_rot(float rx, float ry, float rz) {                          /* XYZ order: Rz * Ry * Rx */
    V3 z = v3(0, 0, rz), y = v3(0, ry, 0), x = v3(rx, 0, 0); (void)z;
    float cx = cosf(DEG2RAD(rx)), sx = sinf(DEG2RAD(rx)), cy = cosf(DEG2RAD(ry)), sy = sinf(DEG2RAD(ry)), cz = cosf(DEG2RAD(rz)), sz = sinf(DEG2RAD(rz)); (void)y; (void)x;
    float Rx[9] = { 1, 0, 0, 0, cx, sx, 0, -sx, cx }, Ry[9] = { cy, 0, -sy, 0, 1, 0, sy, 0, cy }, Rz[9] = { cz, sz, 0, -sz, cz, 0, 0, 0, 1 }, A[9], B[9];
    for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) { float q = 0; for (int k = 0; k < 3; k++) q += Ry[k * 3 + r] * Rx[c * 3 + k]; A[c * 3 + r] = q; }
    for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) { float q = 0; for (int k = 0; k < 3; k++) q += Rz[k * 3 + r] * A[c * 3 + k]; B[c * 3 + r] = q; }
    M4 m = m4_identity(); for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) m.m[c * 4 + r] = B[c * 3 + r]; return m;
}
static bool fbx_p70(const FN *obj, const char *name, double *out, int want) {
    FN *pr = fn_find(obj, "Properties70"); if (!pr) return false;
    for (int i = 0; i < pr->nk; i++) { FN *p = pr->k[i]; if (strcmp(p->name, "P") || p->np < 5 || strcmp(fp_str(&p->p[0]), name)) continue; for (int k = 0; k < want && 4 + k < p->np; k++) out[k] = fp_num(&p->p[4 + k]); return true; }
    return false;
}

typedef struct { int64_t id; FN *n; } FObj;
typedef struct { int64_t child, parent; char prop[48]; } FConn;
static int fo_find(const FObj *o, int n, int64_t id) { for (int i = 0; i < n; i++) if (o[i].id == id) return i; return -1; }

static bool load_fbx(const char *path, const uint8_t *d, size_t n, const LoadOpts *lo, char *err) {
    FN *root = fn_new("root", 4); char *dir = path_dir(path); bool ok = true;
    if (n > 27 && !memcmp(d, "Kaydara FBX Binary", 18)) {
        int ver = (int)rd32(d + 23); size_t pos = 27;
        while (pos + (ver >= 7500 ? 25u : 13u) <= n) { size_t before = pos; if (!fbx_bin_node(d, n, &pos, ver, root, 0, err)) { ok = false; break; } if (pos == before) break; if (root->nk && !strcmp(root->k[root->nk - 1]->name, "") ) break; }
        if (ok) for (int i = root->nk - 1; i >= 0; i--) if (!root->k[i]->name[0]) { fn_free(root->k[i]); root->nk--; }
    } else {
        char *t = kc_malloc(n + 1); memcpy(t, d, n); t[n] = 0; const char *p = t;
        ok = fbx_ascii_nodes(&p, t + n, root, 0, err); free(t);
    }
    if (!ok) { fn_free(root); free(dir); return false; }
    /* global settings */
    double up = 1, ups = 1, fr = 2, frs = 1, co = 0, cos_ = 1, usf = 1;
    FN *gs = fn_find(root, "GlobalSettings");
    if (gs) { fbx_p70(gs, "UpAxis", &up, 1); fbx_p70(gs, "UpAxisSign", &ups, 1); fbx_p70(gs, "FrontAxis", &fr, 1); fbx_p70(gs, "FrontAxisSign", &frs, 1); fbx_p70(gs, "CoordAxis", &co, 1); fbx_p70(gs, "CoordAxisSign", &cos_, 1); fbx_p70(gs, "UnitScaleFactor", &usf, 1); }
    int ui = iclampi((int)up, 0, 2), fi = iclampi((int)fr, 0, 2), ci = iclampi((int)co, 0, 2);
    if (ui == fi || ui == ci || fi == ci) { ui = 1; fi = 2; ci = 0; ups = frs = cos_ = 1; }
    float A[9] = { 0 };                                                    /* out = A * in (row-major) */
    A[0 * 3 + ci] = cos_ < 0 ? -1.0f : 1.0f; A[1 * 3 + ui] = ups < 0 ? -1.0f : 1.0f; A[2 * 3 + fi] = frs < 0 ? -1.0f : 1.0f;
    float det = A[0] * (A[4] * A[8] - A[5] * A[7]) - A[1] * (A[3] * A[8] - A[5] * A[6]) + A[2] * (A[3] * A[7] - A[4] * A[6]);
    float unit = (float)(lo->meters ? usf * 0.01 : 1.0) * (lo->scale > 0 ? lo->scale : 1.0f); if (unit <= 0) unit = 1;
    bool flip = (det < 0) ^ lo->flip_winding;
    /* objects + connections */
    FN *objs = fn_find(root, "Objects"), *cons = fn_find(root, "Connections");
    if (!objs) { fn_free(root); free(dir); FAIL("FBX: no Objects section"); }
    FObj *O = kc_malloc(sizeof(FObj) * (size_t)(objs->nk + 1)); int no = 0;
    for (int i = 0; i < objs->nk; i++) if (objs->k[i]->np >= 1) { O[no].id = objs->k[i]->p[0].t == 'a' ? 0 : (int64_t)fp_num(&objs->k[i]->p[0]); O[no].n = objs->k[i]; no++; }
    int nc = cons ? cons->nk : 0; FConn *C = kc_malloc(sizeof(FConn) * (size_t)(nc + 1)); int ncc = 0;
    for (int i = 0; i < nc; i++) { FN *c = cons->k[i]; if (strcmp(c->name, "C") || c->np < 3) continue; C[ncc].child = (int64_t)fp_num(&c->p[1]); C[ncc].parent = (int64_t)fp_num(&c->p[2]); snprintf(C[ncc].prop, sizeof C[ncc].prop, "%s", c->np > 3 ? fp_str(&c->p[3]) : ""); ncc++; }
    #define OBJ_IS(oi, nm) (!strcmp(O[oi].n->name, nm))
    int emitted = 0;
    for (int mi = 0; mi < no; mi++) {
        if (!OBJ_IS(mi, "Model")) continue;
        /* world matrix: chain of parent models */
        M4 W = m4_identity(); int cur = mi, guard = 0; M4 chain[32]; int nch = 0;
        while (cur >= 0 && guard++ < 32) {
            double tr[3] = { 0, 0, 0 }, rt[3] = { 0, 0, 0 }, sc[3] = { 1, 1, 1 }, pre[3] = { 0, 0, 0 };
            fbx_p70(O[cur].n, "Lcl Translation", tr, 3); fbx_p70(O[cur].n, "Lcl Rotation", rt, 3); fbx_p70(O[cur].n, "Lcl Scaling", sc, 3); fbx_p70(O[cur].n, "PreRotation", pre, 3);
            M4 R = fbx_rot((float)rt[0], (float)rt[1], (float)rt[2]), Rp = fbx_rot((float)pre[0], (float)pre[1], (float)pre[2]); R = m4_mul(&Rp, &R);
            for (int c = 0; c < 3; c++) { for (int r = 0; r < 3; r++) R.m[c * 4 + r] *= (float)sc[c]; }
            R.m[12] = (float)tr[0]; R.m[13] = (float)tr[1]; R.m[14] = (float)tr[2]; chain[nch++] = R;
            int next = -1; for (int k = 0; k < ncc; k++) if (C[k].child == O[cur].id) { int pi = fo_find(O, no, C[k].parent); if (pi >= 0 && OBJ_IS(pi, "Model")) { next = pi; break; } }
            cur = next;
        }
        for (int k = nch - 1; k >= 0; k--) W = m4_mul(&W, &chain[k]);
        /* attached geometry + materials */
        int mats[32], nmats = 0;
        for (int k = 0; k < ncc && nmats < 32; k++) if (C[k].parent == O[mi].id) { int ci2 = fo_find(O, no, C[k].child); if (ci2 >= 0 && OBJ_IS(ci2, "Material")) mats[nmats++] = ci2; }
        for (int k = 0; k < ncc; k++) {
            if (C[k].parent != O[mi].id) continue; int gi = fo_find(O, no, C[k].child); if (gi < 0 || !OBJ_IS(gi, "Geometry")) continue;
            FN *geo = O[gi].n; int nv = 0, ni = 0; const double *V = fn_arr(fn_find(geo, "Vertices"), &nv); const double *PI = fn_arr(fn_find(geo, "PolygonVertexIndex"), &ni);
            if (!V || !PI || nv < 3) continue;
            /* layer elements */
            struct { const double *data, *idx; int nd, ni; int map; bool direct; } NL = { 0 }, UL = { 0 }; const double *MT = NULL; int nmt = 0; int mtmap = 0;
            for (int q = 0; q < geo->nk; q++) {
                FN *le = geo->k[q]; bool isn = !strcmp(le->name, "LayerElementNormal"), isu = !strcmp(le->name, "LayerElementUV"), ism = !strcmp(le->name, "LayerElementMaterial");
                if (!isn && !isu && !ism) continue;
                FN *mt = fn_find(le, "MappingInformationType"), *rf = fn_find(le, "ReferenceInformationType"); const char *ms = mt && mt->np ? fp_str(&mt->p[0]) : "", *rs = rf && rf->np ? fp_str(&rf->p[0]) : "";
                int map = !strncmp(ms, "ByPolygonVertex", 15) ? 0 : (!strncmp(ms, "ByVertice", 9) || !strncmp(ms, "ByVertex", 8) || !strncmp(ms, "ByControlPoint", 14)) ? 1 : !strcmp(ms, "ByPolygon") ? 2 : 3;
                if (isn) { NL.data = fn_arr(fn_find(le, "Normals"), &NL.nd); NL.idx = fn_arr(fn_find(le, "NormalsIndex"), &NL.ni); NL.map = map; NL.direct = strcmp(rs, "IndexToDirect") != 0 || !NL.idx; }
                else if (isu) { UL.data = fn_arr(fn_find(le, "UV"), &UL.nd); UL.idx = fn_arr(fn_find(le, "UVIndex"), &UL.ni); UL.map = map; UL.direct = strcmp(rs, "IndexToDirect") != 0 || !UL.idx; }
                else { MT = fn_arr(fn_find(le, "Materials"), &nmt); mtmap = map; }
            }
            /* polygons */
            int maxslot = 0; int npoly = 0; for (int i = 0; i < ni; i++) if (PI[i] < 0) npoly++;
            int *pstart = kc_malloc(sizeof(int) * (size_t)(npoly + 2)), *pslot = kc_malloc(sizeof(int) * (size_t)(npoly + 1)); int pc = 0, run = 0;
            for (int i = 0; i < ni; i++) { if (run == 0) pstart[pc] = i; run++; if (PI[i] < 0) { pslot[pc] = (MT && mtmap == 2 && pc < nmt) ? (int)MT[pc] : (MT && nmt ? (int)MT[0] : 0); if (pslot[pc] < 0) pslot[pc] = 0; if (pslot[pc] > maxslot) maxslot = pslot[pc]; pc++; run = 0; } }
            for (int slot = 0; slot <= maxslot; slot++) {
                bool any = false; for (int pi2 = 0; pi2 < pc; pi2++) if (pslot[pi2] == slot) { any = true; break; } if (!any) continue;
                uint32_t col = 0xFFFFFFFFu; int tex = -1;
                if (slot < nmats) {
                    FN *mat = O[mats[slot]].n; double dc[3] = { 0.8, 0.8, 0.8 }; if (fbx_p70(mat, "DiffuseColor", dc, 3) || fbx_p70(mat, "Diffuse", dc, 3)) col = col_pack((int)(dc[0] * 255), (int)(dc[1] * 255), (int)(dc[2] * 255), 255);
                    for (int k2 = 0; k2 < ncc && tex < 0; k2++) if (C[k2].parent == O[mats[slot]].id) { int ti = fo_find(O, no, C[k2].child); if (ti >= 0 && OBJ_IS(ti, "Texture") && (!C[k2].prop[0] || strstr(C[k2].prop, "Diffuse") || strstr(C[k2].prop, "Base"))) {
                        FN *rel = fn_find(O[ti].n, "RelativeFilename"), *fnm = fn_find(O[ti].n, "FileName"); const char *f = rel && rel->np ? fp_str(&rel->p[0]) : (fnm && fnm->np ? fp_str(&fnm->p[0]) : "");
                        tex = tex_resolve(dir, f); if (tex >= 0) col = 0xFFFFFFFFu; } }
                }
                mb_part(tex, col, false, false, false);
                int pvc = 0;
                for (int pi2 = 0; pi2 < pc; pi2++) {
                    int s = pstart[pi2], e = (pi2 + 1 < pc ? pstart[pi2 + 1] : ni); int cnt = e - s;
                    if (pslot[pi2] != slot) { pvc += cnt; continue; }
                    int vidx[64]; if (cnt > 64) cnt = 64;
                    for (int j = 0; j < cnt; j++) {
                        int cp = (int)PI[s + j]; if (cp < 0) cp = -cp - 1; if ((cp + 1) * 3 > nv) cp = 0;
                        V3 p = m4_point(&W, v3((float)V[cp * 3], (float)V[cp * 3 + 1], (float)V[cp * 3 + 2]));
                        p = v3(A[0] * p.x + A[1] * p.y + A[2] * p.z, A[3] * p.x + A[4] * p.y + A[5] * p.z, A[6] * p.x + A[7] * p.y + A[8] * p.z); p = v3mul(p, unit);
                        V3 nr = v3(0, 0, 0); bool hn = false;
                        if (NL.data) { int ix = NL.map == 0 ? pvc + j : NL.map == 1 ? cp : NL.map == 2 ? pi2 : 0; if (!NL.direct && ix < NL.ni) ix = (int)NL.idx[ix]; if (ix >= 0 && (ix + 1) * 3 <= NL.nd) {
                            nr = m4_dir(&W, v3((float)NL.data[ix * 3], (float)NL.data[ix * 3 + 1], (float)NL.data[ix * 3 + 2])); nr = v3norm(v3(A[0] * nr.x + A[1] * nr.y + A[2] * nr.z, A[3] * nr.x + A[4] * nr.y + A[5] * nr.z, A[6] * nr.x + A[7] * nr.y + A[8] * nr.z)); hn = true; } }
                        float u = 0, v = 0;
                        if (UL.data) { int ix = UL.map == 0 ? pvc + j : UL.map == 1 ? cp : 0; if (!UL.direct && ix < UL.ni) ix = (int)UL.idx[ix]; if (ix >= 0 && (ix + 1) * 2 <= UL.nd) { u = (float)UL.data[ix * 2]; v = (float)UL.data[ix * 2 + 1]; if (!lo->flip_uv) v = 1.0f - v; } }
                        vidx[j] = mb_vertex(p, nr, u, v, 0xFFFFFFFFu, hn);
                    }
                    for (int j = 1; j + 1 < cnt; j++) tri_f(vidx[0], vidx[j], vidx[j + 1], flip);
                    pvc += e - s; emitted++;
                }
            }
            free(pstart); free(pslot);
        }
    }
    free(O); free(C); fn_free(root); free(dir);
    if (!emitted) FAIL("FBX: no mesh geometry found");
    return true;
}

/* ================================================================== PSK */
typedef struct { int64_t key; int out; } PKey;
static bool load_psk(const char *path, const uint8_t *d, size_t n, const LoadOpts *lo, char *err) {
    Cv cv = cv_make(lo); cv.ue = true; if (lo->meters) cv.s = (lo->scale > 0 ? lo->scale : 1.0f) * 0.01f; else cv.s = lo->scale > 0 ? lo->scale : 1.0f;
    cv.flip = lo->flip_winding; char *dir = path_dir(path);
    const uint8_t *pts = NULL, *wed = NULL, *fac = NULL, *mat = NULL; int np = 0, nw = 0, nf = 0, nm = 0, wsz = 0, fsz = 0, msz = 0; size_t p = 0;
    while (p + 32 <= n) {
        char id[21]; memcpy(id, d + p, 20); id[20] = 0; int32_t sz = (int32_t)rd32(d + p + 24), cnt = (int32_t)rd32(d + p + 28); p += 32;
        if (sz < 0 || cnt < 0 || (size_t)sz * (size_t)cnt > n - p) break;
        if (!strncmp(id, "PNTS0000", 8)) { pts = d + p; np = cnt; }
        else if (!strncmp(id, "VTXW0000", 8)) { wed = d + p; nw = cnt; wsz = sz; }
        else if (!strncmp(id, "FACE0000", 8) || !strncmp(id, "FACE3200", 8)) { fac = d + p; nf = cnt; fsz = sz; }
        else if (!strncmp(id, "MATT0000", 8)) { mat = d + p; nm = cnt; msz = sz; }
        p += (size_t)sz * (size_t)cnt;
    }
    if (!pts || !wed || !fac || np <= 0 || nw <= 0 || nf <= 0 || wsz < 16 || fsz < 12) { free(dir); FAIL("PSK: missing PNTS0000/VTXW0000/FACE0000 chunks (not a valid .psk/.pskx file)"); }
    bool idx32 = fsz >= 18;
    /* faces grouped by material; vertices unique per (wedge, smoothing group) */
    int maxmat = 0; for (int i = 0; i < nf; i++) { int m = fac[(size_t)i * fsz + (idx32 ? 12 : 6)]; if (m > maxmat) maxmat = m; }
    int *mapw = kc_malloc(sizeof(int) * (size_t)nw); V3 *acc = kc_malloc(sizeof(V3) * (size_t)nf * 3);
    for (int slot = 0; slot <= maxmat; slot++) {
        bool any = false; for (int i = 0; i < nf; i++) if (fac[(size_t)i * fsz + (idx32 ? 12 : 6)] == slot) { any = true; break; } if (!any) continue;
        uint32_t col = col_hsv((float)(hash32((uint32_t)slot + 9) & 1023) / 1024.0f, 0.25f, 0.85f, 255); int tex = -1;
        if (slot < nm && msz >= 64) {
            char name[65]; memcpy(name, mat + (size_t)slot * msz, 64); name[64] = 0;
            static const char *exts[] = { ".png", ".tga", ".bmp", ".PNG", ".TGA" }; static const char *pre[] = { "", "T_", "t_" };
            for (int a = 0; a < 3 && tex < 0; a++) for (int b = 0; b < 5 && tex < 0; b++) { char fnm[200]; snprintf(fnm, sizeof fnm, "%s%s%s", pre[a], name, exts[b]); tex = tex_resolve(dir, fnm); }
            if (tex >= 0) col = 0xFFFFFFFFu;
        }
        mb_part(tex, col, true, false, false);
        for (int i = 0; i < nw; i++) mapw[i] = -1;
        int tri0 = mb_vcount(); (void)tri0; (void)acc;
        for (int i = 0; i < nf; i++) {
            const uint8_t *f = fac + (size_t)i * fsz; if (f[idx32 ? 12 : 6] != slot) continue;
            int w[3]; for (int k = 0; k < 3; k++) w[k] = idx32 ? (int)rd32(f + k * 4) : (int)rd16(f + k * 2);
            int vi[3]; V3 pp[3];
            for (int k = 0; k < 3; k++) {
                if (w[k] < 0 || w[k] >= nw) { free(mapw); free(acc); free(dir); FAIL("PSK: face references wedge %d (of %d)", w[k], nw); }
                const uint8_t *wd = wed + (size_t)w[k] * wsz; uint32_t pi = rd32(wd); if ((int)pi >= np) pi &= 0xFFFF; if ((int)pi >= np) { free(mapw); free(acc); free(dir); FAIL("PSK: wedge references point %u (of %d)", pi, np); }
                const uint8_t *pt = pts + (size_t)pi * 12; pp[k] = cv_pos(&cv, v3(rdf(pt), rdf(pt + 4), rdf(pt + 8)));
                vi[k] = mb_vertex(pp[k], v3(0, 0, 0), rdf(wd + 4), rdf(wd + 8), 0xFFFFFFFFu, false);
            }
            tri_f(vi[0], vi[1], vi[2], cv.flip);
        }
    }
    free(mapw); free(acc); free(dir);
    return true;
}

/* =============================================================== dispatch */
bool modelio_load(const char *path, const LoadOpts *lo, char *err) {
    size_t n; uint8_t *d = (uint8_t *)read_file_all(path, &n);
    if (!d) FAIL("cannot read '%s'", path);
    const char *ext = ext_of(path); bool ok;
    if (n >= 4 && rd32(d) == 0x9E2A83C1u) { free(d); FAIL("'%s' is a cooked Unreal package (.uasset) — export it from the editor as FBX/glTF or with ActorX/UModel as .pskx", path); }
    if (!strcasecmp(ext, ".obj")) ok = load_obj(path, d, n, lo, err);
    else if (!strcasecmp(ext, ".stl")) ok = load_stl(d, n, lo, err);
    else if (!strcasecmp(ext, ".ply")) ok = load_ply(d, n, lo, err);
    else if (!strcasecmp(ext, ".gltf") || !strcasecmp(ext, ".glb")) ok = load_gltf(path, d, n, lo, err);
    else if (!strcasecmp(ext, ".fbx")) ok = load_fbx(path, d, n, lo, err);
    else if (!strcasecmp(ext, ".psk") || !strcasecmp(ext, ".pskx")) ok = load_psk(path, d, n, lo, err);
    else if (n >= 4 && !memcmp(d, "glTF", 4)) ok = load_gltf(path, d, n, lo, err);
    else if (n >= 18 && !memcmp(d, "Kaydara FBX Binary", 18)) ok = load_fbx(path, d, n, lo, err);
    else if (n >= 8 && !memcmp(d, "ACTRHEAD", 8)) ok = load_psk(path, d, n, lo, err);
    else if (n >= 4 && !memcmp(d, "ply\n", 4)) ok = load_ply(d, n, lo, err);
    else { free(d); FAIL("unsupported model format '%s' (supported: .obj .gltf .glb .fbx .psk .pskx .stl .ply .kcm)", ext); }
    free(d);
    return ok;
}
