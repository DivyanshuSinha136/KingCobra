#include "util.h"
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#include <limits.h>
#include <unistd.h>
#endif

void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) { fputs("kcide: out of memory\n", stderr); exit(70); } return p; }
void *xcalloc(size_t n, size_t sz) { void *p = calloc(n ? n : 1, sz ? sz : 1); if (!p) { fputs("kcide: out of memory\n", stderr); exit(70); } return p; }
void *xrealloc(void *p, size_t n) { p = realloc(p, n ? n : 1); if (!p) { fputs("kcide: out of memory\n", stderr); exit(70); } return p; }
char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }
char *xstrndup(const char *s, size_t n) { char *r = xmalloc(n + 1); memcpy(r, s, n); r[n] = 0; return r; }
char *xfmt(const char *fmt, ...) {
    va_list ap, ap2; va_start(ap, fmt); va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap); va_end(ap);
    char *r = xmalloc((size_t)n + 1); vsnprintf(r, (size_t)n + 1, fmt, ap2); va_end(ap2);
    return r;
}

void str_init(Str *s) { s->cap = 32; s->n = 0; s->p = xmalloc(s->cap); s->p[0] = 0; }
void str_free(Str *s) { free(s->p); s->p = NULL; s->n = s->cap = 0; }
void str_clear(Str *s) { s->n = 0; if (s->p) s->p[0] = 0; }
static void str_reserve(Str *s, size_t extra) {
    if (!s->p) str_init(s);
    if (s->n + extra + 1 <= s->cap) return;
    while (s->n + extra + 1 > s->cap) s->cap *= 2;
    s->p = xrealloc(s->p, s->cap);
}
void str_add(Str *s, const char *d, size_t n) { str_reserve(s, n); memcpy(s->p + s->n, d, n); s->n += n; s->p[s->n] = 0; }
void str_adds(Str *s, const char *d) { str_add(s, d, strlen(d)); }
void str_addc(Str *s, char c) { str_add(s, &c, 1); }
void str_addf(Str *s, const char *fmt, ...) {
    va_list ap, ap2; va_start(ap, fmt); va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap); va_end(ap);
    str_reserve(s, (size_t)n); vsnprintf(s->p + s->n, (size_t)n + 1, fmt, ap2); va_end(ap2); s->n += (size_t)n;
}
char *str_take(Str *s) { if (!s->p) str_init(s); char *r = s->p; s->p = NULL; s->n = s->cap = 0; return r; }

int utf8_decode(const char *s, size_t n, uint32_t *cp) {
    const unsigned char *u = (const unsigned char *)s;
    if (n == 0) { *cp = 0; return 0; }
    if (u[0] < 0x80) { *cp = u[0]; return 1; }
    int len = (u[0] >= 0xF0 && u[0] < 0xF8) ? 4 : (u[0] >= 0xE0) ? 3 : (u[0] >= 0xC2) ? 2 : 0;
    if (u[0] >= 0xF8 || len == 0 || (size_t)len > n) { *cp = 0xFFFD; return 1; }
    uint32_t c = len == 2 ? (u[0] & 0x1F) : len == 3 ? (u[0] & 0x0F) : (u[0] & 0x07);
    for (int i = 1; i < len; i++) { if ((u[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; } c = (c << 6) | (u[i] & 0x3F); }
    *cp = c; return len;
}
int utf8_encode(uint32_t c, char *o) {
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 0x3F)); o[2] = (char)(0x80 | (c & 0x3F)); return 3; }
    o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 0x3F)); o[2] = (char)(0x80 | ((c >> 6) & 0x3F)); o[3] = (char)(0x80 | (c & 0x3F)); return 4;
}
int utf8_prev(const char *s, int i) { if (i <= 0) return 0; i--; while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) i--; return i; }
int cp_width(uint32_t c) {
    if (c == 0) return 0;
    if (c < 0x300) return 1;
    if ((c >= 0x300 && c <= 0x36F) || (c >= 0x200B && c <= 0x200F) || (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0x20D0 && c <= 0x20FF)) return 0;
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF) || (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) ||
        (c >= 0xFE30 && c <= 0xFE6F) || (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1F64F) ||
        (c >= 0x1F900 && c <= 0x1F9FF) || (c >= 0x20000 && c <= 0x3FFFD)) return 2;
    return 1;
}
bool cp_is_word(uint32_t c) { return c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80; }
bool cp_is_space(uint32_t c) { return c == ' ' || c == '\t' || c == 0xA0 || c == '\r' || c == '\n'; }
uint32_t cp_lower(uint32_t c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

int fuzzy_score(const char *pat, const char *s) {
    if (!*pat) return 0;
    int score = 0, last = -2, i = 0;
    const char *p = pat;
    for (; s[i] && *p; i++) {
        if (tolower((unsigned char)s[i]) == tolower((unsigned char)*p)) {
            score += 10;
            if (last == i - 1) score += 15;                                           /* consecutive */
            if (i == 0 || strchr("/\\_-. ", s[i - 1])) score += 12;                   /* word start */
            if (s[i] == *p) score += 2;                                               /* case match */
            last = i; p++;
        }
    }
    if (*p) return -1;
    score -= (int)strlen(s) / 8;
    const char *base = path_basename(s);
    if (strstr(base, pat)) score += 40;
    return score;
}

bool path_is_abs(const char *p) {
#ifdef _WIN32
    return (p[0] && p[1] == ':') || p[0] == '\\' || p[0] == '/';
#else
    return p[0] == '/';
#endif
}
char *path_join(const char *a, const char *b) {
    if (!a || !*a || path_is_abs(b)) return xstrdup(b);
    size_t n = strlen(a);
    bool sep = a[n - 1] == '/' || a[n - 1] == '\\';
    return xfmt("%s%s%s", a, sep ? "" : (const char[]){PATH_SEP, 0}, b);
}
const char *path_basename(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) if (*q == '/' || *q == '\\') b = q + 1;
    return b;
}
char *path_dirname(const char *p) {
    const char *b = path_basename(p);
    if (b == p) return xstrdup(".");
    size_t n = (size_t)(b - p);
    if (n > 1) n--;
    return xstrndup(p, n);
}
const char *path_ext(const char *p) { const char *b = path_basename(p), *d = strrchr(b, '.'); return d ? d : ""; }
char *path_abs(const char *p) {
#ifdef _WIN32
    char buf[MAX_PATH * 2]; if (_fullpath(buf, p, sizeof buf)) return xstrdup(buf); return xstrdup(p);
#else
    char buf[PATH_MAX];
    if (realpath(p, buf)) return xstrdup(buf);
    if (path_is_abs(p)) return xstrdup(p);
    char cwd[PATH_MAX]; if (!getcwd(cwd, sizeof cwd)) return xstrdup(p);
    return path_join(cwd, p);
#endif
}
bool is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR; }
bool is_file(const char *p) { struct stat st; return stat(p, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG; }
long file_mtime(const char *p) { struct stat st; return stat(p, &st) == 0 ? (long)st.st_mtime : 0; }
bool path_eq(const char *a, const char *b) {
    if (!a || !b) return false;
#ifdef _WIN32
    return _stricmp(a, b) == 0;
#else
    return strcmp(a, b) == 0;
#endif
}
char *path_rel(const char *root, const char *p) {
    size_t n = root ? strlen(root) : 0;
    if (n && strncmp(root, p, n) == 0 && (p[n] == '/' || p[n] == '\\')) return xstrdup(p + n + 1);
    return xstrdup(p);
}
bool make_dir(const char *p) {
#ifdef _WIN32
    return _mkdir(p) == 0 || errno == EEXIST;
#else
    return mkdir(p, 0755) == 0 || errno == EEXIST;
#endif
}
char *home_config_dir(void) {
#ifdef _WIN32
    const char *base = getenv("APPDATA"); if (!base) return NULL;
    char *d = path_join(base, "kcide"); make_dir(d); return d;
#else
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char *base = xdg && *xdg ? xstrdup(xdg) : home ? path_join(home, ".config") : NULL;
    if (!base) return NULL;
    make_dir(base);
    char *d = path_join(base, "kcide"); free(base); make_dir(d); return d;
#endif
}

static int dcmp(const void *a, const void *b) {
    const DirEnt *x = a, *y = b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;
    const char *p = x->name, *q = y->name;
    for (; *p && *q; p++, q++) { int c = tolower((unsigned char)*p) - tolower((unsigned char)*q); if (c) return c; }
    return (unsigned char)*p - (unsigned char)*q;
}
static void dl_add(DirList *d, const char *name, bool dir) {
    if (d->n == d->cap) { d->cap = d->cap ? d->cap * 2 : 32; d->v = xrealloc(d->v, (size_t)d->cap * sizeof *d->v); }
    d->v[d->n].name = xstrdup(name); d->v[d->n].dir = dir; d->n++;
}
bool dir_list(const char *path, DirList *out) {
    out->v = NULL; out->n = out->cap = 0;
#ifdef _WIN32
    char *pat = path_join(path, "*"); WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd); free(pat);
    if (h == INVALID_HANDLE_VALUE) return false;
    do { if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) dl_add(out, fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0); } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(path); if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char *full = path_join(path, e->d_name);
        dl_add(out, e->d_name, is_dir(full)); free(full);
    }
    closedir(d);
#endif
    qsort(out->v, (size_t)out->n, sizeof *out->v, dcmp);
    return true;
}
void dir_list_free(DirList *d) { for (int i = 0; i < d->n; i++) free(d->v[i].name); free(d->v); d->v = NULL; d->n = d->cap = 0; }

char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    Str s; str_init(&s); char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) str_add(&s, buf, n);
    fclose(f);
    if (len) *len = s.n;
    return str_take(&s);
}
bool write_file_atomic(const char *path, const char *data, size_t len) {
    char *tmp = xfmt("%s.kcide-tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(tmp); return false; }
    bool ok = fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (ok) {
#ifdef _WIN32
        remove(path);
#endif
        ok = rename(tmp, path) == 0;
    }
    if (!ok) remove(tmp);
    free(tmp);
    return ok;
}
uint64_t now_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}
bool find_in_path(const char *exe, char *out, size_t cap) {
    const char *pth = getenv("PATH"); if (!pth) return false;
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    while (*pth) {
        const char *e = strchr(pth, sep); size_t n = e ? (size_t)(e - pth) : strlen(pth);
        if (n) {
            char *dir = xstrndup(pth, n);
            char *full = path_join(dir, exe);
#ifdef _WIN32
            char *exe2 = xfmt("%s.exe", full);
            if (is_file(exe2)) { snprintf(out, cap, "%s", exe2); free(exe2); free(full); free(dir); return true; }
            free(exe2);
#endif
            bool ok = is_file(full);
#ifndef _WIN32
            ok = ok && access(full, X_OK) == 0;
#endif
            if (ok) { snprintf(out, cap, "%s", full); free(full); free(dir); return true; }
            free(full); free(dir);
        }
        if (!e) break;
        pth = e + 1;
    }
    return false;
}
