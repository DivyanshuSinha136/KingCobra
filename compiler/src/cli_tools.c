/* `kingcobra check` and `kingcobra test`.
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com> */
#include "kc_common.h"
#include "kc_platform.h"
#include "parser.h"
#include "interp.h"
#include "typecheck.h"
#include "lang.h"
#include "mem.h"
#include "cli_tools.h"
#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#endif
#include <time.h>
#include <errno.h>

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "kingcobra: cannot open '%s': %s\n", path, strerror(errno)); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); fprintf(stderr, "kingcobra: cannot read '%s' (is it a directory?)\n", path); return NULL; }
    char *buf = kc_malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f); buf[got] = 0;
    fclose(f);
    return buf;
}

static bool ends_with(const char *s, const char *suf) { size_t a = strlen(s), b = strlen(suf); return a >= b && strcmp(s + a - b, suf) == 0; }
static bool is_test_file(const char *n) { return ends_with(n, ".kc") && (strncmp(n, "test_", 5) == 0 || ends_with(n, "_test.kc")); }
static int by_name(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

typedef struct { char **v; int n, cap; } PathList;
static void plist_add(PathList *l, char *p) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 16; l->v = kc_realloc(l->v, sizeof(char *) * (size_t)l->cap); }
    l->v[l->n++] = p;
}
static char *join_path(const char *dir, const char *name) {
    bool dot = strcmp(dir, ".") == 0;
    char *p = kc_malloc(strlen(dir) + strlen(name) + 2);
    sprintf(p, "%s%s%s", dot ? "" : dir, dot ? "" : "/", name);
    return p;
}

/* Expands a command-line path: a directory contributes every .kc file below it (only test files when
 * tests_only), sorted; anything else (including a path that does not exist yet) is passed through so the
 * caller reports it. Hidden directories are skipped. */
static void collect_path(const char *path, bool tests_only, bool recurse, PathList *out, int depth) {
#ifndef _WIN32
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (depth > 12) return;
        DIR *d = opendir(path);
        if (!d) return;
        PathList sub = {0};
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            plist_add(&sub, join_path(path, e->d_name));
        }
        closedir(d);
        qsort(sub.v, (size_t)sub.n, sizeof(char *), by_name);
        for (int i = 0; i < sub.n; i++) {
            struct stat s2;
            if (stat(sub.v[i], &s2) != 0) continue;
            const char *base = strrchr(sub.v[i], '/'); base = base ? base + 1 : sub.v[i];
            if (S_ISDIR(s2.st_mode)) { if (recurse) collect_path(sub.v[i], tests_only, recurse, out, depth + 1); }
            else if (tests_only ? is_test_file(base) : ends_with(base, ".kc")) plist_add(out, sub.v[i]);
        }
        return;
    }
#else
    (void)tests_only; (void)depth; (void)recurse;
#endif
    plist_add(out, kc_strdup(path));
}

static void json_str(const char *s) {
    putchar('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { putchar('\\'); putchar(c); }
        else if (c == '\n') fputs("\\n", stdout);
        else if (c == '\t') fputs("\\t", stdout);
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

/* kingcobra check [--quiet] [--format=json] FILE|DIR...   parse + type-check only.
 * Every file is reported; exit 0 only if all are clean. JSON mode prints one object per diagnostic
 * ({"file","line","col","severity","message"}) on stdout, for editors and CI. */
int kc_cmd_check(int nfiles, char **files, const KcCheckOpts *o) {
    if (nfiles == 0) { fprintf(stderr, "kingcobra: 'check' needs at least one file or directory\n"); return 64; }
    PathList list = {0};
    for (int i = 0; i < nfiles; i++) collect_path(files[i], false, true, &list, 0);
    if (list.n == 0) { fprintf(stderr, "kingcobra: no .kc files found\n"); return 66; }
    int bad = 0;
    for (int i = 0; i < list.n; i++) {
        char *src = slurp(list.v[i]);
        if (!src) { bad++; continue; }
        KcErrCatch c; c.quiet = o->json;                /* text: errors print themselves; json: we print them */
        if (KC_TRY(&c) == 0) {
            KcNode *prog = kc_parse_program(src, list.v[i]);
            if (o->typecheck) kc_typecheck_program(prog, list.v[i]);
            kc_try_end(&c);
            if (!o->quiet && !o->json) printf("ok   %s\n", list.v[i]);
        } else {
            kc_try_end(&c); bad++;
            if (o->json) {
                fputs("{\"file\":", stdout); json_str(c.pos.file ? c.pos.file : list.v[i]);
                printf(",\"line\":%d,\"col\":%d,\"severity\":\"error\",\"message\":", c.pos.line, c.pos.col);
                json_str(c.msg); fputs("}\n", stdout);
            }
        }
        free(src);
    }
    if (bad && !o->json) fprintf(stderr, "%d of %d file(s) have errors\n", bad, list.n);
    else if (!bad && !o->quiet && !o->json && list.n > 1) printf("%d file(s) checked, all ok\n", list.n);
    return bad ? 65 : 0;
}

typedef struct { int passed, failed, file_errors; char **fails; int nfails, capfails; } Totals;

static void note_failure(Totals *t, const char *path, const char *name) {
    if (t->nfails == t->capfails) { t->capfails = t->capfails ? t->capfails * 2 : 16; t->fails = kc_realloc(t->fails, sizeof(char *) * (size_t)t->capfails); }
    char *s = kc_malloc(strlen(path) + strlen(name) + 3);
    sprintf(s, "%s::%s", path, name);
    t->fails[t->nfails++] = s;
}

static void run_file(const char *path, const KcTestOpts *o, Totals *t) {
    char *src = slurp(path);
    if (!src) { t->file_errors++; return; }
    KcInterp *ip = NULL;
    KcErrCatch c; c.quiet = false;
    if (KC_TRY(&c) == 0) {
        KcNode *prog = kc_parse_program(src, path);
        if (o->typecheck) kc_typecheck_program(prog, path);
        ip = kc_interp_new(path);
        kc_register_builtins(ip);
        kc_interp_run_nomain(ip, prog);                  /* defines functions; does not call main() */
        kc_try_end(&c);
    } else { kc_try_end(&c); t->file_errors++; return; }

    if (!o->quiet && !o->list) printf("%s\n", path);
    int ran = 0;
    /* every global function whose name starts with test_, in definition order */
    for (int i = 0; i < ip->globals->count; i++) {
        KcEnvEntry *en = &ip->globals->entries[i];
        if (en->value.type != VAL_CLOSURE || strncmp(en->name, "test_", 5) != 0) continue;
        if (o->filter && !strstr(en->name, o->filter)) continue;
        ran++;
        if (o->list) { printf("%s::%s\n", path, en->name); continue; }
        if (o->fail_fast && t->failed) break;
        KcValue err = kc_null();
        fflush(stdout);
        if (kc_interp_try_call(ip, en->value, &err)) { if (!o->quiet) printf("  ok   %s\n", en->name); t->passed++; }
        else {
            char *msg = kc_thrown_text(err);
            if (o->quiet) printf("%s\n", path);
            printf("  FAIL %s\n       %s", en->name, msg);
            if (err.type == VAL_STRUCT && kc_is_object(err)) {
                KcValue *ln = kc_object_get(err, "line", 4), *fl = kc_object_get(err, "file", 4);
                if (ln && fl && ln->type == VAL_INT && fl->type == VAL_STRING && ln->as.i > 0) printf("  (%s:%lld)", fl->as.s->data, (long long)ln->as.i);
            } else if (ip->thrown_pos.file && ip->thrown_pos.line > 0) {       /* `throw "text"`: a plain value has no position of its own */
                printf("  (%s:%d)", ip->thrown_pos.file, (int)ip->thrown_pos.line);
            }
            printf("\n");
            free(msg);
            t->failed++;
            note_failure(t, path, en->name);
        }
    }
    if (ran == 0 && !o->quiet && !o->list) printf("  (no test_* functions%s)\n", o->filter ? " matching the filter" : "");
}

/* kingcobra test [--filter text] [--list] [--fail-fast] [--quiet] [FILE|DIR...]
 * (no paths: every test_*.kc / *_test.kc in . and tests/) */
int kc_cmd_test(int nfiles, char **files, const KcTestOpts *o) {
    PathList list = {0};
    if (nfiles == 0) {
        PathList a = {0};
        collect_path(".", true, false, &a, 0);           /* this directory's own test files … */
        collect_path("tests", true, false, &a, 0);       /* … and those in tests/ (not recursive) */
        for (int i = 0; i < a.n; i++) plist_add(&list, a.v[i]);
        qsort(list.v, (size_t)list.n, sizeof(char *), by_name);
    } else {
        for (int i = 0; i < nfiles; i++) collect_path(files[i], true, true, &list, 0);
    }
    if (list.n == 0) { fprintf(stderr, "kingcobra: no test files found (name them test_*.kc or *_test.kc, or pass files)\n"); return 64; }
    Totals t; memset(&t, 0, sizeof t);
    double t0 = kc_monotonic_seconds();
    for (int i = 0; i < list.n; i++) { if (o->fail_fast && t.failed) break; run_file(list.v[i], o, &t); }
    if (o->list) return 0;
    if (t.nfails) {
        printf("\nFailures:\n");
        for (int i = 0; i < t.nfails; i++) printf("  %s\n", t.fails[i]);
    }
    printf("\n%d passed, %d failed", t.passed, t.failed);
    if (t.file_errors) printf(", %d file(s) could not be loaded", t.file_errors);
    printf("  (%.0f ms)\n", (kc_monotonic_seconds() - t0) * 1000.0);
    return (t.failed || t.file_errors) ? 1 : 0;
}
