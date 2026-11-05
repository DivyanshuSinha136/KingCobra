/* KingCobra std.system — paths and the filesystem around them.
 *
 * Path functions are pure string manipulation (allowed in comptime) and
 * understand both '/' and — on Windows — '\' separators and drive prefixes.
 * Metadata reads (exists/stat/list_dir/which/realpath) are allowed in
 * comptime, like io.exists. Anything that creates, changes or deletes
 * (mkdir, rmdir, remove_file, rename, copy_file, chdir, chmod, make_temp_*)
 * is refused there.
 *
 * File *contents* stay in std.io; this file is about the tree, not the bytes.
 *
 * Safety: rmdir(path, true) never follows symlinks (it unlinks the link, not
 * its target) and refuses to delete a filesystem root, a drive root, the
 * empty path or the user's home directory.
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "system_glue.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <ctype.h>
#if defined(KC_PLATFORM_WINDOWS)
#include <io.h>
#include <direct.h>
#define SEP '\\'
#else
#include <dirent.h>
#include <fcntl.h>
#define SEP '/'
#endif

#if defined(KC_PLATFORM_WINDOWS)
static bool is_sep(char c) { return c == '/' || c == '\\'; }
static bool has_drive(const char *p) { return isalpha((unsigned char)p[0]) && p[1] == ':'; }
#else
static bool is_sep(char c) { return c == '/'; }
static bool has_drive(const char *p) { (void)p; return false; }
#endif

#define PATH_MAX_LEN 4096

/* ------------------------- pure path functions ------------------------- */

/* Length of the root prefix: "/" -> 1, "C:\" -> 3, "C:" -> 2, "\\srv" -> 2 (UNC), else 0. */
static size_t root_len(const char *p) {
    if (has_drive(p)) return is_sep(p[2]) ? 3 : 2;
#if defined(KC_PLATFORM_WINDOWS)
    if (is_sep(p[0]) && is_sep(p[1])) return 2;
#endif
    return is_sep(p[0]) ? 1 : 0;
}

static bool path_is_abs(const char *p) {
    size_t r = root_len(p);
#if defined(KC_PLATFORM_WINDOWS)
    return (has_drive(p) ? r == 3 : r >= 1);
#else
    return r >= 1;
#endif
}

static KcValue sp_sep(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    sys_argc(n, 0, 0, "system.path_sep() -> string", pos);
    char s[2] = { SEP, 0 };
    return kc_string(s);
}

static KcValue sp_join(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "system.path_join(part: string, ...) -> string  (an absolute part restarts the path)";
    if (n < 1) sys_usage(pos, sig);
    SysBuf b = {0};
    for (int i = 0; i < n; i++) {
        const char *part = sys_str(a, i, sig, pos);
        if (!*part) continue;
        if (path_is_abs(part)) { b.len = 0; if (b.p) b.p[0] = 0; }
        if (b.len && !is_sep(b.p[b.len - 1]) && !(has_drive(b.p) && b.len == 2)) { char s = SEP; sysbuf_append(&b, &s, 1); }
        sysbuf_append(&b, part, strlen(part));
    }
    if (!b.p) return kc_string("");
    return sys_bytes_str(b.p, b.len);
}

/* Start/end of the final component, ignoring trailing separators. */
static void last_component(const char *p, size_t *start, size_t *end) {
    size_t len = strlen(p), root = root_len(p);
    size_t e = len;
    while (e > root && is_sep(p[e - 1])) e--;
    size_t s = e;
    while (s > root && !is_sep(p[s - 1])) s--;
    *start = s; *end = e;
}

static KcValue sp_basename(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "system.basename(path: string) -> string";
    sys_argc(n, 1, 1, sig, pos);
    const char *p = sys_str(a, 0, sig, pos);
    size_t s, e; last_component(p, &s, &e);
    char *out = kc_malloc(e - s + 2); memcpy(out, p + s, e - s);
    return sys_bytes_str(out, e - s);
}

static KcValue sp_dirname(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "system.dirname(path: string) -> string";
    sys_argc(n, 1, 1, sig, pos);
    const char *p = sys_str(a, 0, sig, pos);
    size_t root = root_len(p), s, e; last_component(p, &s, &e);
    size_t d = s;
    while (d > root && is_sep(p[d - 1])) d--;       /* drop the separators before the last component */
    if (d == 0) return kc_string(root ? "" : ".");   /* "a" -> "." */
    if (d == root && root) d = root;                 /* "/a" -> "/" */
    char *out = kc_malloc(d + 2); memcpy(out, p, d);
    return sys_bytes_str(out, d);
}

/* Split basename at its last dot: ".bashrc" has no extension, "a.tar.gz" -> ".gz", "a." -> ".". */
static void split_ext(const char *base, size_t blen, size_t *dot) {
    *dot = blen;
    for (size_t i = blen; i > 1; i--) if (base[i - 1] == '.') { *dot = i - 1; return; }
}

static KcValue sp_extension(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "system.extension(path: string) -> string  (\".gz\" for \"a.tar.gz\"; \"\" if none)";
    sys_argc(n, 1, 1, sig, pos);
    const char *p = sys_str(a, 0, sig, pos);
    size_t s, e; last_component(p, &s, &e);
    size_t dot; split_ext(p + s, e - s, &dot);
    char *out = kc_malloc(e - s - dot + 2); memcpy(out, p + s + dot, e - s - dot);
    return sys_bytes_str(out, e - s - dot);
}

static KcValue sp_stem(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "system.stem(path: string) -> string  (file name without its extension)";
    sys_argc(n, 1, 1, sig, pos);
    const char *p = sys_str(a, 0, sig, pos);
    size_t s, e; last_component(p, &s, &e);
    size_t dot; split_ext(p + s, e - s, &dot);
    char *out = kc_malloc(dot + 2); memcpy(out, p + s, dot);
    return sys_bytes_str(out, dot);
}

static KcValue sp_is_absolute(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "system.is_absolute(path: string) -> bool";
    sys_argc(n, 1, 1, sig, pos);
    return kc_bool(path_is_abs(sys_str(a, 0, sig, pos)));
}

/* Lexical normalization: collapse separators, drop ".", resolve "..". Never touches the disk. */
static char *norm_path(const char *p) {
    size_t root = root_len(p), len = strlen(p);
    size_t cap = len / 2 + 2;
    size_t *st = kc_malloc(cap * sizeof *st * 2);   /* stack of (start, length) pairs */
    size_t top = 0;
    size_t i = root;
    while (i < len) {
        while (i < len && is_sep(p[i])) i++;
        size_t s0 = i; while (i < len && !is_sep(p[i])) i++;
        size_t cl = i - s0;
        if (cl == 0 || (cl == 1 && p[s0] == '.')) continue;
        if (cl == 2 && p[s0] == '.' && p[s0 + 1] == '.') {
            bool top_is_up = top && st[2 * (top - 1) + 1] == 2 && p[st[2 * (top - 1)]] == '.' && p[st[2 * (top - 1)] + 1] == '.';
            if (top && !top_is_up) { top--; continue; }
            if (root) continue;                     /* ".." above an absolute root stays at the root */
        }
        st[2 * top] = s0; st[2 * top + 1] = cl; top++;
    }
    char *out = kc_malloc(len + 4); size_t ol = 0;
    for (size_t k = 0; k < root; k++) out[ol++] = is_sep(p[k]) ? SEP : p[k];
    for (size_t k = 0; k < top; k++) {
        if (ol > 0 && !is_sep(out[ol - 1]) && out[ol - 1] != ':') out[ol++] = SEP;
        memcpy(out + ol, p + st[2 * k], st[2 * k + 1]); ol += st[2 * k + 1];
    }
    if (ol == 0) out[ol++] = '.';
    out[ol] = 0;
    free(st);
    return out;
}

static KcValue sp_normalize(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "system.normalize(path: string) -> string  (lexical: collapses //, resolves . and ..)";
    sys_argc(n, 1, 1, sig, pos);
    char *r = norm_path(sys_str(a, 0, sig, pos));
    return kc_string_take(r, (int)strlen(r));
}

static KcValue sp_abspath(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "system.abspath(path: string) -> string  (relative to the current directory, normalized)";
    sys_argc(n, 1, 1, sig, pos);
    const char *p = sys_str(a, 0, sig, pos);
    char cwd[PATH_MAX_LEN];
    char *full;
    if (path_is_abs(p)) full = kc_strdup(p);
    else {
        if (!kc_getcwd(cwd, sizeof cwd)) { sys_set_errno("system.abspath"); return kc_null(); }
        full = kc_malloc(strlen(cwd) + strlen(p) + 3);
        sprintf(full, "%s%c%s", cwd, SEP, p);
    }
    char *r = norm_path(full); free(full);
    return kc_string_take(r, (int)strlen(r));
}

static KcValue sp_path_parts(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "system.path_parts(path: string) -> array of string  (root first if absolute)";
    sys_argc(n, 1, 1, sig, pos);
    const char *p = sys_str(a, 0, sig, pos);
    KcValue out = kc_array_new();
    size_t root = root_len(p), len = strlen(p), i = root;
    if (root) { char *r = kc_malloc(root + 1); memcpy(r, p, root); r[root] = 0; for (size_t k = 0; k < root; k++) if (is_sep(r[k])) r[k] = SEP; kc_array_push(out, kc_string_take(r, (int)root)); }
    while (i < len) {
        while (i < len && is_sep(p[i])) i++;
        size_t s = i; while (i < len && !is_sep(p[i])) i++;
        if (i > s) { char *c = kc_malloc(i - s + 1); memcpy(c, p + s, i - s); c[i - s] = 0; kc_array_push(out, kc_string_take(c, (int)(i - s))); }
    }
    return out;
}

/* --------------------------- file information --------------------------- */

typedef struct { bool ok; int64_t size; double mtime; bool is_dir, is_file, is_link; int mode; } FInfo;

#if defined(KC_PLATFORM_WINDOWS)
static double ft_to_unix(FILETIME ft) {
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return ((double)t - 116444736000000000.0) / 1e7;
}
static bool finfo(const char *path, FInfo *fi) {
    WIN32_FILE_ATTRIBUTE_DATA d; memset(fi, 0, sizeof *fi);
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) { sys_set_errno(path); return false; }
    fi->ok = true;
    fi->is_dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    fi->is_file = !fi->is_dir;
    fi->is_link = (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    fi->size = fi->is_dir ? 0 : (int64_t)(((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow);
    fi->mtime = ft_to_unix(d.ftLastWriteTime);
    fi->mode = (d.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 0444 : 0666;
    if (fi->is_dir) fi->mode |= 0111;
    return true;
}
#else
static bool finfo_x(const char *path, bool follow, FInfo *fi) {
    struct stat st; memset(fi, 0, sizeof *fi);
    if ((follow ? stat(path, &st) : lstat(path, &st)) != 0) { sys_set_errno(path); return false; }
    fi->ok = true;
    fi->is_dir = S_ISDIR(st.st_mode); fi->is_file = S_ISREG(st.st_mode); fi->is_link = S_ISLNK(st.st_mode);
    fi->size = (int64_t)st.st_size;
    fi->mtime = (double)st.st_mtime;
#if defined(__linux__)
    fi->mtime += (double)st.st_mtim.tv_nsec / 1e9;
#endif
    fi->mode = (int)(st.st_mode & 07777);
    return true;
}
static bool finfo(const char *path, FInfo *fi) { return finfo_x(path, true, fi); }
#endif

static KcValue sp_exists(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "system.exists(path: string) -> bool";
    sys_argc(n, 1, 1, sig, pos);
    FInfo fi; return kc_bool(finfo(sys_str(a, 0, sig, pos), &fi));
}
static KcValue sp_is_dir(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "system.is_dir(path: string) -> bool";
    sys_argc(n, 1, 1, sig, pos);
    FInfo fi; return kc_bool(finfo(sys_str(a, 0, sig, pos), &fi) && fi.is_dir);
}
static KcValue sp_is_file(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "system.is_file(path: string) -> bool  (a regular file)";
    sys_argc(n, 1, 1, sig, pos);
    FInfo fi; return kc_bool(finfo(sys_str(a, 0, sig, pos), &fi) && fi.is_file);
}
static KcValue sp_is_symlink(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "system.is_symlink(path: string) -> bool  (does not follow the link)";
    sys_argc(n, 1, 1, sig, pos);
    const char *p = sys_str(a, 0, sig, pos);
#if defined(KC_PLATFORM_WINDOWS)
    FInfo fi; return kc_bool(finfo(p, &fi) && fi.is_link);
#else
    FInfo fi; return kc_bool(finfo_x(p, false, &fi) && fi.is_link);
#endif
}

static KcValue sp_stat(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "system.stat(path: string) -> FileStat { size, mtime, mode, is_dir, is_file, is_symlink }  (null if missing)";
    sys_argc(n, 1, 1, sig, pos);
    const char *p = sys_str(a, 0, sig, pos);
    FInfo fi;
    if (!finfo(p, &fi)) return kc_null();
#if !defined(KC_PLATFORM_WINDOWS)
    FInfo li; fi.is_link = finfo_x(p, false, &li) && li.is_link; /* stat() follows; ask lstat() about the link itself */
#endif
    static const char *names[] = { "size", "mtime", "mode", "is_dir", "is_file", "is_symlink" };
    KcValue vals[6] = { kc_int(fi.size), kc_float(fi.mtime), kc_int(fi.mode), kc_bool(fi.is_dir), kc_bool(fi.is_file), kc_bool(fi.is_link) };
    return sys_struct("FileStat", 6, names, vals);
}

static KcValue sp_access(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "system.access(path: string, mode: string = \"r\") -> bool  (mode: any of r, w, x)";
    sys_argc(n, 1, 2, sig, pos);
    const char *p = sys_str(a, 0, sig, pos), *m = n > 1 ? sys_str(a, 1, sig, pos) : "r";
    int want = 0;
    for (const char *c = m; *c; c++) {
        if (*c == 'r') want |= 4; else if (*c == 'w') want |= 2; else if (*c == 'x') want |= 1;
        else kc_error_at(pos, "system.access: mode must contain only r, w, x (got '%s')", m);
    }
#if defined(KC_PLATFORM_WINDOWS)
    FInfo fi; if (!finfo(p, &fi)) return kc_bool(false);
    if ((want & 2) && (fi.mode & 0200) == 0) return kc_bool(false);
    if ((want & 1) && !fi.is_dir) { const char *x = strrchr(p, '.'); if (!x || (_stricmp(x, ".exe") && _stricmp(x, ".bat") && _stricmp(x, ".cmd") && _stricmp(x, ".com"))) return kc_bool(false); }
    return kc_bool(true);
#else
    int mode = ((want & 4) ? R_OK : 0) | ((want & 2) ? W_OK : 0) | ((want & 1) ? X_OK : 0);
    return kc_bool(access(p, mode ? mode : F_OK) == 0);
#endif
}

/* -------------------------------- listing -------------------------------- */

static int cmp_names(const void *x, const void *y) { return strcmp(*(char *const *)x, *(char *const *)y); }

static KcValue sp_list_dir(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "system.list_dir(path: string = \".\") -> array of string  (names, sorted; no . or ..)";
    sys_argc(n, 0, 1, sig, pos);
    const char *dir = n ? sys_str(a, 0, sig, pos) : ".";
    char **names = NULL; size_t cnt = 0, cap = 0;
#define PUSH_NAME(s) do { if (cnt == cap) { cap = cap ? cap * 2 : 32; names = kc_realloc(names, cap * sizeof *names); } names[cnt++] = kc_strdup(s); } while (0)
#if defined(KC_PLATFORM_WINDOWS)
    char pat[PATH_MAX_LEN]; snprintf(pat, sizeof pat, "%s%s*", dir, (*dir && !is_sep(dir[strlen(dir) - 1])) ? "\\" : "");
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { sys_set_errno(dir); return kc_null(); }
    do { if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) PUSH_NAME(fd.cFileName); } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) { sys_set_errno(dir); return kc_null(); }
    struct dirent *de;
    while ((de = readdir(d)) != NULL)
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) PUSH_NAME(de->d_name);
    closedir(d);
#endif
    if (cnt > 1) qsort(names, cnt, sizeof *names, cmp_names);
    KcValue out = kc_array_new();
    for (size_t i = 0; i < cnt; i++) kc_array_push(out, kc_string_take(names[i], (int)strlen(names[i])));
    free(names);
    return out;
}

/* --------------------------- create / delete / move --------------------------- */

static bool mk1(const char *p) {
#if defined(KC_PLATFORM_WINDOWS)
    return _mkdir(p) == 0;
#else
    return mkdir(p, 0777) == 0; /* subject to the process umask, like mkdir(1) */
#endif
}

static KcValue sp_mkdir(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "system.mkdir");
    const char *sig = "system.mkdir(path: string, parents: bool = false) -> bool";
    sys_argc(n, 1, 2, sig, pos);
    const char *path = sys_str(a, 0, sig, pos);
    bool parents = sys_opt_bool(a, n, 1, false, sig, pos);
    if (!*path) { sys_set_error("system.mkdir: empty path"); return kc_bool(false); }
    char *buf = kc_strdup(path);
    size_t len = strlen(buf);
    while (len > root_len(buf) && is_sep(buf[len - 1])) buf[--len] = 0;
    FInfo fi;
    if (parents) {
        for (size_t i = root_len(buf) + 1; i < len; i++) {
            if (!is_sep(buf[i])) continue;
            char c = buf[i]; buf[i] = 0;
            if (!(finfo(buf, &fi) && fi.is_dir) && !mk1(buf) && !(finfo(buf, &fi) && fi.is_dir)) {
                sys_set_errno(buf); free(buf); return kc_bool(false);
            }
            buf[i] = c;
        }
        if (finfo(buf, &fi) && fi.is_dir) { free(buf); return kc_bool(true); } /* already there: fine with parents=true */
    }
    bool ok = mk1(buf);
    if (!ok) sys_set_errno(buf);
    free(buf);
    return kc_bool(ok);
}

static bool refuse_dangerous(const char *path) {
    char *abs;
    if (path_is_abs(path)) abs = norm_path(path);
    else {
        char cwd[PATH_MAX_LEN]; if (!kc_getcwd(cwd, sizeof cwd)) return true;
        char *full = kc_malloc(strlen(cwd) + strlen(path) + 3); sprintf(full, "%s%c%s", cwd, SEP, path);
        abs = norm_path(full); free(full);
    }
    size_t rl = root_len(abs), al = strlen(abs);
    bool bad = (al <= rl);                                  /* "/" or "C:\" */
    if (!bad) {
        const char *home = getenv("HOME");
#if defined(KC_PLATFORM_WINDOWS)
        if (!home) home = getenv("USERPROFILE");
#endif
        if (home && *home) { char *h = norm_path(home);
#if defined(KC_PLATFORM_WINDOWS)
            bad = _stricmp(abs, h) == 0;
#else
            bad = strcmp(abs, h) == 0;
#endif
            free(h); }
    }
    free(abs);
    return bad;
}

static bool rm_tree(const char *path, int depth) {
    if (depth > 200) { sys_set_error("system.rmdir: directory tree deeper than 200 levels, giving up"); return false; }
#if defined(KC_PLATFORM_WINDOWS)
    DWORD at = GetFileAttributesA(path);
    if (at == INVALID_FILE_ATTRIBUTES) { sys_set_errno(path); return false; }
    if (at & FILE_ATTRIBUTE_READONLY) SetFileAttributesA(path, at & ~FILE_ATTRIBUTE_READONLY);
    if (!(at & FILE_ATTRIBUTE_DIRECTORY)) { if (!DeleteFileA(path)) { sys_set_errno(path); return false; } return true; }
    if (!(at & FILE_ATTRIBUTE_REPARSE_POINT)) { /* real directory: empty it first; a directory symlink is just removed */
        char pat[PATH_MAX_LEN]; snprintf(pat, sizeof pat, "%s\\*", path);
        WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
                char child[PATH_MAX_LEN]; snprintf(child, sizeof child, "%s\\%s", path, fd.cFileName);
                if (!rm_tree(child, depth + 1)) { FindClose(h); return false; }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    if (!RemoveDirectoryA(path)) { sys_set_errno(path); return false; }
    return true;
#else
    struct stat st;
    if (lstat(path, &st) != 0) { sys_set_errno(path); return false; }
    if (!S_ISDIR(st.st_mode)) { if (unlink(path) != 0) { sys_set_errno(path); return false; } return true; }
    DIR *d = opendir(path);
    if (!d) { sys_set_errno(path); return false; }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char child[PATH_MAX_LEN];
        if (snprintf(child, sizeof child, "%s/%s", path, de->d_name) >= (int)sizeof child) { closedir(d); sys_set_error("system.rmdir: path too long under %s", path); return false; }
        if (!rm_tree(child, depth + 1)) { closedir(d); return false; }
    }
    closedir(d);
    if (rmdir(path) != 0) { sys_set_errno(path); return false; }
    return true;
#endif
}

static KcValue sp_rmdir(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "system.rmdir");
    const char *sig = "system.rmdir(path: string, recursive: bool = false) -> bool";
    sys_argc(n, 1, 2, sig, pos);
    const char *path = sys_str(a, 0, sig, pos);
    bool rec = sys_opt_bool(a, n, 1, false, sig, pos);
    if (!*path) { sys_set_error("system.rmdir: empty path"); return kc_bool(false); }
    if (!rec) {
#if defined(KC_PLATFORM_WINDOWS)
        bool ok = RemoveDirectoryA(path) != 0;
#else
        bool ok = rmdir(path) == 0;
#endif
        if (!ok) sys_set_errno(path);
        return kc_bool(ok);
    }
    if (refuse_dangerous(path)) { sys_set_error("system.rmdir: refusing to recursively delete '%s' (a filesystem root or the home directory)", path); return kc_bool(false); }
    FInfo fi;
    if (!finfo(path, &fi)) return kc_bool(false);
    if (!fi.is_dir) { sys_set_error("system.rmdir: '%s' is not a directory (use remove_file)", path); return kc_bool(false); }
    return kc_bool(rm_tree(path, 0));
}

static KcValue sp_remove_file(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "system.remove_file");
    const char *sig = "system.remove_file(path: string) -> bool  (a file or symlink; never a directory)";
    sys_argc(n, 1, 1, sig, pos);
    const char *p = sys_str(a, 0, sig, pos);
#if defined(KC_PLATFORM_WINDOWS)
    bool ok = DeleteFileA(p) != 0;
#else
    bool ok = unlink(p) == 0;
#endif
    if (!ok) sys_set_errno(p);
    return kc_bool(ok);
}

static KcValue sp_rename(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "system.rename");
    const char *sig = "system.rename(from: string, to: string) -> bool  (replaces an existing file at `to`)";
    sys_argc(n, 2, 2, sig, pos);
    const char *f = sys_str(a, 0, sig, pos), *t = sys_str(a, 1, sig, pos);
#if defined(KC_PLATFORM_WINDOWS)
    bool ok = MoveFileExA(f, t, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0;
#else
    bool ok = rename(f, t) == 0;
#endif
    if (!ok) sys_set_errno("system.rename");
    return kc_bool(ok);
}

/* Copies via a sibling temp file and an atomic rename, so a failure part-way
 * never leaves a half-written destination behind. */
static KcValue sp_copy_file(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "system.copy_file");
    const char *sig = "system.copy_file(from: string, to: string, overwrite: bool = true) -> bool";
    sys_argc(n, 2, 3, sig, pos);
    const char *src = sys_str(a, 0, sig, pos), *dst = sys_str(a, 1, sig, pos);
    bool overwrite = sys_opt_bool(a, n, 2, true, sig, pos);
    FInfo si, di;
    if (!finfo(src, &si)) return kc_bool(false);
    if (!si.is_file) { sys_set_error("system.copy_file: '%s' is not a regular file", src); return kc_bool(false); }
    if (finfo(dst, &di)) {
        if (di.is_dir) { sys_set_error("system.copy_file: destination '%s' is a directory", dst); return kc_bool(false); }
        if (!overwrite) { sys_set_error("system.copy_file: '%s' already exists", dst); return kc_bool(false); }
    }
    char tmp[PATH_MAX_LEN];
    if (snprintf(tmp, sizeof tmp, "%s.kc-copy-%ld.tmp", dst, kc_getpid()) >= (int)sizeof tmp) { sys_set_error("system.copy_file: path too long"); return kc_bool(false); }
    FILE *in = fopen(src, "rb");
    if (!in) { sys_set_errno(src); return kc_bool(false); }
    FILE *out = fopen(tmp, "wb");
    if (!out) { sys_set_errno(tmp); fclose(in); return kc_bool(false); }
    char buf[65536]; size_t got; bool ok = true;
    while ((got = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, got, out) != got) { ok = false; break; }
    if (ferror(in)) ok = false;
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (!ok) { sys_set_errno("system.copy_file: write failed"); remove(tmp); return kc_bool(false); }
#if defined(KC_PLATFORM_WINDOWS)
    ok = MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    chmod(tmp, (mode_t)(si.mode & 0777)); /* keep the permission bits, as cp -p would */
    ok = rename(tmp, dst) == 0;
#endif
    if (!ok) { sys_set_errno("system.copy_file: rename"); remove(tmp); }
    return kc_bool(ok);
}

static KcValue sp_chdir(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "system.chdir");
    const char *sig = "system.chdir(path: string) -> bool";
    sys_argc(n, 1, 1, sig, pos);
    const char *p = sys_str(a, 0, sig, pos);
#if defined(KC_PLATFORM_WINDOWS)
    bool ok = _chdir(p) == 0;
#else
    bool ok = chdir(p) == 0;
#endif
    if (!ok) sys_set_errno(p);
    return kc_bool(ok);
}

static KcValue sp_chmod(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "system.chmod");
    const char *sig = "system.chmod(path: string, mode: int) -> bool  (e.g. 0o755 written as 493; Windows only honours the write bit)";
    sys_argc(n, 2, 2, sig, pos);
    const char *p = sys_str(a, 0, sig, pos); int64_t m = sys_int(a, 1, sig, pos);
    if (m < 0 || m > 07777) kc_error_at(pos, "system.chmod: mode must be 0..4095 (octal 0..07777)");
#if defined(KC_PLATFORM_WINDOWS)
    bool ok = _chmod(p, (m & 0200) ? (_S_IREAD | _S_IWRITE) : _S_IREAD) == 0;
#else
    bool ok = chmod(p, (mode_t)m) == 0;
#endif
    if (!ok) sys_set_errno(p);
    return kc_bool(ok);
}

/* Unique names that nothing else can claim between "pick" and "create":
 * mkstemp/mkdtemp on POSIX, CREATE_NEW / CreateDirectory-until-unique on Windows. */
static KcValue make_temp(KcInterp *ip, KcValue *a, int n, KcPos pos, bool dir) {
    const char *who = dir ? "system.make_temp_dir" : "system.make_temp_file";
    SYS_IMPURE(ip, pos, who);
    char sig[160]; snprintf(sig, sizeof sig, "%s(prefix: string = \"kc\") -> string  (creates it; the caller deletes it)", who);
    sys_argc(n, 0, 1, sig, pos);
    const char *prefix = n ? sys_str(a, 0, sig, pos) : "kc";
    for (const char *c = prefix; *c; c++) if (is_sep(*c)) kc_error_at(pos, "%s: prefix must not contain a path separator", who);
    if (strlen(prefix) > 64) kc_error_at(pos, "%s: prefix too long (max 64)", who);
    char base[PATH_MAX_LEN];
#if defined(KC_PLATFORM_WINDOWS)
    char td[MAX_PATH + 2]; DWORD tl = GetTempPathA(sizeof td, td);
    if (tl == 0 || tl >= sizeof td) { sys_set_errno(who); return kc_null(); }
    static unsigned counter;
    for (int tries = 0; tries < 100; tries++) {
        snprintf(base, sizeof base, "%s%s%lx%llx%x", td, prefix, (unsigned long)kc_getpid(), (unsigned long long)GetTickCount64(), counter++);
        if (dir) { if (CreateDirectoryA(base, NULL)) return kc_string(base); if (GetLastError() != ERROR_ALREADY_EXISTS) break; }
        else {
            HANDLE h = CreateFileA(base, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return kc_string(base); }
            if (GetLastError() != ERROR_FILE_EXISTS) break;
        }
    }
    sys_set_errno(who); return kc_null();
#else
    const char *td = getenv("TMPDIR"); if (!td || !*td) td = "/tmp";
    if (snprintf(base, sizeof base, "%s/%sXXXXXX", td, prefix) >= (int)sizeof base) { sys_set_error("%s: path too long", who); return kc_null(); }
    if (dir) { if (!mkdtemp(base)) { sys_set_errno(who); return kc_null(); } }
    else { int fd = mkstemp(base); if (fd < 0) { sys_set_errno(who); return kc_null(); } close(fd); }
    return kc_string(base);
#endif
}
static KcValue sp_make_temp_file(KcInterp *ip, KcValue *a, int n, KcPos pos) { return make_temp(ip, a, n, pos, false); }
static KcValue sp_make_temp_dir(KcInterp *ip, KcValue *a, int n, KcPos pos) { return make_temp(ip, a, n, pos, true); }

/* ------------------------- realpath / which ------------------------- */

static KcValue sp_realpath(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "system.realpath(path: string) -> string  (absolute, symlinks resolved; null if it doesn't exist)";
    sys_argc(n, 1, 1, sig, pos);
    const char *p = sys_str(a, 0, sig, pos);
#if defined(KC_PLATFORM_WINDOWS)
    char buf[PATH_MAX_LEN]; FInfo fi;
    if (!finfo(p, &fi)) return kc_null();
    DWORD len = GetFullPathNameA(p, sizeof buf, buf, NULL);
    if (len == 0 || len >= sizeof buf) { sys_set_errno(p); return kc_null(); }
    return kc_string(buf);
#else
    char buf[PATH_MAX_LEN];
    if (!realpath(p, buf)) { sys_set_errno(p); return kc_null(); }
    return kc_string(buf);
#endif
}

static bool is_exec_file(const char *p) {
    FInfo fi;
    if (!finfo(p, &fi) || !fi.is_file) return false;
#if defined(KC_PLATFORM_WINDOWS)
    return true;
#else
    return access(p, X_OK) == 0;
#endif
}

/* Resolve a command name the way a shell would: search PATH (and PATHEXT on Windows). */
static KcValue sp_which(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "system.which(command: string) -> string  (full path of the executable, or null)";
    sys_argc(n, 1, 1, sig, pos);
    const char *cmd = sys_str(a, 0, sig, pos);
    if (!*cmd) return kc_null();
#if defined(KC_PLATFORM_WINDOWS)
    const char *pathext = getenv("PATHEXT"); if (!pathext || !*pathext) pathext = ".COM;.EXE;.BAT;.CMD";
    const char psep = ';';
#else
    const char *pathext = ""; const char psep = ':';
#endif
    bool has_dir = false; for (const char *c = cmd; *c; c++) if (is_sep(*c)) has_dir = true;
    const char *path = getenv("PATH"); if (!path) path = "";
    char cand[PATH_MAX_LEN];
    /* try `base` as-is and then with each PATHEXT suffix */
#define TRY(base) do { \
        if (is_exec_file(base)) return kc_string(base); \
        const char *e = pathext; \
        while (*e) { const char *end = strchr(e, ';'); size_t el = end ? (size_t)(end - e) : strlen(e); \
            char withext[PATH_MAX_LEN]; if (el && snprintf(withext, sizeof withext, "%s%.*s", base, (int)el, e) < (int)sizeof withext && is_exec_file(withext)) return kc_string(withext); \
            e += el; if (*e == ';') e++; } \
    } while (0)
    if (has_dir) { TRY(cmd); return kc_null(); }
    const char *s = path;
    for (;;) {
        const char *end = strchr(s, psep); size_t dl = end ? (size_t)(end - s) : strlen(s);
        if (dl == 0) snprintf(cand, sizeof cand, "%s", cmd);               /* empty PATH entry means the current directory */
        else if (snprintf(cand, sizeof cand, "%.*s%c%s", (int)dl, s, SEP, cmd) >= (int)sizeof cand) { if (!end) break; s = end + 1; continue; }
        TRY(cand);
        if (!end) break;
        s = end + 1;
    }
    sys_set_error("system.which: '%s' not found in PATH", cmd);
    return kc_null();
}

const SysEntry sys_path_entries[] = {
    {"path_sep", sp_sep}, {"path_join", sp_join}, {"basename", sp_basename}, {"dirname", sp_dirname},
    {"extension", sp_extension}, {"stem", sp_stem}, {"is_absolute", sp_is_absolute},
    {"normalize", sp_normalize}, {"abspath", sp_abspath}, {"path_parts", sp_path_parts},
    {"exists", sp_exists}, {"is_dir", sp_is_dir}, {"is_file", sp_is_file}, {"is_symlink", sp_is_symlink},
    {"stat", sp_stat}, {"access", sp_access}, {"list_dir", sp_list_dir},
    {"mkdir", sp_mkdir}, {"rmdir", sp_rmdir}, {"remove_file", sp_remove_file}, {"rename", sp_rename},
    {"copy_file", sp_copy_file}, {"chdir", sp_chdir}, {"chmod", sp_chmod},
    {"make_temp_file", sp_make_temp_file}, {"make_temp_dir", sp_make_temp_dir},
    {"realpath", sp_realpath}, {"which", sp_which},
};
const int sys_path_count = (int)(sizeof sys_path_entries / sizeof sys_path_entries[0]);
