/* The `kingcobra` command-line driver (v0.8).
 *
 *   kingcobra [options] <command> [args]      kingcobra [options] file.kc [script args]
 *
 * Commands: run repl eval check test preprocess new init completions info modules help version.
 * Global options (-D -I --no-typecheck --leaks --time --color) are accepted before the command and between
 * the command and the script; everything AFTER the script path belongs to the script, untouched.
 *
 * Exit status:  0 success · 1 a test failed · 64 usage error · 65 source / runtime error (parse, type,
 * uncaught exception) · 66 cannot read an input file · or whatever `main()` returns / `exit(n)` is given.
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>   License: MIT */
#include "kc_common.h"
#include "kc_platform.h"
#include "parser.h"
#include "interp.h"
#include "typecheck.h"
#include "jit.h"
#include "kc_arch.h"
#include "mem.h"
#include "repl.h"
#include "cli_tools.h"
#include "debug.h"
#include "cli.h"

#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define KC_MKDIR(p) _mkdir(p)
#else
#include <unistd.h>
#define KC_MKDIR(p) mkdir((p), 0777)
#endif

void kc_system_set_args(int argc, char **argv);   /* std/system/system.c */

enum { EX_OK = 0, EX_TESTFAIL = 1, EX_USAGE = 64, EX_DATA = 65, EX_NOINPUT = 66 };

static const char *g_prog = "kingcobra";

typedef struct { bool typecheck, leaks, timing; } Opts;

/* ---------------------------------------------------------------- command table */

typedef struct {
    const char *name, *args, *summary;
    const char *help;                         /* long text for `help <command>` / `<command> --help` */
} CmdInfo;

static bool g_debug;                      /* `debug FILE`: run under the debugger */
static const CmdInfo CMDS[] = {
    { "run", "<file.kc|-> [args...]", "Run a program (a bare file.kc means the same)",
      "Runs a KingCobra program: top-level statements first, then main() if there is one.\n"
      "  main()              no arguments\n"
      "  main(argv)          argv: array of strings, argv[0] is the script\n"
      "  main(argc, argv)    ... plus the count\n"
      "An int returned by main() is the process exit status. `-` reads the program from standard input.\n"
      "Everything after the file name is passed to the script, including things that look like options.\n"
      "Options: --no-typecheck  --leaks  --time  -DNAME[=v]  -I DIR  --color=WHEN" },
    { "repl", "[file.kc]", "Interactive session (optionally preloading a file)",
      "Starts the REPL (see docs/REPL.md). With no command at all on a terminal, kingcobra starts it too.\n"
      "The preloaded file's definitions are available; its main() is not called.\n"
      "Options: --no-typecheck  -DNAME[=v]  -I DIR  --color=WHEN" },
    { "eval", "<code> [args...]", "Run a snippet given on the command line (alias: -e)",
      "kingcobra -e 'print(6 * 7)'           run the code as a program (functions, loops, imports all work)\n"
      "kingcobra -p '6 * 7'                  print the value of one expression\n"
      "kingcobra eval 'print(argv)' a b      the arguments after the code arrive as `argv` (and `argc`)\n"
      "Errors show the offending line of the snippet." },
    { "debug", "<file.kc> [args...]", "Run under the debugger (driven by an IDE, see docs/DEBUGGER.md)",
      "Runs a program under the source-level debugger: breakpoints, step over/into/out, call stack, variables, expression\n"
      "evaluation, and a stop on any uncaught runtime error. An IDE drives it over two pipes whose base path is in\n"
      "$KC_DEBUG_PIPE (the bundled IDE does this: `make ide`, then Alt+F5). The program keeps its own stdin/stdout.\n"
      "The closure JIT is switched off while debugging. Protocol: docs/DEBUGGER.md.\n" },
    { "check", "[options] <file|dir>...", "Parse and type-check without running",
      "Checks every file (directories are searched recursively for *.kc) and reports every error.\n"
      "  -q, --quiet            print only problems\n"
      "  --format=text|json     json: one object per problem on stdout, for editors and CI:\n"
      "                         {\"file\":\"a.kc\",\"line\":3,\"col\":9,\"severity\":\"error\",\"message\":\"...\"}\n"
      "  --no-typecheck         parse only\n"
      "Exit status 65 if any file has errors." },
    { "test", "[options] [file|dir]...", "Run every test_* function",
      "Runs each global function whose name starts with test_ (a failing assert or an uncaught throw fails it).\n"
      "With no paths: test_*.kc and *_test.kc in . and tests/.  Directories are searched recursively.\n"
      "  -k, --filter TEXT      only tests whose name contains TEXT\n"
      "  -l, --list             list the tests without running them\n"
      "  -x, --fail-fast        stop after the first failure\n"
      "  -q, --quiet            show only failures and the summary\n"
      "  --no-typecheck\n"
      "Exit status 1 if any test failed." },
    { "preprocess", "<file.kc>", "Print the source after the preprocessor (alias: -E)",
      "Expands #define / #include / #if and friends and prints the result (line numbers are preserved).\n"
      "Useful for debugging macros:  kingcobra -DDEBUG -E main.kc" },
    { "new", "<name>", "Create a project directory with a program and a test",
      "Creates <name>/ with main.kc, tests/test_main.kc, README.md and .gitignore.\n"
      "Then:  cd <name> && kingcobra run main.kc && kingcobra test" },
    { "init", "", "Create the same files in the current directory (never overwrites)",
      "Like `new`, but in the current directory; files that already exist are left alone." },
    { "completions", "<bash|zsh|fish>", "Print a shell completion script",
      "  bash:  source <(kingcobra completions bash)            (or save it under /etc/bash_completion.d/)\n"
      "  zsh:   kingcobra completions zsh > \"${fpath[1]}/_kingcobra\"\n"
      "  fish:  kingcobra completions fish > ~/.config/fish/completions/kingcobra.fish" },
    { "info", "", "Version, platform, features and search paths",
      "Prints what this build is: version, compiler, platform, whether readline / X11 / the JIT are available,\n"
      "the standard-library modules, and the #include search path. Handy in bug reports." },
    { "modules", "", "List the standard-library modules (import std.<name>)", "One module name per line." },
    { "help", "[command|topic]", "Show help: a command, or a language topic", "kingcobra help <command>   or   kingcobra <command> --help\nkingcobra help map | switch | topics | builtins   the language's own help (same as help(\"map\") in a program)" },
    { "version", "", "Print the version", "Also: --version, -V, -v" },
};
#define NCMDS ((int)(sizeof CMDS / sizeof CMDS[0]))

static const CmdInfo *find_cmd(const char *n) {
    for (int i = 0; i < NCMDS; i++) if (strcmp(CMDS[i].name, n) == 0) return &CMDS[i];
    return NULL;
}

static const char *const GLOBAL_OPTS_HELP =
    "Options (before the command, or between it and the script):\n"
    "  -DNAME[=value]     define a preprocessor macro (#ifdef NAME / #if NAME == value)\n"
    "  -I DIR             add DIR to the #include <...> search path (also: $KC_INCLUDE, colon-separated)\n"
    "  --no-typecheck     skip the static type checker\n"
    "  --no-jit           never compile hot functions to machine code (interpreter only; also: KC_JIT=0)\n"
    "  --leaks            at exit, report alloc()/buffer() blocks never freed and objects still allocated\n"
    "  --time             report parse / check / run times on stderr\n"
    "  --color=WHEN       auto (default) | always | never; also honours NO_COLOR and FORCE_COLOR\n"
    "  -h, --help         this help          -V, --version    print the version\n";

static void print_usage(FILE *out) {
    fprintf(out, "KingCobra %s — a native systems programming language\n\n", KC_VERSION);
    fprintf(out, "Usage:\n  %s [options] <command> [args]\n  %s [options] <file.kc> [script args]     (same as `run`)\n"
                 "  %s -e <code>   %s -p <expression>   %s -E <file.kc>\n  %s                  on a terminal: start the REPL\n\n",
            g_prog, g_prog, g_prog, g_prog, g_prog, g_prog);
    fprintf(out, "Commands:\n");
    for (int i = 0; i < NCMDS; i++) {
        char left[96]; snprintf(left, sizeof left, "%s %s", CMDS[i].name, CMDS[i].args);
        fprintf(out, "  %-34s %s\n", left, CMDS[i].summary);
    }
    fprintf(out, "\n%s\nExit status: 0 ok · 1 a test failed · 64 usage error · 65 source or runtime error · 66 cannot read a file\n"
                 "             (a script's own exit code when main() returns an int or exit(n) is called)\n", GLOBAL_OPTS_HELP);
    fprintf(out, "\nMore: `%s help <command>`, docs/CLI.md, docs/LANGUAGE.md, docs/REPL.md\n", g_prog);
}

static void print_cmd_help(const CmdInfo *c) {
    printf("%s %s %s\n\n%s\n\n%s\n\n%s", g_prog, c->name, c->args, c->summary, c->help, GLOBAL_OPTS_HELP);
}

/* ---------------------------------------------------------------- suggestions */

static int edit_distance(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 40 || lb > 40) return 99;
    int d[41][41];
    for (int i = 0; i <= la; i++) d[i][0] = i;
    for (int j = 0; j <= lb; j++) d[0][j] = j;
    for (int i = 1; i <= la; i++)
        for (int j = 1; j <= lb; j++) {
            int c = a[i - 1] == b[j - 1] ? 0 : 1, m = d[i - 1][j] + 1;
            if (d[i][j - 1] + 1 < m) m = d[i][j - 1] + 1;
            if (d[i - 1][j - 1] + c < m) m = d[i - 1][j - 1] + c;
            d[i][j] = m;
        }
    return d[la][lb];
}
static const char *closest(const char *word, const char *const *cands, int n) {
    const char *best = NULL; int bd = 3;
    for (int i = 0; i < n; i++) { int d = edit_distance(word, cands[i]); if (d < bd) { bd = d; best = cands[i]; } }
    return best;
}

static const char *const LONG_OPTS[] = { "--help", "--version", "--no-typecheck", "--no-jit", "--leaks", "--time", "--color", "--no-color", "--eval", "--print", "--repl", "--list",
                                         "--filter", "--fail-fast", "--quiet", "--format" };

/* ---------------------------------------------------------------- global options */

static void add_include_dir(const char *dir) {
    const char *old = getenv("KC_INCLUDE");
    size_t n = strlen(dir) + (old ? strlen(old) : 0) + 2;
    char *v = kc_malloc(n);
    snprintf(v, n, "%s%s%s", dir, old && *old ? ":" : "", old ? old : "");
#ifdef _WIN32
    _putenv_s("KC_INCLUDE", v);
#else
    setenv("KC_INCLUDE", v, 1);
#endif
    free(v);
}

/* Tries to read a global option at argv[i]. Returns how many arguments it used, 0 if it is not one,
 * -1 if it is one but malformed (already reported). */
static int parse_global(Opts *o, int argc, char **argv, int i) {
    const char *a = argv[i];
    if (!strcmp(a, "--no-typecheck")) { o->typecheck = false; return 1; }
    if (!strcmp(a, "--no-jit")) { kc_jit_set_enabled(false); return 1; }
    if (!strcmp(a, "--leaks")) { o->leaks = true; return 1; }
    if (!strcmp(a, "--time")) { o->timing = true; return 1; }
    if (!strcmp(a, "--no-color")) { kc_color_mode = 0; return 1; }
    if (!strcmp(a, "--color") || !strncmp(a, "--color=", 8)) {
        const char *w = a[7] == '=' ? a + 8 : "always";
        if (!strcmp(w, "always") || !strcmp(w, "yes")) kc_color_mode = 1;
        else if (!strcmp(w, "never") || !strcmp(w, "no")) kc_color_mode = 0;
        else if (!strcmp(w, "auto")) kc_color_mode = -1;
        else { fprintf(stderr, "%s: --color expects auto, always or never (got '%s')\n", g_prog, w); return -1; }
        return 1;
    }
    if (!strncmp(a, "-D", 2)) {
        if (a[2]) { kc_pp_add_cmdline_define(a + 2); return 1; }
        if (i + 1 >= argc) { fprintf(stderr, "%s: -D needs a macro name (-DNAME or -DNAME=value)\n", g_prog); return -1; }
        kc_pp_add_cmdline_define(argv[i + 1]); return 2;
    }
    if (!strncmp(a, "-I", 2)) {
        if (a[2]) { add_include_dir(a + 2); return 1; }
        if (i + 1 >= argc) { fprintf(stderr, "%s: -I needs a directory\n", g_prog); return -1; }
        add_include_dir(argv[i + 1]); return 2;
    }
    return 0;
}

/* ---------------------------------------------------------------- running programs */

static char *g_src_text; static const char *g_src_name;
static bool cli_source_line(const char *file, int line, char *buf, size_t cap) {
    if (!g_src_text || !g_src_name || strcmp(file, g_src_name) != 0) return false;
    const char *p = g_src_text;
    for (int ln = 1; ln < line; ln++) { p = strchr(p, '\n'); if (!p) return false; p++; }
    const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p);
    if (n >= cap) n = cap - 1;
    memcpy(buf, p, n); buf[n] = 0;
    return true;
}

static char *read_stream(FILE *f) {
    size_t cap = 4096, n = 0; char *b = kc_malloc(cap);
    for (;;) {
        size_t got = fread(b + n, 1, cap - n - 1, f);
        n += got;
        if (got == 0) break;
        if (n + 1 >= cap) { cap *= 2; b = kc_realloc(b, cap); }
    }
    b[n] = 0;
    return b;
}

static char *read_source_file(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) { fprintf(stderr, "%s: '%s' is a directory (did you mean `%s check %s` or `%s test %s`?)\n", g_prog, path, g_prog, path, g_prog, path); exit(EX_NOINPUT); }
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "%s: cannot open '%s': %s\n", g_prog, path, strerror(errno));
        exit(EX_NOINPUT);
    }
    char *b = read_stream(f);
    fclose(f);
    return b;
}

static int run_source(const Opts *o, char *src, const char *name, int nargs, char **args) {
    kc_script_argv = kc_malloc(sizeof(char *) * (size_t)(nargs + 1));
    kc_script_argv[0] = (char *)name;
    for (int i = 0; i < nargs; i++) kc_script_argv[i + 1] = args[i];
    kc_script_argc = nargs + 1;
    kc_system_set_args(nargs, args);
    g_src_text = src; g_src_name = name;
    if (name[0] == '<') kc_source_line_hook = cli_source_line;

    double t0 = kc_monotonic_seconds();
    KcNode *program = kc_parse_program(src, name);
    double t1 = kc_monotonic_seconds();
    if (o->typecheck) kc_typecheck_program(program, name);
    double t2 = kc_monotonic_seconds();

    KcInterp *ip = kc_interp_new(name);
    kc_register_builtins(ip);
    if (g_debug) {
        char derr[300];
        if (!kc_dbg_init(ip, getenv("KC_DEBUG_PIPE"), derr, sizeof derr)) { fprintf(stderr, "%s: %s\n", g_prog, derr); return 70; }
    }
    kc_interp_run(ip, program);
    double t3 = kc_monotonic_seconds();

    if (o->leaks) { fflush(stdout); kc_mem_report(stderr); }
    fflush(stdout);
    if (o->timing)
        fprintf(stderr, "%s: parse %.2f ms · check %.2f ms · run %.2f ms · total %.2f ms\n", g_prog,
                (t1 - t0) * 1e3, (t2 - t1) * 1e3, (t3 - t2) * 1e3, (t3 - t0) * 1e3);
    return kc_main_exit_code;
}

/* run <file|-> [args]: options, then the file, then the script's own arguments (never interpreted) */
static int cmd_run(Opts *o, int argc, char **argv) {
    int i = 0;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { print_cmd_help(find_cmd("run")); return EX_OK; }
        int c = parse_global(o, argc, argv, i);
        if (c < 0) return EX_USAGE;
        if (c == 0) {
            if (argv[i][0] == '-' && argv[i][1]) {
                const char *s = closest(argv[i], LONG_OPTS, (int)(sizeof LONG_OPTS / sizeof *LONG_OPTS));
                fprintf(stderr, "%s: unknown option '%s'%s%s%s (use `--` before a file name that starts with '-')\n", g_prog, argv[i], s ? "; did you mean '" : "", s ? s : "", s ? "'?" : "");
                return EX_USAGE;
            }
            break;
        }
        i += c - 1;
    }
    if (i >= argc) { fprintf(stderr, "%s: 'run' needs a file (or - for standard input)\nTry: %s help run\n", g_prog, g_prog); return EX_USAGE; }
    const char *path = argv[i];
    char *src; const char *name = path;
    if (!strcmp(path, "-")) { src = read_stream(stdin); name = "<stdin>"; }
    else src = read_source_file(path);
    return run_source(o, src, name, argc - i - 1, argv + i + 1);
}

/* eval / -e / -p */
static int cmd_eval(Opts *o, int argc, char **argv, bool print_expr) {
    int i = 0;
    for (; i < argc; i++) {
        int c = parse_global(o, argc, argv, i);
        if (c < 0) return EX_USAGE;
        if (c == 0) break;
        i += c - 1;
    }
    if (i >= argc) { fprintf(stderr, "%s: %s needs %s\nTry: %s help eval\n", g_prog, print_expr ? "-p" : "-e / eval", print_expr ? "an expression" : "some code", g_prog); return EX_USAGE; }
    const char *code = argv[i];
    size_t n = strlen(code) + 32;
    char *src = kc_malloc(n);
    size_t cl = strlen(code);
    while (cl && isspace((unsigned char)code[cl - 1])) cl--;
    bool terminated = cl && (code[cl - 1] == ';' || code[cl - 1] == '}');       /* `-e 'print(1)'` needs no ';' */
    if (print_expr) snprintf(src, n, "print(%s);\n", code); else snprintf(src, n, "%s%s\n", code, terminated || !cl ? "" : ";");
    if (strstr(code, "argv") || strstr(code, "argc")) {            /* snippets see their arguments as `argv` / `argc` (kept on line 1) */
        int na = argc - i - 1; char **ar = argv + i + 1;
        size_t cap = strlen(src) + 64; for (int k = 0; k < na; k++) cap += strlen(ar[k]) * 2 + 8;
        char *full = kc_malloc(cap); size_t w = (size_t)snprintf(full, cap, "let argc = %d; let argv = [", na);
        for (int k = 0; k < na; k++) {
            full[w++] = '"';
            for (const char *q = ar[k]; *q; q++) {
                if (*q == '"' || *q == '\\') full[w++] = '\\';
                if (*q == '\n') { full[w++] = '\\'; full[w++] = 'n'; continue; }
                full[w++] = *q;
            }
            full[w++] = '"'; if (k + 1 < na) { full[w++] = ','; full[w++] = ' '; }
        }
        w += (size_t)snprintf(full + w, cap - w, "]; %s", src);
        src = full;
    }
    return run_source(o, src, print_expr ? "<-p>" : "<-e>", argc - i - 1, argv + i + 1);
}

static int cmd_check(Opts *o, int argc, char **argv) {
    KcCheckOpts c = { false, false, o->typecheck };
    int i = 0;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) { i++; break; }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { print_cmd_help(find_cmd("check")); return EX_OK; }
        if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) { c.quiet = true; continue; }
        if (!strncmp(a, "--format=", 9)) {
            if (!strcmp(a + 9, "json")) c.json = true;
            else if (!strcmp(a + 9, "text")) c.json = false;
            else { fprintf(stderr, "%s: --format expects text or json (got '%s')\n", g_prog, a + 9); return EX_USAGE; }
            continue;
        }
        int g = parse_global(o, argc, argv, i);
        if (g < 0) return EX_USAGE;
        if (g > 0) { c.typecheck = o->typecheck; i += g - 1; continue; }
        if (a[0] == '-' && a[1]) { fprintf(stderr, "%s: unknown option '%s' for 'check'\nTry: %s help check\n", g_prog, a, g_prog); return EX_USAGE; }
        break;
    }
    return kc_cmd_check(argc - i, argv + i, &c);
}

static int cmd_test(Opts *o, int argc, char **argv) {
    KcTestOpts t = { NULL, false, false, false, o->typecheck };
    int i = 0;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) { i++; break; }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { print_cmd_help(find_cmd("test")); return EX_OK; }
        if (!strcmp(a, "-l") || !strcmp(a, "--list")) { t.list = true; continue; }
        if (!strcmp(a, "-x") || !strcmp(a, "--fail-fast")) { t.fail_fast = true; continue; }
        if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) { t.quiet = true; continue; }
        if (!strcmp(a, "-k") || !strcmp(a, "--filter")) {
            if (i + 1 >= argc) { fprintf(stderr, "%s: %s needs a value\n", g_prog, a); return EX_USAGE; }
            t.filter = argv[++i]; continue;
        }
        if (!strncmp(a, "--filter=", 9)) { t.filter = a + 9; continue; }
        int g = parse_global(o, argc, argv, i);
        if (g < 0) return EX_USAGE;
        if (g > 0) { t.typecheck = o->typecheck; i += g - 1; continue; }
        if (a[0] == '-' && a[1]) { fprintf(stderr, "%s: unknown option '%s' for 'test'\nTry: %s help test\n", g_prog, a, g_prog); return EX_USAGE; }
        break;
    }
    return kc_cmd_test(argc - i, argv + i, &t);
}

static int cmd_preprocess(Opts *o, int argc, char **argv) {
    int i = 0;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { print_cmd_help(find_cmd("preprocess")); return EX_OK; }
        int c = parse_global(o, argc, argv, i);
        if (c < 0) return EX_USAGE;
        if (c == 0) break;
        i += c - 1;
    }
    if (i >= argc) { fprintf(stderr, "%s: 'preprocess' needs a file\n", g_prog); return EX_USAGE; }
    char *src = !strcmp(argv[i], "-") ? read_stream(stdin) : read_source_file(argv[i]);
    const char *name = !strcmp(argv[i], "-") ? "<stdin>" : argv[i];
    g_src_text = src; g_src_name = name; if (name[0] == '<') kc_source_line_hook = cli_source_line;
    const char *out = kc_preprocess(src, name, NULL);
    fputs(out, stdout);
    size_t n = strlen(out);
    if (n && out[n - 1] != '\n') fputc('\n', stdout);
    return EX_OK;
}

/* ---------------------------------------------------------------- project scaffolding */

static bool write_new(const char *dir, const char *rel, const char *text, bool *wrote_any) {
    char path[1024];
    snprintf(path, sizeof path, "%s%s%s", dir, *dir ? "/" : "", rel);
    struct stat st;
    if (stat(path, &st) == 0) { printf("  exists   %s (left alone)\n", path); return true; }
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "%s: cannot create '%s': %s\n", g_prog, path, strerror(errno)); return false; }
    fputs(text, f); fclose(f);
    printf("  created  %s\n", path);
    *wrote_any = true;
    return true;
}

static int scaffold(const char *dir, const char *name) {
    char main_kc[1024], readme[1024];
    snprintf(main_kc, sizeof main_kc,
        "// %s — created with `kingcobra new`.\n"
        "fn greet(name: string) -> string {\n"
        "    return f\"Hello, {name}!\";\n"
        "}\n\n"
        "// main(argv): argv[0] is this file, argv[1..] are the command-line arguments.\n"
        "fn main(argv) {\n"
        "    let who = len(argv) > 1 ? argv[1] : \"world\";\n"
        "    print(greet(who));\n"
        "}\n", name);
    snprintf(readme, sizeof readme,
        "# %s\n\n"
        "```\n"
        "kingcobra run main.kc            # run it\n"
        "kingcobra run main.kc Ada        # with an argument\n"
        "kingcobra test                   # run tests/test_*.kc\n"
        "kingcobra check .                # type-check every .kc file here\n"
        "```\n", name);
    bool any = false;
    if (*dir) {
        struct stat st;
        if (stat(dir, &st) != 0 && KC_MKDIR(dir) != 0) { fprintf(stderr, "%s: cannot create directory '%s': %s\n", g_prog, dir, strerror(errno)); return EX_NOINPUT; }
    }
    char tdir[1024]; snprintf(tdir, sizeof tdir, "%s%stests", dir, *dir ? "/" : "");
    struct stat st;
    if (stat(tdir, &st) != 0 && KC_MKDIR(tdir) != 0) { fprintf(stderr, "%s: cannot create directory '%s': %s\n", g_prog, tdir, strerror(errno)); return EX_NOINPUT; }
    if (!write_new(dir, "main.kc", main_kc, &any)) return EX_NOINPUT;
    if (!write_new(dir, "tests/test_main.kc",
        "import \"../main.kc\" as app;\n\n"
        "fn test_greet() {\n"
        "    assert_eq(app.greet(\"KingCobra\"), \"Hello, KingCobra!\");\n"
        "}\n", &any)) return EX_NOINPUT;
    if (!write_new(dir, "README.md", readme, &any)) return EX_NOINPUT;
    if (!write_new(dir, ".gitignore", "*.o\n*.so\n*.dll\n*.dylib\n*.exe\n", &any)) return EX_NOINPUT;
    printf("\nNext:  %s%s%s%s run main.kc   &&   %s test\n", *dir ? "cd " : "", dir, *dir ? " && " : "", g_prog, g_prog);
    return EX_OK;
}

static int cmd_new(int argc, char **argv) {
    if (argc >= 1 && (!strcmp(argv[0], "-h") || !strcmp(argv[0], "--help"))) { print_cmd_help(find_cmd("new")); return EX_OK; }
    if (argc != 1) { fprintf(stderr, "usage: %s new <name>\n", g_prog); return EX_USAGE; }
    struct stat st;
    if (stat(argv[0], &st) == 0) { fprintf(stderr, "%s: '%s' already exists (use `%s init` inside an existing directory)\n", g_prog, argv[0], g_prog); return EX_USAGE; }
    return scaffold(argv[0], argv[0]);
}
static int cmd_init(int argc, char **argv) {
    if (argc >= 1 && (!strcmp(argv[0], "-h") || !strcmp(argv[0], "--help"))) { print_cmd_help(find_cmd("init")); return EX_OK; }
    if (argc != 0) { fprintf(stderr, "usage: %s init\n", g_prog); return EX_USAGE; }
    return scaffold("", "project");
}

/* ---------------------------------------------------------------- completions */

static int cmd_completions(int argc, char **argv) {
    if (argc != 1 || !strcmp(argv[0], "-h") || !strcmp(argv[0], "--help")) { print_cmd_help(find_cmd("completions")); return argc == 1 ? EX_OK : EX_USAGE; }
    const char *sh = argv[0];
    if (!strcmp(sh, "bash")) {
        printf("# kingcobra bash completion — source <(kingcobra completions bash)\n_kingcobra() {\n"
               "  local cur=\"${COMP_WORDS[COMP_CWORD]}\" prev=\"${COMP_WORDS[COMP_CWORD-1]}\" cmds=\"");
        for (int i = 0; i < NCMDS; i++) printf("%s%s", i ? " " : "", CMDS[i].name);
        printf("\" opts=\"--help --version --no-typecheck --no-jit --leaks --time --color= --no-color -D -I -e -p -E\"\n"
               "  case \"$prev\" in\n"
               "    completions) COMPREPLY=($(compgen -W \"bash zsh fish\" -- \"$cur\")); return;;\n"
               "    help) COMPREPLY=($(compgen -W \"$cmds\" -- \"$cur\")); return;;\n"
               "    --color) COMPREPLY=($(compgen -W \"auto always never\" -- \"$cur\")); return;;\n"
               "    --format) COMPREPLY=($(compgen -W \"text json\" -- \"$cur\")); return;;\n"
               "    -I) COMPREPLY=($(compgen -d -- \"$cur\")); return;;\n"
               "  esac\n"
               "  if [[ \"$cur\" == --color=* ]]; then COMPREPLY=($(compgen -W \"auto always never\" -P \"--color=\" -- \"${cur#--color=}\")); return; fi\n"
               "  if [[ \"$cur\" == --format=* ]]; then COMPREPLY=($(compgen -W \"text json\" -P \"--format=\" -- \"${cur#--format=}\")); return; fi\n"
               "  if [[ $COMP_CWORD -eq 1 || \"$cur\" == -* ]]; then COMPREPLY=($(compgen -W \"$cmds $opts --quiet --list --filter --fail-fast --format=\" -- \"$cur\")); fi\n"
               "  COMPREPLY+=($(compgen -f -X '!*.kc' -- \"$cur\") $(compgen -d -- \"$cur\"))\n"
               "}\ncomplete -o filenames -F _kingcobra kingcobra kc\n");
        return EX_OK;
    }
    if (!strcmp(sh, "zsh")) {
        printf("#compdef kingcobra kc\n_kingcobra() {\n  local -a cmds\n  cmds=(\n");
        for (int i = 0; i < NCMDS; i++) printf("    '%s:%s'\n", CMDS[i].name, CMDS[i].summary);
        printf("  )\n  _arguments -C \\\n"
               "    '--no-typecheck[skip the static type checker]' '--no-jit[interpreter only, no machine code]' '--leaks[report leaked allocations at exit]' '--time[report phase timings]' \\\n"
               "    '--color=[colour diagnostics]:when:(auto always never)' '*-D[define a preprocessor macro]:macro' '*-I[add an include directory]:dir:_files -/' \\\n"
               "    '(-h --help)'{-h,--help}'[show help]' '(-V --version)'{-V,--version}'[print the version]' \\\n"
               "    '1: :->cmd' '*:: :->args'\n"
               "  case $state in\n"
               "    cmd) _describe 'command' cmds; _files -g '*.kc' ;;\n"
               "    args) case $words[1] in\n"
               "      completions) _values shell bash zsh fish ;;\n"
               "      help) _describe 'command' cmds ;;\n"
               "      check) _arguments '(-q --quiet)'{-q,--quiet}'[only problems]' '--format=[output format]:format:(text json)' '*:file:_files' ;;\n"
               "      test) _arguments '(-k --filter)'{-k,--filter}'[name filter]:text' '(-l --list)'{-l,--list}'[list tests]' '(-x --fail-fast)'{-x,--fail-fast}'[stop at first failure]' '(-q --quiet)'{-q,--quiet}'[only failures]' '*:file:_files' ;;\n"
               "      *) _files ;;\n"
               "    esac ;;\n  esac\n}\n_kingcobra \"$@\"\n");
        return EX_OK;
    }
    if (!strcmp(sh, "fish")) {
        printf("# kingcobra fish completion\ncomplete -c kingcobra -f -n '__fish_use_subcommand' -a '*.kc'\n");
        for (int i = 0; i < NCMDS; i++) printf("complete -c kingcobra -n '__fish_use_subcommand' -a %s -d '%s'\n", CMDS[i].name, CMDS[i].summary);
        printf("complete -c kingcobra -l no-typecheck -d 'skip the static type checker'\n"
               "complete -c kingcobra -l no-jit -d 'interpreter only, no machine code'\n"
               "complete -c kingcobra -l leaks -d 'report leaked allocations at exit'\n"
               "complete -c kingcobra -l time -d 'report phase timings'\n"
               "complete -c kingcobra -l color -x -a 'auto always never' -d 'colour diagnostics'\n"
               "complete -c kingcobra -s D -x -d 'define a preprocessor macro'\n"
               "complete -c kingcobra -s I -r -d 'add an include directory'\n"
               "complete -c kingcobra -n '__fish_seen_subcommand_from completions' -f -a 'bash zsh fish'\n"
               "complete -c kingcobra -n '__fish_seen_subcommand_from help' -f -a '");
        for (int i = 0; i < NCMDS; i++) printf("%s%s", i ? " " : "", CMDS[i].name);
        printf("'\ncomplete -c kingcobra -n '__fish_seen_subcommand_from check' -l format -x -a 'text json'\n"
               "complete -c kingcobra -n '__fish_seen_subcommand_from check test' -s q -l quiet -d 'only problems / failures'\n"
               "complete -c kingcobra -n '__fish_seen_subcommand_from test' -s k -l filter -x -d 'name filter'\n"
               "complete -c kingcobra -n '__fish_seen_subcommand_from test' -s l -l list -d 'list tests'\n"
               "complete -c kingcobra -n '__fish_seen_subcommand_from test' -s x -l fail-fast -d 'stop at first failure'\n");
        return EX_OK;
    }
    fprintf(stderr, "%s: unknown shell '%s' (expected bash, zsh or fish)\n", g_prog, sh);
    return EX_USAGE;
}

/* ---------------------------------------------------------------- info / modules */

static int cmd_info(void) {
    printf("KingCobra %s\n", KC_VERSION);
    const char *arch = KC_ARCH_NAME;
#if defined(_WIN32)
    const char *os = "windows";
#elif defined(__APPLE__)
    const char *os = "macos";
#elif defined(__linux__)
    const char *os = "linux";
#else
    const char *os = "unix";
#endif
    printf("  platform     %s/%s\n", os, arch);
#ifdef __VERSION__
    printf("  built with   %s\n", __VERSION__);
#endif
    printf("  word size    %d-bit\n", KC_ARCH_WORD_BITS);
    printf("  closure JIT  %s\n", kc_jit_available() ? "available (falls back to the interpreter if it cannot be used)" : kc_jit_status());
    printf("  REPL editing %s\n",
#ifdef KC_READLINE
           "GNU readline (history, Tab completion)"
#else
           "plain stdin (build with readline headers for history and completion)"
#endif
    );
    printf("  std.gui      %s\n",
#ifdef KC_GUI_X11
           "X11 window backend"
#else
           "headless"
#endif
    );
    printf("  colour       %s (NO_COLOR / FORCE_COLOR / --color)\n", kc_stderr_color() ? "on" : "off");
    const char *inc = getenv("KC_INCLUDE");
    printf("  #include <>  %s%s./include\n", inc ? inc : "", inc && *inc ? ":" : "");
    printf("  modules     ");
    for (int i = 0; kc_std_module_names[i]; i++) printf(" %s", kc_std_module_names[i]);
    printf("\n");
    return EX_OK;
}

static int cmd_modules(void) {
    for (int i = 0; kc_std_module_names[i]; i++) printf("%s\n", kc_std_module_names[i]);
    return EX_OK;
}

/* ---------------------------------------------------------------- main */

static const char *base_name(const char *p) {
    const char *s = strrchr(p, '/');
#ifdef _WIN32
    const char *b = strrchr(p, '\\'); if (b && (!s || b > s)) s = b;
#endif
    return s ? s + 1 : p;
}

static bool looks_like_path(const char *a) {
    if (strchr(a, '/') || strchr(a, '\\')) return true;
    size_t n = strlen(a);
    return (n > 3 && !strcmp(a + n - 3, ".kc")) || strchr(a, '.');
}

int kc_cli_main(int argc, char **argv) {
    kc_enable_ansi_colors();                                  /* no-op on POSIX; needed on Windows consoles */
    if (argc > 0) { g_prog = base_name(argv[0]); if (!*g_prog) g_prog = "kingcobra"; }
    Opts o = { true, false, false };

    int i = 1;
    for (; i < argc; i++) {                                    /* leading global options */
        int c = parse_global(&o, argc, argv, i);
        if (c < 0) return EX_USAGE;
        if (c == 0) break;
        i += c - 1;
    }

    if (i >= argc) {
        if (kc_stdin_is_tty()) return kc_repl_main(NULL, o.typecheck);
        print_usage(stderr);
        return EX_USAGE;
    }

    const char *cmd = argv[i];
    int rest = argc - i - 1; char **rargs = argv + i + 1;

    if (!strcmp(cmd, "--help") || !strcmp(cmd, "-h")) { print_usage(stdout); return EX_OK; }
    if (!strcmp(cmd, "--version") || !strcmp(cmd, "-V") || !strcmp(cmd, "-v") || !strcmp(cmd, "version")) { printf("KingCobra %s\n", KC_VERSION); return EX_OK; }
    if (!strcmp(cmd, "-e") || !strcmp(cmd, "--eval") || !strcmp(cmd, "eval")) return cmd_eval(&o, rest, rargs, false);
    if (!strcmp(cmd, "-p") || !strcmp(cmd, "--print")) return cmd_eval(&o, rest, rargs, true);
    if (!strcmp(cmd, "-E") || !strcmp(cmd, "preprocess") || !strcmp(cmd, "pp")) return cmd_preprocess(&o, rest, rargs);
    if (!strcmp(cmd, "-i") || !strcmp(cmd, "--repl") || !strcmp(cmd, "repl")) {
        int j = 0;
        for (; j < rest; j++) {
            if (!strcmp(rargs[j], "-h") || !strcmp(rargs[j], "--help")) { print_cmd_help(find_cmd("repl")); return EX_OK; }
            int c = parse_global(&o, rest, rargs, j);
            if (c < 0) return EX_USAGE;
            if (c == 0) break;
            j += c - 1;
        }
        kc_system_set_args(0, rargs + rest);
        return kc_repl_main(j < rest ? rargs[j] : NULL, o.typecheck);
    }
    if (!strcmp(cmd, "run")) return cmd_run(&o, rest, rargs);
    if (!strcmp(cmd, "debug")) { g_debug = true; return cmd_run(&o, rest, rargs); }
    if (!strcmp(cmd, "check")) return cmd_check(&o, rest, rargs);
    if (!strcmp(cmd, "test")) return cmd_test(&o, rest, rargs);
    if (!strcmp(cmd, "new")) return cmd_new(rest, rargs);
    if (!strcmp(cmd, "init")) return cmd_init(rest, rargs);
    if (!strcmp(cmd, "completions")) return cmd_completions(rest, rargs);
    if (!strcmp(cmd, "info")) return cmd_info();
    if (!strcmp(cmd, "modules")) return cmd_modules();
    if (!strcmp(cmd, "help")) {
        if (rest == 0) { print_usage(stdout); return EX_OK; }
        const CmdInfo *c = find_cmd(rargs[0]);
        if (!c) {                                          /* not a command: the language's own help (builtins, keywords, topics, modules) */
            char code[400]; size_t k = (size_t)snprintf(code, sizeof code, "help(\"");
            for (const char *q = rargs[0]; *q && k + 4 < sizeof code; q++) { if (*q == '"' || *q == '\\') code[k++] = '\\'; code[k++] = *q; }
            snprintf(code + k, sizeof code - k, "\");");
            return run_source(&o, kc_strdup(code), "<help>", 0, NULL);
        }
        print_cmd_help(c);
        return EX_OK;
    }

    /* Not a command. A dash-word is a mistyped option; a word that is not an existing file is a mistyped
     * command; anything else is a script (`kingcobra file.kc args...`). */
    if (cmd[0] == '-' && cmd[1]) {
        const char *s = closest(cmd, LONG_OPTS, (int)(sizeof LONG_OPTS / sizeof *LONG_OPTS));
        fprintf(stderr, "%s: unknown option '%s'%s%s%s\nTry: %s --help\n", g_prog, cmd, s ? "; did you mean '" : "", s ? s : "", s ? "'?" : "", g_prog);
        return EX_USAGE;
    }
    struct stat st;
    if (strcmp(cmd, "-") != 0 && stat(cmd, &st) != 0 && !looks_like_path(cmd)) {
        const char *names[NCMDS]; for (int k = 0; k < NCMDS; k++) names[k] = CMDS[k].name;
        const char *s = closest(cmd, names, NCMDS);
        fprintf(stderr, "%s: unknown command '%s'%s%s%s\nTry: %s help\n", g_prog, cmd, s ? " — did you mean '" : "", s ? s : "", s ? "'?" : "", g_prog);
        return EX_USAGE;
    }
    /* script shorthand: options may sit between nothing and the file only (we already consumed the leading ones) */
    char **sargs = argv + i;
    return cmd_run(&o, argc - i, sargs);
}
