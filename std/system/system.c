/* KingCobra std.system — native OS interaction.
 *
 * v0.1 was one 80-line file with eight entry points (exec/getenv/setenv/
 * platform/cwd/sleep/exit/pid). This module now aggregates five focused
 * files (see system_glue.h) the way std.nn and std.rhex aggregate theirs:
 *
 *   system.c        (this file) module table, environment variables, script
 *                   arguments, last_error(), shared argument/value helpers
 *   system_info.c   OS / arch / CPU / memory / user / terminal / disk info
 *   system_path.c   path functions + directory and file-metadata operations
 *   system_time.c   clocks, sleep, date formatting, random bytes
 *   system_proc.c   shell commands, argv commands (no shell), spawn/wait/kill
 *
 * Every v0.1 entry keeps its name and its result type, so existing scripts
 * run unchanged. Two v0.1 behaviours were bugs and are fixed: on Windows,
 * setenv() used SetEnvironmentVariable but getenv() read the C runtime's
 * stale copy of the environment (so a value just set was invisible); and
 * setenv() dereferenced its arguments without checking they were strings.
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 * License: MIT
 */
#include "system_glue.h"
#include <ctype.h>
#if !defined(KC_PLATFORM_WINDOWS)
#include <sys/wait.h>
extern char **environ;
#endif

/* ============================== errors ============================== */

static char g_err[320];

void sys_set_error(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
}

void sys_set_errno(const char *what) {
#if defined(KC_PLATFORM_WINDOWS)
    DWORD code = GetLastError();
    char msg[200] = "";
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, msg, sizeof msg, NULL);
    while (n > 0 && (msg[n-1] == '\r' || msg[n-1] == '\n' || msg[n-1] == ' ')) msg[--n] = 0;
    if (!n && errno) snprintf(msg, sizeof msg, "%s", strerror(errno));
    sys_set_error("%s: %s (code %lu)", what, n || errno ? msg : "unknown error", (unsigned long)code);
#else
    sys_set_error("%s: %s", what, strerror(errno));
#endif
}

const char *sys_get_error(void) { return g_err; }

static KcValue sys_last_error(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    sys_argc(n, 0, 0, "system.last_error() -> string", pos);
    return kc_string(g_err);
}

static KcValue sys_clear_error(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    sys_argc(n, 0, 0, "system.clear_error()", pos);
    g_err[0] = 0;
    return kc_null();
}

/* ========================= argument helpers ========================= */

void sys_usage(KcPos pos, const char *sig) { kc_error_at(pos, "%s", sig); }

void sys_argc(int n, int lo, int hi, const char *sig, KcPos pos) {
    if (n < lo || n > hi) sys_usage(pos, sig);
}

const char *sys_str(KcValue *a, int i, const char *sig, KcPos pos) {
    if (a[i].type != VAL_STRING) sys_usage(pos, sig);
    return a[i].as.s->data;
}

int64_t sys_int(KcValue *a, int i, const char *sig, KcPos pos) {
    if (a[i].type != VAL_INT) sys_usage(pos, sig);
    return a[i].as.i;
}

double sys_num(KcValue *a, int i, const char *sig, KcPos pos) {
    if (a[i].type == VAL_INT) return (double)a[i].as.i;
    if (a[i].type == VAL_FLOAT) return a[i].as.f;
    sys_usage(pos, sig);
    return 0;
}

bool sys_is_nullish(KcValue *a, int n, int i) { return i >= n || a[i].type == VAL_NULL; }

bool sys_opt_bool(KcValue *a, int n, int i, bool def, const char *sig, KcPos pos) {
    if (sys_is_nullish(a, n, i)) return def;
    if (a[i].type != VAL_BOOL) sys_usage(pos, sig);
    return a[i].as.b;
}

double sys_opt_num(KcValue *a, int n, int i, double def, const char *sig, KcPos pos) {
    if (sys_is_nullish(a, n, i)) return def;
    return sys_num(a, i, sig, pos);
}

KcValue sys_str_val(const char *s) { return kc_string(s ? s : ""); }

KcValue sys_bytes_str(char *owned, size_t len) {
    /* Callers pass a buffer with room for a terminating NUL at owned[len]. */
    if (len > 0x7fffffffu) len = 0x7fffffffu;
    owned[len] = 0;
    return kc_string_take(owned, (int)len);
}

KcValue sys_struct(const char *type, int n, const char *const *names, const KcValue *vals) {
    KcStructInst *inst = kc_struct_alloc();
    inst->type_name = kc_strdup(type);
    inst->count = n;
    inst->names = kc_malloc(sizeof(char *) * (n ? n : 1));
    inst->values = kc_malloc(sizeof(KcValue) * (n ? n : 1));
    for (int i = 0; i < n; i++) { inst->names[i] = kc_strdup(names[i]); inst->values[i] = vals[i]; }
    KcValue v; v.type = VAL_STRUCT; v.as.strct = inst;
    return v;
}

void sysbuf_append(SysBuf *b, const char *d, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        while (b->len + n + 1 > nc) nc *= 2;
        b->p = kc_realloc(b->p, nc); b->cap = nc;
    }
    memcpy(b->p + b->len, d, n); b->len += n; b->p[b->len] = 0;
}

void sysbuf_free(SysBuf *b) { free(b->p); b->p = NULL; b->len = b->cap = 0; }

/* ============================ environment ============================ */

/* Windows: SetEnvironmentVariable edits the process environment block but
 * the CRT's getenv() reads its own cached copy, so a value just set was
 * invisible to getenv(). Use the Win32 getter so both sides agree. */
static char *env_lookup(const char *name) {
#if defined(KC_PLATFORM_WINDOWS)
    DWORD need = GetEnvironmentVariableA(name, NULL, 0);
    if (need == 0) return NULL;
    char *buf = kc_malloc(need + 1);
    DWORD got = GetEnvironmentVariableA(name, buf, need + 1);
    if (got == 0 || got > need) { free(buf); return NULL; }
    buf[got] = 0;
    return buf;
#else
    const char *v = getenv(name);
    return v ? kc_strdup(v) : NULL;
#endif
}

static bool valid_env_name(const char *s) { return s[0] && !strchr(s, '='); }

static KcValue sys_exec(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "system.exec");
    sys_argc(n, 1, 1, "system.exec(cmd: string) -> int", pos);
    const char *cmd = sys_str(a, 0, "system.exec(cmd: string) -> int", pos);
    fflush(stdout); fflush(stderr); /* keep our buffered output ordered before the child's */
    int rc = system(cmd);
#if defined(KC_PLATFORM_WINDOWS)
    return kc_int(rc); /* Windows system() already returns the plain exit code */
#else
    if (rc == -1) { sys_set_errno("system.exec"); return kc_int(-1); }
    if (WIFEXITED(rc)) return kc_int(WEXITSTATUS(rc));
    if (WIFSIGNALED(rc)) return kc_int(128 + WTERMSIG(rc)); /* same convention as a shell's $? */
    return kc_int(rc);
#endif
}

static KcValue sys_getenv(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    sys_argc(n, 1, 1, "system.getenv(name: string) -> string", pos);
    char *v = env_lookup(sys_str(a, 0, "system.getenv(name: string) -> string", pos));
    if (!v) return kc_string("");
    return kc_string_take(v, (int)strlen(v));
}

static KcValue sys_getenv_or(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "system.getenv_or(name: string, default: string) -> string";
    sys_argc(n, 2, 2, sig, pos);
    char *v = env_lookup(sys_str(a, 0, sig, pos));
    if (!v) { sys_str(a, 1, sig, pos); return a[1]; }
    return kc_string_take(v, (int)strlen(v));
}

static KcValue sys_has_env(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "system.has_env(name: string) -> bool";
    sys_argc(n, 1, 1, sig, pos);
    char *v = env_lookup(sys_str(a, 0, sig, pos));
    bool has = v != NULL; free(v);
    return kc_bool(has);
}

static KcValue sys_setenv(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "system.setenv");
    const char *sig = "system.setenv(name: string, value: string)";
    sys_argc(n, 2, 2, sig, pos);
    const char *name = sys_str(a, 0, sig, pos), *val = sys_str(a, 1, sig, pos);
    if (!valid_env_name(name)) kc_error_at(pos, "system.setenv: invalid variable name '%s' (empty, or contains '=')", name);
    kc_setenv(name, val);
    return kc_null();
}

static KcValue sys_unsetenv(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "system.unsetenv");
    const char *sig = "system.unsetenv(name: string)";
    sys_argc(n, 1, 1, sig, pos);
    const char *name = sys_str(a, 0, sig, pos);
    if (!valid_env_name(name)) kc_error_at(pos, "system.unsetenv: invalid variable name '%s'", name);
#if defined(KC_PLATFORM_WINDOWS)
    SetEnvironmentVariableA(name, NULL);
#else
    unsetenv(name);
#endif
    return kc_null();
}

/* All variables as an array of "NAME=value" strings. */
static KcValue sys_env_list(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    sys_argc(n, 0, 0, "system.env_list() -> array of \"NAME=value\"", pos);
    KcValue out = kc_array_new();
#if defined(KC_PLATFORM_WINDOWS)
    char *blk = GetEnvironmentStringsA();
    if (blk) {
        for (const char *p = blk; *p; p += strlen(p) + 1)
            if (*p != '=') kc_array_push(out, kc_string(p)); /* skip hidden per-drive cwd entries like "=C:=..." */
        FreeEnvironmentStringsA(blk);
    }
#else
    if (environ) for (char **e = environ; *e; e++) kc_array_push(out, kc_string(*e));
#endif
    return out;
}

/* $NAME, ${NAME}, $$ (a literal $), and a leading ~ or ~/ (home directory).
 * An unset variable expands to the empty string, like a shell. */
static KcValue sys_expand_env(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip;
    const char *sig = "system.expand_env(text: string) -> string";
    sys_argc(n, 1, 1, sig, pos);
    const char *s = sys_str(a, 0, sig, pos);
    SysBuf out = {0};
    if (s[0] == '~' && (s[1] == 0 || s[1] == '/' || s[1] == '\\')) {
        char *h = env_lookup("HOME");
#if defined(KC_PLATFORM_WINDOWS)
        if (!h) h = env_lookup("USERPROFILE");
#endif
        if (h) { sysbuf_append(&out, h, strlen(h)); free(h); s++; }
    }
    for (; *s; s++) {
        if (*s != '$') { sysbuf_append(&out, s, 1); continue; }
        if (s[1] == '$') { sysbuf_append(&out, "$", 1); s++; continue; }
        char name[256]; size_t nl = 0; const char *q = s + 1; bool braced = false;
        if (*q == '{') { braced = true; q++; }
        while ((isalnum((unsigned char)*q) || *q == '_') && nl < sizeof name - 1) name[nl++] = *q++;
        name[nl] = 0;
        if (braced) { if (*q != '}') { sysbuf_append(&out, "$", 1); continue; } q++; }
        if (nl == 0) { sysbuf_append(&out, "$", 1); continue; }
        char *v = env_lookup(name);
        if (v) { sysbuf_append(&out, v, strlen(v)); free(v); }
        s = q - 1;
    }
    if (!out.p) return kc_string("");
    return sys_bytes_str(out.p, out.len);
}

/* ====================== platform / process basics ====================== */

static KcValue sys_platform(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a; (void)n; (void)pos;
#if defined(KC_PLATFORM_WINDOWS)
    return kc_string("windows");
#else
    return kc_string("linux");
#endif
}

static KcValue sys_cwd(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    sys_argc(n, 0, 0, "system.cwd() -> string", pos);
    char buf[4096];
    if (!kc_getcwd(buf, sizeof buf)) { sys_set_errno("system.cwd"); return kc_null(); }
    return kc_string(buf);
}

static KcValue sys_exit(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    SYS_IMPURE(ip, pos, "system.exit");
    sys_argc(n, 0, 1, "system.exit(code: int = 0)", pos);
    int code = 0;
    if (n >= 1) { if (a[0].type != VAL_INT) sys_usage(pos, "system.exit(code: int = 0)"); code = (int)(a[0].as.i & 0xff); }
    fflush(stdout); fflush(stderr);
    exit(code);
}

static KcValue sys_pid(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a; (void)n; (void)pos;
    return kc_int(kc_getpid());
}

/* ========================== script arguments ========================== */

static int    g_argc;
static char **g_argv;

void kc_system_set_args(int argc, char **argv) { g_argc = argc; g_argv = argv; }

/* Arguments after the script path: `kingcobra run prog.kc a b` -> ["a", "b"]. */
static KcValue sys_args(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)ip; (void)a;
    sys_argc(n, 0, 0, "system.args() -> array of string", pos);
    KcValue out = kc_array_new();
    for (int i = 0; i < g_argc; i++) kc_array_push(out, kc_string(g_argv[i]));
    return out;
}

static KcValue sys_script_path(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    (void)a;
    sys_argc(n, 0, 0, "system.script_path() -> string", pos);
    return kc_string(ip->filename ? ip->filename : "");
}

const SysEntry sys_core_entries[] = {
    {"exec", sys_exec}, {"getenv", sys_getenv}, {"setenv", sys_setenv},
    {"getenv_or", sys_getenv_or}, {"has_env", sys_has_env}, {"unsetenv", sys_unsetenv},
    {"env_list", sys_env_list}, {"expand_env", sys_expand_env},
    {"platform", sys_platform}, {"cwd", sys_cwd}, {"exit", sys_exit}, {"pid", sys_pid},
    {"args", sys_args}, {"script_path", sys_script_path},
    {"last_error", sys_last_error}, {"clear_error", sys_clear_error},
};
const int sys_core_count = (int)(sizeof sys_core_entries / sizeof sys_core_entries[0]);

/* ============================ module table ============================ */

#define SYS_MAX_ENTRIES 256
static KcModuleEntry g_entries[SYS_MAX_ENTRIES];

static int add_table(int at, const SysEntry *t, int count) {
    for (int i = 0; i < count; i++) {
        if (at >= SYS_MAX_ENTRIES) { fprintf(stderr, "kingcobra: std.system entry table overflow\n"); exit(70); }
        g_entries[at].name = (char *)t[i].name; g_entries[at].fn = t[i].fn; at++;
    }
    return at;
}

KcModule *kc_mod_system(void) {
    static KcModule m;
    static bool built = false;
    if (!built) {
        int at = 0;
        at = add_table(at, sys_core_entries, sys_core_count);
        at = add_table(at, sys_info_entries, sys_info_count);
        at = add_table(at, sys_path_entries, sys_path_count);
        at = add_table(at, sys_time_entries, sys_time_count);
        at = add_table(at, sys_proc_entries, sys_proc_count);
        m.name = "system"; m.entries = g_entries; m.count = at;
        built = true;
    }
    return &m;
}
