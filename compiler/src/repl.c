/* KingCobra REPL — read-eval-print loop.
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 * License: MIT
 *
 * Design notes (full write-up in docs/REPL.md):
 *   - One persistent KcInterp; every input is parsed, type-checked and run
 *     against its global scope, so `let`, `fn`, `struct` and `import`
 *     declarations accumulate across inputs.
 *   - Errors are recoverable: kc_error_at longjmps back here (see
 *     KcErrCatch in kc_common.h) instead of exiting.
 *   - Multi-line input: brackets / strings / block comments left open
 *     continue onto the next line with a `... ` prompt; so do `try` without
 *     its `catch`, a trailing operator, and an `if` (a following `else` on the
 *     next line is joined; any other line, or a blank one, ends the statement).
 *   - The C preprocessor (#define / #include / #if ...) runs on every input and
 *     its macros persist for the session (:macros lists them).
 *   - Values echo pretty-printed (wrapped at :set width, coloured on a terminal).
 *   - Every input is registered as "<repl:N>", so errors show the source line
 *     and caret even for functions defined several inputs ago.
 *   - A trailing `;` is optional (`1 + 2` works).
 *   - Parsed ASTs and their source text are never freed: closures keep
 *     pointers into them for the life of the session.
 *   - Ctrl-C interrupts the running statement (POSIX); Ctrl-D exits.
 *   - Line editing + history + Tab completion when built with readline
 *     (-DKC_READLINE, auto-detected by the Makefile); plain stdin otherwise.
 */
#include "kc_common.h"
#include "kc_platform.h"
#include "lexer.h"
#include "parser.h"
#include "interp.h"
#include "typecheck.h"
#include "repl.h"
#include "mem.h"
#include "lib.h"

#include <ctype.h>
#include <errno.h>

#if defined(_WIN32)
#  include <io.h>
#else
#  include <unistd.h>
#  include <signal.h>
#  include <sys/resource.h>
#  define KC_HAVE_SIGINT 1
#endif

#ifdef KC_READLINE
#  include <readline/readline.h>
#  include <readline/history.h>
#endif

bool kc_stdin_is_tty(void) { return KC_ISATTY(KC_FILENO(stdin)) != 0; }

typedef struct { char *d; size_t n, cap; } Sb;

/* ------------------------------------------------------------------ state */

static KcInterp *g_ip;
static bool g_typecheck = true;
static Sb   buf;                    /* pending (possibly multi-line) input; static: survives siglongjmp */
static bool g_tty = false;
static bool g_quit = false;
static int  g_exit_code = 0;
static KcPP *g_pp;                  /* macros persist across inputs */
static bool g_color = false;
static int  g_width = 80;
static bool g_timing = false;
static int  g_seq = 0;              /* inputs seen: names them "<repl:N>" */
typedef struct { char *name, *text; } ReplSrc;
static ReplSrc *g_srcs; static int g_nsrcs, g_capsrcs;
static char **g_hist; static int g_nhist, g_caphist;     /* inputs that ran successfully (:history, :save, :redo) */
static char *g_pending;             /* a line read ahead (looking for `else`) */
static char g_last_load[1024];
static char g_indent[96];

#ifdef KC_HAVE_SIGINT
static sigjmp_buf g_jb;
static volatile sig_atomic_t g_jb_ready = 0;
static volatile sig_atomic_t g_in_eval = 0;
static volatile sig_atomic_t g_segv_hit = 0;
static void on_sigint(int sig) { (void)sig; if (g_jb_ready) siglongjmp(g_jb, 1); }

/* Machine-code (JIT) self-recursion has no depth counter, so runaway recursion
 * in a hot int function ends in a stack-overflow SIGSEGV. If the fault address
 * lies at the edge of the stack while evaluating, unwind to the prompt as a
 * normal error; any other fault keeps the default (crash) behaviour. */
static char *g_stack_hi = NULL;
static size_t g_stack_lim = 0;
static char g_altstack[64 * 1024];
static void on_segv(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    char *a = (char *)si->si_addr;
    char *lo = g_stack_hi - g_stack_lim;
    if (g_in_eval && g_jb_ready && g_stack_hi && a < lo + (1 << 20) && a > lo - (1 << 20)) {
        g_segv_hit = 1;
        siglongjmp(g_jb, 1);
    }
    signal(sig, SIG_DFL);   /* unrelated fault: re-raise with default action */
}
#else
static volatile int g_in_eval = 0;
#endif

/* ------------------------------------------------------------- text buffer */

static void sb_init(Sb *s) { s->d = kc_malloc(64); s->cap = 64; s->n = 0; s->d[0] = 0; }
static void sb_reserve(Sb *s, size_t extra) {
    if (s->n + extra + 1 > s->cap) {
        while (s->n + extra + 1 > s->cap) s->cap *= 2;
        s->d = kc_realloc(s->d, s->cap);
    }
}
static void sb_add(Sb *s, const char *t, size_t len) { sb_reserve(s, len); memcpy(s->d + s->n, t, len); s->n += len; s->d[s->n] = 0; }
static void sb_puts(Sb *s, const char *t) { sb_add(s, t, strlen(t)); }
static void sb_clear(Sb *s) { s->n = 0; s->d[0] = 0; }

/* -------------------------------------------------------- value echo (repr) */

static void col_put(Sb *o, bool color, const char *code, const char *text, size_t n) {
    if (color) sb_puts(o, code);
    sb_add(o, text, n);
    if (color) sb_puts(o, "\x1b[0m");
}
#define C_NUM "\x1b[33m"
#define C_STR "\x1b[32m"
#define C_KW  "\x1b[35m"
#define C_FN  "\x1b[36m"
#define C_DIM "\x1b[2m"

static void repr_into(Sb *o, KcValue v, int depth, bool color) {
    if (kc_value_freed(v)) {            /* a tombstone left by free(): say so instead of showing it as empty */
        char b[48]; snprintf(b, sizeof b, "<freed %s>", kc_type_name(v));
        col_put(o, color, C_DIM, b, strlen(b));
        return;
    }
    switch (v.type) {
        case VAL_STRING: {
            Sb t; sb_init(&t);
            sb_puts(&t, "\"");
            for (int i = 0; i < v.as.s->len; i++) {
                unsigned char c = (unsigned char)v.as.s->data[i];
                switch (c) {
                    case '\n': sb_puts(&t, "\\n"); break;
                    case '\t': sb_puts(&t, "\\t"); break;
                    case '\r': sb_puts(&t, "\\r"); break;
                    case '"':  sb_puts(&t, "\\\""); break;
                    case '\\': sb_puts(&t, "\\\\"); break;
                    default:
                        if (c < 0x20 || c == 0x7f) { char b[8]; snprintf(b, sizeof b, "\\x%02x", c); sb_puts(&t, b); }
                        else sb_add(&t, (const char *)&c, 1);
                }
            }
            sb_puts(&t, "\"");
            col_put(o, color, C_STR, t.d, t.n);
            free(t.d);
            return;
        }
        case VAL_ARRAY: {
            if (depth >= 4) { sb_puts(o, "[...]"); return; }
            sb_puts(o, "[");
            int n = v.as.arr->count, shown = n > 64 ? 64 : n;
            for (int i = 0; i < shown; i++) {
                if (i) sb_puts(o, ", ");
                repr_into(o, v.as.arr->items[i], depth + 1, color);
            }
            if (n > shown) { char b[40]; snprintf(b, sizeof b, ", ... %d more", n - shown); sb_puts(o, b); }
            sb_puts(o, "]");
            return;
        }
        case VAL_STRUCT: {
            KcStructInst *st = v.as.strct;
            bool obj = kc_is_object(v);
            if (depth >= 4) { sb_puts(o, obj ? "{...}" : "{ ... }"); return; }
            if (!obj) { sb_puts(o, st->type_name ? st->type_name : "struct"); sb_puts(o, " "); }
            if (st->count == 0) { sb_puts(o, "{}"); return; }
            sb_puts(o, "{ ");
            int shown = st->count > 32 ? 32 : st->count;
            for (int i = 0; i < shown; i++) {
                if (i) sb_puts(o, ", ");
                sb_puts(o, st->names[i]); sb_puts(o, ": ");
                repr_into(o, st->values[i], depth + 1, color);
            }
            if (st->count > shown) sb_puts(o, ", ...");
            sb_puts(o, " }");
            return;
        }
        case VAL_INT: case VAL_FLOAT: { char *s = kc_to_display_string(v); col_put(o, color, C_NUM, s, strlen(s)); free(s); return; }
        case VAL_BOOL: case VAL_NULL: { char *s = kc_to_display_string(v); col_put(o, color, C_KW, s, strlen(s)); free(s); return; }
        case VAL_CLOSURE: case VAL_NATIVE: case VAL_EXTERN: { char *s = kc_to_display_string(v); col_put(o, color, C_FN, s, strlen(s)); free(s); return; }
        default: {
            char *s = kc_to_display_string(v);
            sb_puts(o, s);
            free(s);
        }
    }
}

static void spaces(Sb *o, int n) { while (n-- > 0) sb_puts(o, " "); }

/* One line when it fits in the width, else one element/field per line, nested with 2-space steps. */
static void pretty_into(Sb *o, KcValue v, int indent, int depth) {
    bool container = !kc_value_freed(v) && (v.type == VAL_ARRAY || v.type == VAL_STRUCT) && depth < 4;
    Sb flat; sb_init(&flat);
    if (container) repr_into(&flat, v, depth, false);
    if (!container || (int)flat.n + indent <= g_width) { free(flat.d); repr_into(o, v, depth, g_color); return; }
    free(flat.d);
    if (v.type == VAL_ARRAY) {
        int n = v.as.arr->count, shown = n > 64 ? 64 : n;
        sb_puts(o, "[\n");
        for (int i = 0; i < shown; i++) {
            spaces(o, indent + 2);
            pretty_into(o, v.as.arr->items[i], indent + 2, depth + 1);
            sb_puts(o, i + 1 < shown || n > shown ? ",\n" : "\n");
        }
        if (n > shown) { spaces(o, indent + 2); char b[40]; snprintf(b, sizeof b, "... %d more\n", n - shown); sb_puts(o, b); }
        spaces(o, indent); sb_puts(o, "]");
        return;
    }
    KcStructInst *st = v.as.strct;
    if (!kc_is_object(v)) { sb_puts(o, st->type_name ? st->type_name : "struct"); sb_puts(o, " "); }
    sb_puts(o, "{\n");
    int shown = st->count > 32 ? 32 : st->count;
    for (int i = 0; i < shown; i++) {
        spaces(o, indent + 2);
        sb_puts(o, st->names[i]); sb_puts(o, ": ");
        pretty_into(o, st->values[i], indent + 2, depth + 1);
        sb_puts(o, i + 1 < shown || st->count > shown ? ",\n" : "\n");
    }
    if (st->count > shown) { spaces(o, indent + 2); sb_puts(o, "...\n"); }
    spaces(o, indent); sb_puts(o, "}");
}

/* ------------------------------------------------------------ diagnostics */

static void print_error(const KcErrCatch *c) {
    fflush(stdout);
    bool col = kc_stderr_color();
    fprintf(stderr, "%s%s:%d:%d: %serror:%s %s\n", col ? "\x1b[1m" : "", c->pos.file ? c->pos.file : "<repl>", c->pos.line, c->pos.col,
            col ? "\x1b[31m" : "", col ? "\x1b[0m" : "", c->msg);
    kc_print_excerpt(c->pos);
}

/* Every input gets its own pseudo-file name so an error in a function defined many inputs ago still shows ITS line. */
static const char *register_input(const char *text) {
    char nm[32]; snprintf(nm, sizeof nm, "<repl:%d>", ++g_seq);
    if (g_nsrcs == g_capsrcs) { g_capsrcs = g_capsrcs ? g_capsrcs * 2 : 64; g_srcs = kc_realloc(g_srcs, sizeof(ReplSrc) * (size_t)g_capsrcs); }
    g_srcs[g_nsrcs].name = kc_strdup(nm); g_srcs[g_nsrcs].text = kc_strdup(text);
    return g_srcs[g_nsrcs++].name;
}
static bool repl_source_line(const char *file, int line, char *buf, size_t cap) {
    for (int i = g_nsrcs - 1; i >= 0; i--) {
        if (strcmp(g_srcs[i].name, file) != 0) continue;
        const char *p = g_srcs[i].text;
        for (int ln = 1; ln < line; ln++) { p = strchr(p, '\n'); if (!p) return false; p++; }
        const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n >= cap) n = cap - 1;
        memcpy(buf, p, n); buf[n] = 0;
        return true;
    }
    return false;
}

/* ------------------------------------------------------- input completeness */

/* True when `s` leaves a string, block comment or bracket open, or ends in a
 * line-continuation backslash: the statement obviously continues. */
static int bracket_depth(const char *s, bool *open_str) {
    int depth = 0;
    const char *p = s;
    *open_str = false;
    while (*p) {
        if (p[0] == '/' && p[1] == '/') { while (*p && *p != '\n') p++; continue; }
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) p++;
            if (!*p) { *open_str = true; return depth; }
            p += 2; continue;
        }
        if (*p == '"') {
            p++;
            while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
            if (!*p) { *open_str = true; return depth; }
            p++; continue;
        }
        if (*p == '(' || *p == '[' || *p == '{') depth++;
        else if (*p == ')' || *p == ']' || *p == '}') depth--;
        p++;
    }
    return depth;
}
static bool input_incomplete(const char *s) {
    bool open_str;
    int d = bracket_depth(s, &open_str);
    if (open_str || d > 0) return true;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    return n && s[n - 1] == '\\';                       /* #define continuation */
}

static KcTokKind last_token_kind(const char *s) {
    KcLexer lx; kc_lexer_init(&lx, s, "<repl>");
    KcTokKind last = TK_EOF;
    for (;;) {
        KcToken t = kc_lexer_next(&lx);
        if (t.kind == TK_EOF) break;
        if (t.kind == TK_STRING) free(t.str);
        last = t.kind;
    }
    return last;
}

static char *dup_with_suffix(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    char *r = kc_malloc(a + b + 1);
    memcpy(r, s, a); memcpy(r + a, suffix, b + 1);
    return r;
}

typedef struct { char msg[600]; KcPos pos; } PErr;

static KcNode *try_parse(const char *src, const char *name, KcErrCatch *c) {
    c->quiet = true;
    if (KC_TRY(c) == 0) {
        KcNode *p = kc_parse_program(src, name);
        kc_try_end(c);
        return p;
    }
    kc_try_end(c);
    return NULL;
}

/* Parse one REPL input, supplying the optional trailing ';'. The returned program keeps pointers into the
 * text it was parsed from, so that text is deliberately leaked. On failure fills *err and returns NULL. */
static KcNode *parse_input_ex(const char *text, const char *name, PErr *err) {
    KcErrCatch c; c.quiet = true;
    const char *volatile pre = NULL;
    if (KC_TRY(&c) == 0) { pre = kc_preprocess(text, name, g_pp); kc_try_end(&c); }
    else {
        kc_try_end(&c);
        snprintf(err->msg, sizeof err->msg, "%s", c.msg); err->pos = c.pos;
        return NULL;
    }
    KcTokKind volatile last = TK_EOF;
    if (KC_TRY(&c) == 0) { last = last_token_kind(pre); kc_try_end(&c); }
    else kc_try_end(&c);                                /* a lexer error: let the parser report it below */
    KcNode *prog = NULL;
    switch (last) {                                     /* ends in an operator / separator: more input is coming */
        case TK_PLUS: case TK_MINUS: case TK_STAR: case TK_SLASH: case TK_PERCENT: case TK_AND: case TK_OR:
        case TK_EQ: case TK_NEQ: case TK_LT: case TK_LE: case TK_GT: case TK_GE: case TK_ASSIGN: case TK_COMMA:
        case TK_DOT: case TK_ARROW: case TK_QUESTION: case TK_QQ: case TK_AMP: case TK_PIPE: case TK_CARET:
        case TK_SHL: case TK_SHR: case TK_USHR: case TK_PLUSEQ: case TK_MINUSEQ: case TK_STAREQ: case TK_SLASHEQ:
            snprintf(err->msg, sizeof err->msg, "unexpected end of input: an expression is incomplete");
            err->pos = (KcPos){ name, 1, 1 };
            return NULL;
        default: break;
    }
    if (last == TK_SEMI || last == TK_RBRACE || last == TK_EOF) {
        prog = try_parse(dup_with_suffix(pre, "\n"), name, &c);
        if (prog) return prog;
    }
    if (last != TK_SEMI && last != TK_EOF) {
        prog = try_parse(dup_with_suffix(pre, ";\n"), name, &c);
        if (prog) return prog;
    }
    snprintf(err->msg, sizeof err->msg, "%s", c.msg); err->pos = c.pos;
    return NULL;
}
static void print_perr(const PErr *e) {
    KcErrCatch c; snprintf(c.msg, sizeof c.msg, "%s", e->msg); c.pos = e->pos;
    print_error(&c);
}
/* An error that only means "more input is coming". */
static bool continuation_error(const PErr *e) {
    return strstr(e->msg, "needs a 'catch'") || strstr(e->msg, "after the body of a do-loop") ||
           strstr(e->msg, "unexpected end of input");
}
/* `if (...) {...}` (or an if/else-if chain) with no final else: a following `else` may still come. */
static bool may_take_else(KcNode *prog) {
    if (!prog->as.program.stmts.count) return false;
    KcNode *last = prog->as.program.stmts.items[prog->as.program.stmts.count - 1];
    if (last->kind != N_IF) return false;
    while (last->kind == N_IF) {
        if (!last->as.ifstmt.else_branch) return true;
        last = last->as.ifstmt.else_branch;
    }
    return false;
}
static bool starts_with_else(const char *l) {
    while (*l == ' ' || *l == '\t') l++;
    return !strncmp(l, "else", 4) && !(isalnum((unsigned char)l[4]) || l[4] == '_');
}

/* ---------------------------------------------------------------- execution */

static void set_underscore(KcValue v) {
    if (!kc_env_set(g_ip->globals, "_", v)) kc_env_define(g_ip->globals, "_", v, true);
}

static void echo_value(KcValue v) {
    /* Evaluating a freed value is the same mistake as print(v): report it (recoverably). */
    if (kc_value_freed(v)) kc_error_freed(v, (KcPos){ "<repl>", 1, 1 });
    Sb o; sb_init(&o);
    pretty_into(&o, v, 0, 0);
    fputs(o.d, stdout); fputc('\n', stdout);
    free(o.d);
}

/* Type-check (optionally) then run `prog`. If `echo`, print expression values.
 * Returns true on success. Any error is reported and the session continues. */
static bool run_program(KcNode *prog, bool echo) {
    KcErrCatch c; c.quiet = false;

    if (g_typecheck) {
        void *volatile snap = kc_typecheck_snapshot();
        if (KC_TRY(&c) == 0) {
            kc_typecheck_repl(prog, "<repl>");
            kc_try_end(&c);
            kc_typecheck_discard(snap);
        } else {
            kc_try_end(&c);
            kc_typecheck_restore(snap);
            return false;
        }
    }

    bool volatile ok = true;
    double volatile t0 = g_timing ? kc_monotonic_seconds() : 0;
    if (KC_TRY(&c) == 0) {
        g_in_eval = 1;
        for (int i = 0; i < prog->as.program.stmts.count; i++) {
            KcValue v;
            if (kc_interp_exec_repl(g_ip, prog->as.program.stmts.items[i], &v) && v.type != VAL_NULL) {
                set_underscore(v);
                if (echo) echo_value(v);
            }
        }
        g_in_eval = 0;
        kc_try_end(&c);
    } else {
        g_in_eval = 0;
        kc_try_end(&c);
        kc_interp_reset_state(g_ip);
        ok = false;
    }
    fflush(stdout);
    if (g_timing && ok) {
        double dt = kc_monotonic_seconds() - t0;
        if (dt < 1e-3) printf("%s(%.1f us)%s\n", g_color ? C_DIM : "", dt * 1e6, g_color ? "\x1b[0m" : "");
        else if (dt < 1.0) printf("%s(%.2f ms)%s\n", g_color ? C_DIM : "", dt * 1e3, g_color ? "\x1b[0m" : "");
        else printf("%s(%.3f s)%s\n", g_color ? C_DIM : "", dt, g_color ? "\x1b[0m" : "");
    }
    return ok;
}

static char *read_whole_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = kc_malloc((size_t)size + 1);
    size_t got = fread(buf, 1, (size_t)size, f);
    buf[got] = 0;
    fclose(f);
    return buf;
}

/* Load a .kc file into the session: its top-level statements run against the
 * persistent globals; main() is NOT auto-called. */
static bool load_file(const char *path) {
    char *src = read_whole_file(path);
    if (!src) { fprintf(stderr, "kingcobra: cannot open '%s'\n", path); return false; }
    KcErrCatch c; c.quiet = true;
    KcNode *volatile prog = NULL;
    snprintf(g_last_load, sizeof g_last_load, "%s", path);
    if (KC_TRY(&c) == 0) { prog = kc_parse_program(src, path); kc_try_end(&c); }
    else { kc_try_end(&c); print_error(&c); return false; }
    return run_program(prog, false);
}

/* --------------------------------------------------------------- commands */

static void print_help(void) {
    puts("KingCobra REPL");
    puts("  Type any statement or expression; a trailing ';' is optional.");
    puts("  Unclosed (, [, {, or \"...\", `try` without `catch`, and `if` followed by `else` continue");
    puts("  on the next line (for an `if`, a blank line or any other line ends it).");
    puts("  The value of the last expression is available as `_`.");
    puts("  C preprocessor lines work here: #define N 10, #define SQ(x) ((x)*(x)), #undef, #include, #if ...");
    puts("");
    puts("Commands:");
    puts("  :help, :h, :?           show this help      (:help <name> = the language help for <name>)");
    puts("  help  /  help()  /  ?name   the language help (Python-style): help(print), help(\"switch\"), help(\"topics\")");
    puts("  :quit, :q, :exit        leave (also Ctrl-D, `exit`, `quit`)");
    puts("  :load <file.kc>         run a file into this session (main() is not called)");
    puts("  :reload                 run the last :load again (pick up edits)");
    puts("  :reset                  discard all definitions and start fresh");
    puts("  :vars                   list everything defined this session");
    puts("  :info <name>            signature of a function, fields of a struct, or a value's type");
    puts("  :type <expr>            show the type of an expression's value");
    puts("  :del <name>             forget a variable or function");
    puts("  :macros                 list preprocessor macros");
    puts("  :time <code>            run code and report how long it took");
    puts("  :timing [on|off]        report the time of every input");
    puts("  :history [n]            the last n inputs that ran (numbered)");
    puts("  :redo [n]               run input number n again (default: the last)");
    puts("  :save <file>            write the session's inputs to a .kc file");
    puts("  :paste                  paste several lines; finish with :end (or Ctrl-D)");
    puts("  :set width <n>          wrap long values at n columns (default 80)");
    puts("  :color [on|off]         colour the echoed values");
    puts("  :mem                    live allocations (see also: free, defer, --leaks)");
    puts("  :clear                  clear the screen");
    puts("  :typecheck [on|off]     show / toggle the static type checker");
    puts("");
    puts("Tab completes keywords, names, :commands, file names after :load/:save, and members after a dot");
    puts("(module functions, struct fields, string / array / object methods).");
    puts("Startup: ~/.kingcobrarc (or $KC_REPL_INIT) is loaded when the REPL starts on a terminal.");
    puts("Ctrl-C interrupts a running statement; at the prompt it discards the current input.");
}

static const char *kind_label(KcValue v) {
    switch (v.type) {
        case VAL_CLOSURE: return "fn";
        case VAL_NATIVE: case VAL_EXTERN: return "native fn";
        case VAL_MODULE: return "module";
        case VAL_NATIVE_LIB: return "native lib";
        default: return kc_type_name(v);
    }
}

static void cmd_vars(void) {
    KcEnv *g = g_ip->globals;
    if (g->count == 0 && g_ip->struct_types_count == 0) { puts("(nothing defined yet)"); return; }
    /* later entries shadow earlier ones: show only the newest binding per name */
    for (int i = 0; i < g->count; i++) {
        bool shadowed = false;
        for (int j = i + 1; j < g->count; j++) if (g->entries[j].name == g->entries[i].name) { shadowed = true; break; }
        if (shadowed) continue;
        KcValue v = g->entries[i].value;
        Sb o; sb_init(&o);
        if (v.type == VAL_CLOSURE) {
            KcNode *f = v.as.closure->fnnode;
            sb_puts(&o, "(");
            for (int k = 0; k < f->as.fn.params.count; k++) {
                if (k) sb_puts(&o, ", ");
                sb_puts(&o, f->as.fn.params.items[k]->as.ident.name);
            }
            sb_puts(&o, ")");
        } else if (v.type == VAL_MODULE) {
            sb_puts(&o, v.as.module->name);
        } else if (v.type == VAL_NATIVE || v.type == VAL_EXTERN || v.type == VAL_NATIVE_LIB) {
            /* nothing useful to show */
        } else {
            repr_into(&o, v, 0, false);
            if (o.n > 70) { o.d[67] = 0; o.n = 67; sb_puts(&o, "..."); }
        }
        printf("  %-16s %-10s %s\n", g->entries[i].name, kind_label(v), o.d);
        free(o.d);
    }
    for (int i = 0; i < g_ip->struct_types_count; i++) {
        KcStructTypeDef *d = &g_ip->struct_types[i];
        printf("  %-16s %-10s {", d->name, "struct");
        for (int k = 0; k < d->field_count; k++) printf("%s%s", k ? ", " : " ", d->field_names[k]);
        printf(" }\n");
    }
}

static void reset_session(void) {
    g_ip = kc_interp_new("<repl>");
    kc_register_builtins(g_ip);
    kc_typecheck_reset();
    kc_parse_keep_aliases(true);
    kc_parse_reset_aliases();
    g_pp = kc_pp_new();
    kc_source_line_hook = repl_source_line;
}

static void cmd_type(const char *expr) {
    if (!*expr) { puts("usage: :type <expression>"); return; }
    PErr pe;
    KcNode *prog = parse_input_ex(expr, register_input(expr), &pe);
    if (!prog) { print_perr(&pe); return; }
    if (prog->as.program.stmts.count != 1 || prog->as.program.stmts.items[0]->kind != N_EXPRSTMT) {
        puts(":type needs a single expression"); return;
    }
    KcErrCatch c; c.quiet = false;
    KcValue v = kc_null();
    if (KC_TRY(&c) == 0) {
        g_in_eval = 1;
        KcValue r;
        if (kc_interp_exec_repl(g_ip, prog->as.program.stmts.items[0], &r)) v = r;
        g_in_eval = 0;
        kc_try_end(&c);
    } else {
        g_in_eval = 0; kc_try_end(&c); kc_interp_reset_state(g_ip); return;
    }
    if (v.type == VAL_STRUCT && v.as.strct->type_name && !kc_is_object(v)) puts(v.as.strct->type_name);
    else puts(kc_type_name(v));
}

static void hist_add(const char *text) {
    if (g_nhist == g_caphist) { g_caphist = g_caphist ? g_caphist * 2 : 64; g_hist = kc_realloc(g_hist, sizeof(char *) * (size_t)g_caphist); }
    g_hist[g_nhist++] = kc_strdup(text);
}

static char *read_line(const char *prompt);

/* Register, parse and run one chunk of text exactly as if it had been typed. */
static bool run_text(const char *text, bool log) {
    const char *name = register_input(text);
    PErr pe;
    KcNode *prog = parse_input_ex(text, name, &pe);
    if (!prog) { print_perr(&pe); return false; }
    if (!prog->as.program.stmts.count) return true;
    bool ok = run_program(prog, true);
    if (ok && log) hist_add(text);
    return ok;
}

static void cmd_info(const char *name) {
    if (!*name) { puts("usage: :info <name>"); return; }
    KcValue v;
    if (kc_env_get(g_ip->globals, name, &v)) {
        if (v.type == VAL_CLOSURE) {
            KcNode *f = v.as.closure->fnnode;
            printf("fn %s(", name);
            for (int k = 0; k < f->as.fn.params.count; k++) {
                bool dflt = FN_DEFAULTS(f) && FN_DEFAULTS(f)[k];
                const char *ty = f->as.fn.param_types ? f->as.fn.param_types[k] : NULL;
                printf("%s%s%s%s%s", k ? ", " : "", f->as.fn.params.items[k]->as.ident.name,
                       ty && strcmp(ty, "any") ? ": " : "", ty && strcmp(ty, "any") ? ty : "", dflt ? " = ..." : "");
            }
            printf(")");
            if (f->as.fn.ret_type && strcmp(f->as.fn.ret_type, "void") != 0) printf(" -> %s", f->as.fn.ret_type);
            if (f->flags & NF_HAS_STATIC) printf("   [has static locals]");
            printf("\n");
            return;
        }
        if (v.type == VAL_MODULE) {
            printf("module %s:", v.as.module->name);
            for (int i = 0; i < v.as.module->count; i++) printf(" %s", v.as.module->entries[i].name);
            printf("\n");
            return;
        }
        printf("%s : %s\n", name, kind_label(v));
        return;
    }
    KcStructTypeDef *d = kc_find_struct_type(g_ip, name);
    if (d) {
        printf("%s %s {", d->is_union ? "union" : d->packed ? "packed struct" : "struct", d->name);
        for (int k = 0; k < d->field_count; k++) printf("%s%s: %s", k ? ", " : " ", d->field_names[k], d->field_types && d->field_types[k] ? d->field_types[k] : "any");
        printf(" }\n");
        return;
    }
    printf("'%s' is not defined\n", name);
}

static void cmd_del(const char *name) {
    if (!*name) { puts("usage: :del <name>"); return; }
    int hit = 0;
    for (int i = 0; i < g_ip->globals->count; i++)
        if (!strcmp(g_ip->globals->entries[i].name, name)) { g_ip->globals->entries[i].name = kc_intern("<deleted>"); hit++; }
    if (hit) printf("%s forgotten\n", name); else printf("'%s' is not defined in this session\n", name);
}

static void cmd_macros(void) {
    int n = kc_pp_macro_count(g_pp);
    if (!n) { puts("(no macros)"); return; }
    for (int i = 0; i < n; i++) {
        char params[256];
        kc_pp_macro_params(g_pp, i, params, sizeof params);
        printf("  #define %s%s%s%s %s\n", kc_pp_macro_name(g_pp, i), kc_pp_macro_is_func(g_pp, i) ? "(" : "", params,
               kc_pp_macro_is_func(g_pp, i) ? ")" : "", kc_pp_macro_body(g_pp, i));
    }
}

static void cmd_history(const char *arg) {
    int n = *arg ? atoi(arg) : g_nhist;
    if (n <= 0 || n > g_nhist) n = g_nhist;
    for (int i = g_nhist - n; i < g_nhist; i++) {
        const char *t = g_hist[i];
        printf("%4d  ", i + 1);
        for (; *t; t++) { fputc(*t, stdout); if (*t == '\n') fputs("      ", stdout); }
        fputc('\n', stdout);
    }
    if (!g_nhist) puts("(no inputs yet)");
}

static void cmd_save(const char *path) {
    if (!*path) { puts("usage: :save <file.kc>"); return; }
    FILE *f = fopen(path, "wb");
    if (!f) { printf("cannot write '%s'\n", path); return; }
    for (int i = 0; i < g_nhist; i++) { fputs(g_hist[i], f); fputc('\n', f); }
    fclose(f);
    printf("saved %d input(s) to %s\n", g_nhist, path);
}

static void cmd_paste(void) {
    if (g_tty) puts("(paste mode: finish with :end on its own line, or Ctrl-D)");
    Sb all; sb_init(&all);
    for (;;) {
        char *l = read_line("... ");
        if (!l) break;
        if (!strcmp(l, ":end")) { free(l); break; }
        sb_puts(&all, l); sb_puts(&all, "\n"); free(l);
    }
    if (all.n) run_text(all.d, true);
}

static void onoff(const char *arg, bool *flag, const char *what) {
    if (!strcmp(arg, "on")) *flag = true;
    else if (!strcmp(arg, "off")) *flag = false;
    else if (*arg) { printf("usage: %s [on|off]\n", what); return; }
    printf("%s is %s\n", what, *flag ? "on" : "off");
}

/* returns true if `line` was a REPL command (handled) */
static bool handle_command(char *line) {
    while (*line == ' ' || *line == '\t') line++;
    char *e = line + strlen(line);
    while (e > line && isspace((unsigned char)e[-1])) *--e = 0;

    if (!strcmp(line, "exit") || !strcmp(line, "quit") || !strcmp(line, "exit()") || !strcmp(line, "quit()")) { g_quit = true; return true; }
    if (!strcmp(line, "help")) { run_text("help()", false); return true; }           /* Python-style: `help` alone */
    if (line[0] == '?') {                                                              /* ?name  ==  help("name") */
        char *w = line + 1; while (*w == ' ') w++;
        if (!*w) { run_text("help()", false); return true; }
        char code[300]; size_t k = (size_t)snprintf(code, sizeof code, "help(\"");
        for (const char *q = w; *q && k + 4 < sizeof code; q++) { if (*q == '"' || *q == '\\') code[k++] = '\\'; code[k++] = *q; }
        snprintf(code + k, sizeof code - k, "\")");
        run_text(code, false);
        return true;
    }
    if (line[0] != ':') return false;

    char *arg = line + 1;
    while (*arg && !isspace((unsigned char)*arg)) arg++;
    char *name = line + 1;
    if (*arg) { *arg++ = 0; while (*arg == ' ' || *arg == '\t') arg++; }

    if (!strcmp(name, "help") || !strcmp(name, "h") || !strcmp(name, "?")) {
        if (*arg) {                                         /* :help map  ==  help("map") */
            char code[300]; size_t k = (size_t)snprintf(code, sizeof code, "help(\"");
            for (const char *q = arg; *q && k + 4 < sizeof code; q++) { if (*q == '"' || *q == '\\') code[k++] = '\\'; code[k++] = *q; }
            snprintf(code + k, sizeof code - k, "\")");
            run_text(code, false);
        } else print_help();
    }
    else if (!strcmp(name, "quit") || !strcmp(name, "q") || !strcmp(name, "exit")) g_quit = true;
    else if (!strcmp(name, "vars") || !strcmp(name, "globals")) cmd_vars();
    else if (!strcmp(name, "reset")) { reset_session(); g_nhist = 0; puts("session reset"); }
    else if (!strcmp(name, "load") || !strcmp(name, "l")) {
        if (!*arg) puts("usage: :load <file.kc>");
        else load_file(arg);
    }
    else if (!strcmp(name, "reload")) {
        if (!g_last_load[0]) puts("nothing loaded yet (use :load <file.kc>)");
        else { char p[1024]; snprintf(p, sizeof p, "%s", g_last_load); load_file(p); }
    }
    else if (!strcmp(name, "type") || !strcmp(name, "t")) cmd_type(arg);
    else if (!strcmp(name, "info") || !strcmp(name, "doc") || !strcmp(name, "i")) cmd_info(arg);
    else if (!strcmp(name, "del") || !strcmp(name, "forget")) cmd_del(arg);
    else if (!strcmp(name, "macros") || !strcmp(name, "defines")) cmd_macros();
    else if (!strcmp(name, "history") || !strcmp(name, "hist")) cmd_history(arg);
    else if (!strcmp(name, "redo")) {
        int n = *arg ? atoi(arg) : g_nhist;
        if (n < 1 || n > g_nhist) puts("no such input (see :history)");
        else { printf("%s\n", g_hist[n - 1]); run_text(g_hist[n - 1], true); }
    }
    else if (!strcmp(name, "save")) cmd_save(arg);
    else if (!strcmp(name, "paste")) cmd_paste();
    else if (!strcmp(name, "time")) {
        if (!*arg) puts("usage: :time <code>");
        else { bool saved = g_timing; g_timing = true; run_text(arg, false); g_timing = saved; }
    }
    else if (!strcmp(name, "timing")) onoff(arg, &g_timing, "timing");
    else if (!strcmp(name, "color") || !strcmp(name, "colour")) onoff(arg, &g_color, "color");
    else if (!strcmp(name, "set")) {
        if (!strncmp(arg, "width", 5)) {
            int w = atoi(arg + 5);
            if (w >= 20 && w <= 1000) g_width = w; else if (*(arg + 5)) { puts("width must be between 20 and 1000"); return true; }
            printf("width is %d\n", g_width);
        } else if (!strncmp(arg, "color", 5)) onoff(arg + 5 + (arg[5] == ' '), &g_color, "color");
        else puts("usage: :set width <n> | :set color on|off");
    }
    else if (!strcmp(name, "mem")) run_text("mem_report()", false);
    else if (!strcmp(name, "clear") || !strcmp(name, "cls")) { fputs("\x1b[2J\x1b[H", stdout); fflush(stdout); }
    else if (!strcmp(name, "typecheck")) {
        if (!strcmp(arg, "on")) g_typecheck = true;
        else if (!strcmp(arg, "off")) g_typecheck = false;
        else if (*arg) { puts("usage: :typecheck [on|off]"); return true; }
        printf("type checker is %s\n", g_typecheck ? "on" : "off");
    }
    else printf("unknown command ':%s' (try :help)\n", name);
    return true;
}

/* ------------------------------------------------------- line input / editing */

#ifdef KC_READLINE
static const char *KEYWORDS_[] = {
    "fn", "let", "var", "if", "else", "while", "for", "return", "struct", "import", "extern",
    "unsafe", "comptime", "break", "continue", "true", "false", "null", "match", "from", "as", "in",
    "do", "switch", "case", "default", "goto", "static", "auto", "extrn", "typedef", "union",
    "enum", "impl", "type", "try", "catch", "throw", "defer", "const", "sizeof", "alignof", "offsetof", NULL
};
static const char *COMMANDS_[] = {
    ":help", ":quit", ":load", ":reload", ":reset", ":vars", ":info", ":type", ":del", ":macros", ":time", ":timing",
    ":history", ":redo", ":save", ":paste", ":set", ":color", ":mem", ":clear", ":typecheck", NULL
};
static const char **g_cands = NULL;
static int g_ncands = 0, g_capcands = 0;
static void cand_add(const char *s) {
    if (g_ncands == g_capcands) { g_capcands = g_capcands ? g_capcands * 2 : 256; g_cands = kc_realloc((void *)g_cands, sizeof(char *) * (size_t)g_capcands); }
    g_cands[g_ncands++] = s;
}
static void collect_candidates(void) {
    g_ncands = 0;
    for (int i = 0; KEYWORDS_[i]; i++) cand_add(KEYWORDS_[i]);
    for (int i = 0; COMMANDS_[i]; i++) cand_add(COMMANDS_[i]);
    for (KcEnv *e = g_ip->globals; e; e = e->parent)
        for (int i = 0; i < e->count; i++) cand_add(e->entries[i].name);
    for (int i = 0; i < g_ip->struct_types_count; i++) cand_add(g_ip->struct_types[i].name);
    for (int i = 0; i < kc_pp_macro_count(g_pp); i++) cand_add(kc_pp_macro_name(g_pp, i));
}
/* members after `name.`: module functions, struct fields, string / array / object methods */
static void collect_members(const char *obj) {
    g_ncands = 0;
    KcValue v;
    if (!kc_env_get(g_ip->globals, obj, &v)) return;
    if (v.type == VAL_MODULE) { for (int i = 0; i < v.as.module->count; i++) cand_add(v.as.module->entries[i].name); return; }
    const char *names[64]; int n = 0;
    if (v.type == VAL_STRUCT) {
        for (int i = 0; i < v.as.strct->count; i++) cand_add(v.as.strct->names[i]);
        n = kc_method_names(2, names, 64);
    } else if (v.type == VAL_STRING) n = kc_method_names(0, names, 64);
    else if (v.type == VAL_ARRAY) n = kc_method_names(1, names, 64);
    for (int i = 0; i < n; i++) cand_add(names[i]);
}
static char *completion_gen(const char *text, int state) {
    static int idx; static size_t len;
    if (state == 0) { idx = 0; len = strlen(text); }
    while (idx < g_ncands) {
        const char *c = g_cands[idx++];
        if (strncmp(c, text, len) != 0) continue;
        /* skip duplicates already offered earlier in the list */
        bool dup = false;
        for (int j = 0; j < idx - 1; j++) if (!strcmp(g_cands[j], c)) { dup = true; break; }
        if (dup) continue;
        return kc_strdup(c);
    }
    return NULL;
}
static char **complete(const char *text, int start, int end) {
    (void)end;
    const char *lb = rl_line_buffer;
    if (!strncmp(lb, ":load ", 6) || !strncmp(lb, ":save ", 6) || !strncmp(lb, ":l ", 3)) {
        rl_attempted_completion_over = 0;                       /* file names */
        return NULL;
    }
    rl_attempted_completion_over = 1;
    if (start > 0 && lb[start - 1] == '.') {
        int j = start - 2, e = j;
        while (j >= 0 && (isalnum((unsigned char)lb[j]) || lb[j] == '_')) j--;
        char obj[128]; int n = e - j; if (n <= 0 || n >= (int)sizeof obj) return NULL;
        memcpy(obj, lb + j + 1, (size_t)n); obj[n] = 0;
        collect_members(obj);
    } else collect_candidates();
    return rl_completion_matches(text, completion_gen);
}
static char g_hist_path[1024];
static void save_history_at_exit(void) { if (g_hist_path[0]) write_history(g_hist_path); }
static int indent_hook(void) { if (g_indent[0]) rl_insert_text(g_indent); return 0; }
#endif

/* Returns a malloc'd line without the newline, or NULL on EOF. */
static char *read_line(const char *prompt) {
    if (g_pending) { char *l = g_pending; g_pending = NULL; return l; }
#ifdef KC_READLINE
    if (g_tty) {
        char *l = readline(prompt);
        if (l && *l) add_history(l);
        return l;
    }
#endif
    if (g_tty) { fputs(prompt, stdout); fflush(stdout); }
    size_t cap = 128, n = 0;
    char *buf = kc_malloc(cap);
    for (;;) {
        int ch = fgetc(stdin);
        if (ch == EOF) {
            if (n == 0) { free(buf); return NULL; }
            break;
        }
        if (ch == '\n') break;
        if (n + 2 > cap) { cap *= 2; buf = kc_realloc(buf, cap); }
        buf[n++] = (char)ch;
    }
    if (n && buf[n - 1] == '\r') n--;
    buf[n] = 0;
    return buf;
}

static void load_startup_file(void) {
    const char *env = getenv("KC_REPL_INIT");
    char path[1100];
    if (env && *env) snprintf(path, sizeof path, "%s", env);
    else if (g_tty) {
        const char *home = getenv("HOME");
        if (!home) return;
        snprintf(path, sizeof path, "%s/.kingcobrarc", home);
    } else return;
    FILE *f = fopen(path, "rb");
    if (!f) { if (env && *env) fprintf(stderr, "kingcobra: cannot open KC_REPL_INIT file '%s'\n", path); return; }
    fclose(f);
    char keep[1024]; snprintf(keep, sizeof keep, "%s", g_last_load);
    load_file(path);
    snprintf(g_last_load, sizeof g_last_load, "%s", keep);        /* :reload refers to what the user loaded */
}

/* ------------------------------------------------------------------- main */

int kc_repl_main(const char *preload, bool typecheck) {
    g_typecheck = typecheck;
    g_tty = kc_stdin_is_tty();
    g_color = g_tty && !getenv("NO_COLOR");
    reset_session();

#ifdef KC_HAVE_SIGINT
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_sigint;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, NULL);   /* no SA_RESTART: a blocked read is interrupted */

        char marker; struct rlimit rl;
        g_stack_hi = &marker;
        g_stack_lim = (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) ? (size_t)rl.rlim_cur : (size_t)8 << 20;
        stack_t ss; ss.ss_sp = g_altstack; ss.ss_size = sizeof g_altstack; ss.ss_flags = 0;
        if (sigaltstack(&ss, NULL) == 0) {
            struct sigaction sv;
            memset(&sv, 0, sizeof sv);
            sv.sa_sigaction = on_segv;
            sv.sa_flags = SA_SIGINFO | SA_ONSTACK;
            sigemptyset(&sv.sa_mask);
            sigaction(SIGSEGV, &sv, NULL);
        }
    }
#endif

#ifdef KC_READLINE
    if (g_tty) {
        rl_attempted_completion_function = complete;
        rl_startup_hook = indent_hook;
        const char *home = getenv("HOME");
        if (home) {
            snprintf(g_hist_path, sizeof g_hist_path, "%s/.kingcobra_history", home);
            stifle_history(1000);
            read_history(g_hist_path);
            atexit(save_history_at_exit);
        }
    }
#endif

    load_startup_file();
    if (preload && !load_file(preload) && !g_tty) return 1;

    if (g_tty) printf("KingCobra %s REPL — help() for the language, :help for REPL commands, Ctrl-D to exit\n", KC_VERSION);

    sb_init(&buf);

    for (;;) {
#ifdef KC_HAVE_SIGINT
        if (sigsetjmp(g_jb, 1)) {
            /* Ctrl-C: abandon whatever was running / being typed */
            kc_err_top = NULL;
            kc_interp_reset_state(g_ip);
            fflush(stdout);
            if (g_segv_hit) fputs("error: stack overflow (recursion too deep)\n", stderr);
            else fputs(g_in_eval ? "\nKeyboardInterrupt\n" : "\n", stderr);
            g_segv_hit = 0;
            g_in_eval = 0;
            if (!g_tty) g_exit_code = 1;
            sb_clear(&buf);
            if (g_pending) { free(g_pending); g_pending = NULL; }
        }
        g_jb_ready = 1;
#endif
        const char *prompt = buf.n ? "... " : ">>> ";
        {                                                       /* auto-indent continuation lines by bracket depth */
            bool os; int d = buf.n ? bracket_depth(buf.d, &os) : 0;
            if (d < 0) d = 0;
            if (d > 20) d = 20;
            memset(g_indent, ' ', (size_t)d * 4); g_indent[d * 4] = 0;
        }
        char *line = read_line(prompt);
        if (!line) {
            if (g_tty) fputc('\n', stdout);
            if (buf.n) { /* unfinished input at EOF: surface the parse error */
                const char *nm = register_input(buf.d);
                PErr pe;
                KcNode *prog = parse_input_ex(buf.d, nm, &pe);
                if (!prog) { print_perr(&pe); g_exit_code = 1; }
            }
            break;
        }

        if (buf.n == 0) {
            if (handle_command(line)) { free(line); if (g_quit) break; continue; }
        }

        if (buf.n) sb_puts(&buf, "\n");
        sb_puts(&buf, line);
        free(line);

        /* skip blank input */
        bool blank = true;
        for (size_t i = 0; i < buf.n; i++) if (!isspace((unsigned char)buf.d[i])) { blank = false; break; }
        if (blank) { sb_clear(&buf); continue; }

        if (input_incomplete(buf.d)) continue;

        {   /* `///` doc-comment lines wait for the declaration they document */
            bool only_docs = true; const char *q = buf.d;
            while (*q) {
                while (*q == ' ' || *q == '\t') q++;
                if (*q == '\n') { q++; continue; }
                if (!(q[0] == '/' && q[1] == '/' && q[2] == '/')) { only_docs = false; break; }
                while (*q && *q != '\n') q++;
            }
            if (only_docs) continue;
        }

        const char *iname = register_input(buf.d);
        PErr pe;
        KcNode *prog = parse_input_ex(buf.d, iname, &pe);
        if (!prog) {
            if (continuation_error(&pe)) continue;              /* `try {}` still waiting for its catch, `1 +` ... */
            print_perr(&pe);
            if (!g_tty) g_exit_code = 1;
            sb_clear(&buf);
            continue;
        }
        if (may_take_else(prog)) {                               /* `if (...) { }` : look for an `else` on the next line */
            char *next = read_line("... ");
            if (next && starts_with_else(next)) { sb_puts(&buf, "\n"); sb_puts(&buf, next); free(next); continue; }
            if (next) {
                bool nb = true;
                for (char *q = next; *q; q++) if (!isspace((unsigned char)*q)) { nb = false; break; }
                if (nb) free(next); else g_pending = next;      /* a blank line just ends the statement */
            }
        }
        if (prog->as.program.stmts.count) {
            if (run_program(prog, true)) hist_add(buf.d);
            else if (!g_tty) g_exit_code = 1;
        }
        sb_clear(&buf);
    }
    fflush(stdout);
    return g_exit_code;
}
