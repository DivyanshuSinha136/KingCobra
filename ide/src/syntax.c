#include "syntax.h"
#include <ctype.h>

const char *const KC_KEYWORDS[] = { "let", "var", "const", "fn", "struct", "enum", "impl", "type", "if", "else", "while", "for", "in", "return",
    "break", "continue", "import", "as", "extern", "from", "unsafe", "comptime", "match", "try", "catch", "throw", "defer", "packed", "true", "false", "null",
    "do", "switch", "case", "default", "goto", "static", "typedef", "union", "auto", "extrn" };
const int KC_N_KEYWORDS = (int)(sizeof KC_KEYWORDS / sizeof *KC_KEYWORDS);
const char *const KC_TYPES[] = { "int", "float", "string", "bool", "void", "any", "u8", "i8", "u16", "i16", "u32", "i32", "u64", "i64", "usize", "isize", "f32", "f64" };
const int KC_N_TYPES = (int)(sizeof KC_TYPES / sizeof *KC_TYPES);
const char *const KC_BUILTINS[] = { "print", "println", "len", "str", "int", "float", "typeof", "push", "range", "clock", "sqrt", "input",
    "assert", "assert_eq", "error", "abs", "min", "max", "clamp", "idiv", "ord", "chr", "map", "filter", "reduce", "find", "any", "all", "each", "sum", "sorted", "zip",
    "keys", "values", "items", "has", "free", "free_deep", "is_freed", "mem_stats", "mem_report", "mem_quarantine",
    "alloc", "alloc_array", "realloc", "memcpy", "memmove", "memset", "memcmp", "buffer", "buffer_from", "ptr_from_addr",
    "align_up", "align_down", "is_aligned", "sizeof", "alignof", "offsetof",
    "printf", "sprintf", "format", "eprintf", "puts", "putchar", "getchar", "fflush", "printn", "putnumb", "strlen", "strcmp", "strncmp", "strcasecmp",
    "strchr", "strrchr", "strstr", "strcat", "strdup", "atoi", "atol", "atof", "strtol", "strtod", "itoa", "isdigit", "isalpha", "isalnum", "isspace",
    "isupper", "islower", "ispunct", "isxdigit", "isprint", "iscntrl", "isgraph", "toupper", "tolower", "exit", "abort", "rand", "srand", "time",
    "qsort", "bsearch", "floor", "ceil", "round", "trunc", "fabs", "pow", "fmod", "hypot", "atan2", "sin", "cos", "tan", "asin", "acos", "atan",
    "exp", "log", "log2", "log10", "cbrt", "char", "lchar", "getvec", "rlsevec", "help" };
const int KC_N_BUILTINS = (int)(sizeof KC_BUILTINS / sizeof *KC_BUILTINS);
const char *const KC_METHODS[] = { "upper", "lower", "trim", "split", "contains", "index_of", "starts_with", "ends_with", "replace", "slice", "repeat",
    "pad_left", "pad_right", "chars", "lines", "push", "pop", "join", "reverse", "map", "filter", "reduce", "find", "any", "all", "each", "sort", "concat",
    "first", "last", "clear", "insert", "remove", "extend", "sum", "min", "max", "keys", "values", "items", "has", "get", "merge", "message" };
const int KC_N_METHODS = (int)(sizeof KC_METHODS / sizeof *KC_METHODS);
const char *const KC_STD_MODULES[] = { "acpi", "ascii_screen", "csv", "fbin", "game", "gui", "http", "io", "json", "memory", "mmio", "mmu",
    "music_note", "network", "nlp", "nn", "regx", "rhex", "system", "tts", "vector" };
const int KC_N_STD_MODULES = (int)(sizeof KC_STD_MODULES / sizeof *KC_STD_MODULES);

static bool in_list(const char *const *l, int n, const char *s, int len) {
    for (int i = 0; i < n; i++) if ((int)strlen(l[i]) == len && memcmp(l[i], s, (size_t)len) == 0) return true;
    return false;
}
bool kc_is_std_module(const char *s, int n) { return in_list(KC_STD_MODULES, KC_N_STD_MODULES, s, n); }
bool hl_is_code(uint8_t c) { return c != HC_COMMENT && c != HC_DOC && c != HC_TODO && c != HC_STRING && c != HC_ESCAPE && c != HC_CHAR; }

static bool is_id0(unsigned char c) { return isalpha(c) || c == '_' || c >= 0x80; }
static bool is_id(unsigned char c) { return isalnum(c) || c == '_' || c >= 0x80; }
static int skip_ws(const char *s, int len, int i) { while (i < len && (s[i] == ' ' || s[i] == '\t')) i++; return i; }
static void fill(uint8_t *out, int a, int b, uint8_t c) { for (int i = a; i < b; i++) out[i] = c; }

static void mark_todo(const char *s, uint8_t *out, int a, int b) {
    static const char *W[] = { "TODO", "FIXME", "XXX", "HACK", "NOTE", "BUG" };
    for (int i = a; i < b; i++) {
        if (i > a && is_id((unsigned char)s[i - 1])) continue;
        for (size_t k = 0; k < sizeof W / sizeof *W; k++) {
            int n = (int)strlen(W[k]);
            if (i + n <= b && memcmp(s + i, W[k], (size_t)n) == 0 && (i + n == b || !is_id((unsigned char)s[i + n]))) { fill(out, i, i + n, HC_TODO); i += n - 1; break; }
        }
    }
}

/* Scans string-ish text from i until the closing quote; returns the index after it (or len). */
static int scan_str_body(const char *s, int len, int i, uint8_t *out, bool *closed) {
    *closed = false;
    while (i < len) {
        if (s[i] == '\\' && i + 1 < len) {
            int e = i + 2;
            if (s[i + 1] == 'x') { while (e < len && e < i + 4 && isxdigit((unsigned char)s[e])) e++; }
            else if (s[i + 1] == 'u' && e < len && s[e] == '{') { while (e < len && s[e] != '}') e++; if (e < len) e++; }
            fill(out, i, e, HC_ESCAPE); i = e; continue;
        }
        if (s[i] == '"') { out[i] = HC_STRING; *closed = true; return i + 1; }
        out[i] = HC_STRING; i++;
    }
    return len;
}

static const char *OPS3[] = { ">>>", "<<=", ">>=", "...", NULL };
static const char *OPS2[] = { "==", "!=", "<=", ">=", "&&", "||", "<<", ">>", "??", "->", "=>", "..", "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", NULL };

uint8_t hl_line(const char *s, int len, uint8_t st, uint8_t *out) {
    int mode = st & 7, depth = st >> 3;
    int i = 0;
    int prev = 0;                    /* 0 none, 1 'fn', 2 type keyword, 3 '.', 4 import, 5 import std '.', 6 break/continue, 7 let/var/const */
    bool after_import_std = false;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (mode == HS_BLOCK) {
            int j = i; bool end = false;
            while (j < len) { if (s[j] == '*' && j + 1 < len && s[j + 1] == '/') { j += 2; end = true; break; } j++; }
            fill(out, i, j, HC_COMMENT); mark_todo(s, out, i, j); i = j; if (end) mode = HS_NORMAL;
            continue;
        }
        if (mode == HS_STRING) {
            bool closed; int j = scan_str_body(s, len, i, out, &closed); i = j; if (closed) mode = HS_NORMAL; continue;
        }
        if (mode == HS_FSTR) {
            while (i < len) {
                if (s[i] == '\\' && i + 1 < len) { bool cl; int j = i; /* reuse escape logic */
                    int e = j + 2; if (s[j + 1] == 'x') { while (e < len && e < j + 4 && isxdigit((unsigned char)s[e])) e++; } else if (s[j + 1] == 'u' && e < len && s[e] == '{') { while (e < len && s[e] != '}') e++; if (e < len) e++; }
                    fill(out, j, e, HC_ESCAPE); i = e; (void)cl; continue; }
                if (s[i] == '{' && i + 1 < len && s[i + 1] == '{') { out[i] = out[i + 1] = HC_STRING; i += 2; continue; }
                if (s[i] == '}' && i + 1 < len && s[i + 1] == '}') { out[i] = out[i + 1] = HC_STRING; i += 2; continue; }
                if (s[i] == '{') { out[i++] = HC_INTERP; mode = HS_FEXPR; depth = 1; break; }
                if (s[i] == '"') { out[i++] = HC_STRING; mode = HS_NORMAL; break; }
                out[i++] = HC_STRING;
            }
            continue;
        }
        /* ---- code (HS_NORMAL or HS_FEXPR) ---- */
        if (c == ' ' || c == '\t') { out[i++] = HC_TEXT; continue; }
        if (c == '/' && i + 1 < len && s[i + 1] == '/') {
            uint8_t cl = (i + 2 < len && s[i + 2] == '/' && !(i + 3 < len && s[i + 3] == '/')) ? HC_DOC : HC_COMMENT;
            fill(out, i, len, cl); mark_todo(s, out, i, len); i = len; continue;
        }
        if (c == '/' && i + 1 < len && s[i + 1] == '*') { fill(out, i, i + 2, HC_COMMENT); i += 2; mode = HS_BLOCK; continue; }
        if (c == '"') {
            out[i++] = HC_STRING;
            if (mode == HS_FEXPR) { bool cl; i = scan_str_body(s, len, i, out, &cl); continue; }
            mode = HS_STRING; continue;
        }
        if (c == 'f' && i + 1 < len && s[i + 1] == '"' && (i == 0 || !is_id((unsigned char)s[i - 1]))) {
            if (mode == HS_FEXPR) { fill(out, i, i + 2, HC_STRING); bool cl; i = scan_str_body(s, len, i + 2, out, &cl); continue; }
            fill(out, i, i + 2, HC_STRING); i += 2; mode = HS_FSTR; continue;
        }
        if (c == '\'') {
            int j = i + 1;
            if (j < len && s[j] == '\\') { j++; if (j < len) { if (s[j] == 'x') { j++; int k = 0; while (j < len && k < 2 && isxdigit((unsigned char)s[j])) { j++; k++; } } else if (s[j] == 'u' && j + 1 < len && s[j + 1] == '{') { while (j < len && s[j] != '}') j++; if (j < len) j++; } else j++; } }
            else if (j < len) { uint32_t cp; j += utf8_decode(s + j, (size_t)(len - j), &cp); }
            if (j < len && s[j] == '\'') { fill(out, i, j + 1, HC_CHAR); i = j + 1; }
            else { out[i++] = HC_OPERATOR; }
            prev = 0; continue;
        }
        if (isdigit(c)) {
            int j = i;
            if (c == '0' && j + 1 < len && (s[j + 1] == 'x' || s[j + 1] == 'X')) { j += 2; while (j < len && (isxdigit((unsigned char)s[j]) || s[j] == '_')) j++; }
            else if (c == '0' && j + 1 < len && (s[j + 1] == 'b' || s[j + 1] == 'B')) { j += 2; while (j < len && (s[j] == '0' || s[j] == '1' || s[j] == '_')) j++; }
            else if (c == '0' && j + 1 < len && (s[j + 1] == 'o' || s[j + 1] == 'O')) { j += 2; while (j < len && ((s[j] >= '0' && s[j] <= '7') || s[j] == '_')) j++; }
            else {
                while (j < len && (isdigit((unsigned char)s[j]) || s[j] == '_')) j++;
                if (j + 1 < len && s[j] == '.' && isdigit((unsigned char)s[j + 1])) { j++; while (j < len && (isdigit((unsigned char)s[j]) || s[j] == '_')) j++; }
                if (j < len && (s[j] == 'e' || s[j] == 'E')) {
                    int k = j + 1; if (k < len && (s[k] == '+' || s[k] == '-')) k++;
                    if (k < len && isdigit((unsigned char)s[k])) { while (k < len && isdigit((unsigned char)s[k])) k++; j = k; }
                }
            }
            fill(out, i, j, HC_NUMBER); i = j; prev = 0; continue;
        }
        if (c == '#' && mode == HS_NORMAL && i + 1 < len && is_id0((unsigned char)s[i + 1])) {     /* preprocessor directive at line start */
            bool bol = true; for (int k = 0; k < i; k++) if (s[k] != ' ' && s[k] != '\t') { bol = false; break; }
            if (bol) {
                int j = i + 1; while (j < len && is_id((unsigned char)s[j])) j++;
                int n = j - (i + 1); const char *w = s + i + 1;
                fill(out, i, j, HC_DECL);
                bool names = (n == 6 && !memcmp(w, "define", 6)) || (n == 5 && !memcmp(w, "undef", 5)) || (n == 5 && !memcmp(w, "ifdef", 5)) || (n == 6 && !memcmp(w, "ifndef", 6));
                i = j; prev = names ? 7 : 0;
                continue;
            }
        }
        if (is_id0(c)) {
            int j = i; while (j < len && is_id((unsigned char)s[j])) j++;
            int n = j - i; const char *w = s + i;
            int nx = skip_ws(s, len, j); char nc = nx < len ? s[nx] : 0; char nc2 = nx + 1 < len ? s[nx + 1] : 0;
            uint8_t cls = HC_TEXT; int newprev = 0;
            #define IS(k) (n == (int)sizeof(k) - 1 && memcmp(w, k, (size_t)n) == 0)
            if (prev == 3) {                                  /* after '.' */
                if (after_import_std) { cls = HC_MODULE; after_import_std = false; }
                else cls = nc == '(' ? HC_FUNC : HC_FIELD;
            } else if (prev == 4 && IS("std")) { cls = HC_MODULE; newprev = 5; }
            else if (prev == 1) { cls = HC_FUNCDEF; }
            else if (prev == 2) { cls = HC_TYPEDEF; }
            else if (prev == 6) { cls = HC_LABEL; }
            else if (IS("fn")) { cls = HC_DECL; newprev = 1; }
            else if (IS("struct")) { cls = HC_DECL; newprev = 2; }
            else if (IS("enum") || IS("impl")) { if (is_id0((unsigned char)nc)) { cls = HC_DECL; newprev = 2; } }
            else if (IS("type")) { int a = nx; if (is_id0((unsigned char)nc)) { while (a < len && is_id((unsigned char)s[a])) a++; a = skip_ws(s, len, a); if (a < len && s[a] == '=') { cls = HC_DECL; newprev = 2; } } }
            else if (IS("let") || IS("var")) { cls = HC_DECL; newprev = 7; }
            else if (IS("const")) { if (is_id0((unsigned char)nc) || nc == '[' || nc == '{') { cls = HC_DECL; newprev = 7; } }
            else if (IS("import")) { cls = HC_DECL; newprev = 4; }
            else if (IS("extern") || IS("unsafe") || IS("comptime")) cls = HC_DECL;
            else if (IS("packed")) { if (nx < len && is_id0((unsigned char)nc)) cls = HC_DECL; }
            else if (IS("if") || IS("else") || IS("while") || IS("for") || IS("in") || IS("return") || IS("as")) cls = HC_KEYWORD;
            else if (IS("break") || IS("continue")) { cls = HC_KEYWORD; newprev = 6; }
            else if (IS("match")) { if (nc == '(') cls = HC_KEYWORD; }
            else if (IS("switch")) { if (nc == '(') cls = HC_KEYWORD; }
            else if (IS("do")) { if (nc == '{') cls = HC_KEYWORD; }
            else if (IS("case")) { if (nc && nc != '=' && nc != '.' && nc != ',' && nc != ';' && nc != ')' && nc != '(') cls = HC_KEYWORD; }
            else if (IS("default")) { if (nc == ':' && nc2 != ':') cls = HC_KEYWORD; }
            else if (IS("goto")) { if (is_id0((unsigned char)nc)) { cls = HC_KEYWORD; newprev = 6; } }
            else if (IS("static") || IS("typedef") || IS("extrn")) { if (is_id0((unsigned char)nc)) cls = HC_DECL; }
            else if (IS("auto")) { if (is_id0((unsigned char)nc)) { cls = HC_DECL; newprev = 7; } }
            else if (IS("union")) { if (is_id0((unsigned char)nc)) { cls = HC_DECL; newprev = 2; } }
            else if (IS("try")) { if (nc == '{') cls = HC_KEYWORD; }
            else if (IS("catch")) { if (nc == '(' || nc == '{') cls = HC_KEYWORD; }
            else if (IS("throw")) { if (nc && nc != '=' && nc != '.' && nc != ';' && nc != ',' && nc != ')') cls = HC_KEYWORD; }
            else if (IS("defer")) { if (is_id0((unsigned char)nc) || nc == '{' || nc == '"' || isdigit((unsigned char)nc)) cls = HC_KEYWORD; }
            else if (IS("from")) { if (nc == '"') cls = HC_KEYWORD; }
            else if (IS("true") || IS("false") || IS("null")) cls = HC_CONST;
            else if (IS("self")) cls = HC_SELF;
            else if (in_list(KC_TYPES, KC_N_TYPES, w, n) && nc != '(') cls = HC_TYPE;
            else if (nc == ':' && nc2 != ':' && nx > j - 1) {
                /* label?  `name: for` / `name: while` */
                int a = skip_ws(s, len, nx + 1);
                if ((a + 3 <= len && memcmp(s + a, "for", 3) == 0 && (a + 3 == len || !is_id((unsigned char)s[a + 3]))) ||
                    (a + 5 <= len && memcmp(s + a, "while", 5) == 0 && (a + 5 == len || !is_id((unsigned char)s[a + 5])))) cls = HC_LABEL;
            }
            if (cls == HC_TEXT) {
                if (nc == '.' && kc_is_std_module(w, n) && nc2 != '.') cls = HC_MODULE;
                else if (in_list(KC_BUILTINS, KC_N_BUILTINS, w, n) && (nc == '(' || IS("sizeof"))) cls = HC_BUILTIN;
                else if (nc == '(') cls = HC_FUNC;
                else if (isupper((unsigned char)w[0])) {
                    bool allcaps = true, lower = false;
                    for (int k = 0; k < n; k++) { if (islower((unsigned char)w[k])) { lower = true; allcaps = false; } }
                    cls = (allcaps && n > 1 && !lower) ? HC_CONST : HC_TYPE;
                }
            }
            #undef IS
            fill(out, i, j, cls);
            if (newprev == 5) { after_import_std = false; }
            i = j; prev = newprev;
            if (prev == 5) { after_import_std = true; }
            continue;
        }
        /* operators and punctuation */
        {
            int n = 0;
            for (int k = 0; OPS3[k]; k++) if (i + 3 <= len && memcmp(s + i, OPS3[k], 3) == 0) { n = 3; break; }
            if (!n) for (int k = 0; OPS2[k]; k++) if (i + 2 <= len && memcmp(s + i, OPS2[k], 2) == 0) { n = 2; break; }
            if (n) { fill(out, i, i + n, HC_OPERATOR); i += n; prev = 0; continue; }
        }
        if (c == '.') { out[i++] = HC_PUNCT; prev = (prev == 5 || prev == 4) ? 3 : 3; if (!after_import_std) { /* normal member access */ } continue; }
        if (strchr("()[]{},;", c)) {
            if (mode == HS_FEXPR) {
                if (c == '{') depth++;
                else if (c == '}') { if (--depth <= 0) { depth = 0; out[i++] = HC_INTERP; mode = HS_FSTR; prev = 0; continue; } }
            }
            out[i++] = HC_PUNCT; prev = 0; continue;
        }
        if (strchr("+-*/%=<>!~&|^?:", c)) { out[i++] = HC_OPERATOR; prev = 0; continue; }
        out[i++] = HC_TEXT; prev = 0;
    }
    if (depth < 0) depth = 0; if (depth > 31) depth = 31;
    return (uint8_t)(mode | (depth << 3));
}

/* ---------------------------------------------------------------- symbols */
const char *sym_kind_name(SymKind k) {
    static const char *N[] = { "fn", "struct", "enum", "impl", "type", "const", "var" };
    return N[k];
}
bool sym_scan_line(const char *s, int len, SymHit *o) {
    int i = skip_ws(s, len, 0), indent = i;
    if (i >= len || s[i] == '/' || s[i] == '*') return false;
    static const struct { const char *kw; SymKind k; } KW[] = { { "fn", SYM_FN }, { "struct", SYM_STRUCT }, { "union", SYM_STRUCT }, { "enum", SYM_ENUM }, { "impl", SYM_IMPL }, { "type", SYM_TYPE }, { "const", SYM_CONST } };
    if (len - i > 7 && memcmp(s + i, "packed ", 7) == 0) i = skip_ws(s, len, i + 7);
    for (size_t k = 0; k < sizeof KW / sizeof *KW; k++) {
        int n = (int)strlen(KW[k].kw);
        if (i + n < len && memcmp(s + i, KW[k].kw, (size_t)n) == 0 && (s[i + n] == ' ' || s[i + n] == '\t')) {
            int a = skip_ws(s, len, i + n);
            if (a >= len || !is_id0((unsigned char)s[a])) return false;
            int b = a; while (b < len && is_id((unsigned char)s[b])) b++;
            if (b - a >= (int)sizeof o->name) return false;
            memcpy(o->name, s + a, (size_t)(b - a)); o->name[b - a] = 0; o->kind = KW[k].k; o->col = a; o->indent = indent;
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------- themes */
#define HLC(...) { __VA_ARGS__ }
const Theme THEMES[] = {
    { .name = "Cobra Dark", .bg = 0x16181d, .fg = 0xd7dae0, .sel_bg = 0x33467a, .line_bg = 0x1d2027, .gutter_fg = 0x4b5263, .gutter_bg = 0x16181d, .gutter_cur = 0xa0a8b7,
      .ui_bg = 0x1b1e24, .ui_fg = 0xc0c5d0, .ui_dim = 0x6b7280, .accent = 0x3fb950, .tab_on_bg = 0x16181d, .tab_off_bg = 0x23262e, .status_bg = 0x1f6f43, .status_fg = 0xf0fff4,
      .border = 0x2c313c, .menu_bg = 0x23262e, .menu_sel = 0x2f6f4f, .popup_bg = 0x23262e, .popup_sel = 0x2f4f7f, .error = 0xf47067, .warn = 0xe3b341, .info = 0x58a6ff, .ok = 0x56d364,
      .match_bg = 0x3b4a6b, .find_bg = 0x5a4a14, .find_cur_bg = 0x9a6c10, .bracket_bg = 0x45506b, .guide = 0x2a2e37, .occurrence_bg = 0x2a3140,
      .hl = { [HC_TEXT] = 0xd7dae0, [HC_COMMENT] = 0x6a737d, [HC_DOC] = 0x7f8c99, [HC_TODO] = 0xffb86c, [HC_STRING] = 0x9ece6a, [HC_ESCAPE] = 0x56d4dd, [HC_CHAR] = 0x9ece6a,
              [HC_INTERP] = 0xff9e64, [HC_NUMBER] = 0xff9e64, [HC_KEYWORD] = 0xbb9af7, [HC_DECL] = 0xf7768e, [HC_TYPE] = 0x2ac3de, [HC_TYPEDEF] = 0x73daca,
              [HC_BUILTIN] = 0x7dcfff, [HC_FUNC] = 0x7aa2f7, [HC_FUNCDEF] = 0x82aaff, [HC_CONST] = 0xe0af68, [HC_OPERATOR] = 0x89ddff, [HC_PUNCT] = 0x9aa5ce,
              [HC_MODULE] = 0xc3e88d, [HC_FIELD] = 0xc0caf5, [HC_LABEL] = 0xe0af68, [HC_SELF] = 0xf7768e, [HC_NOTE] = 0x4fd6be, [HC_DURATION] = 0xe0af68, [HC_CHORD] = 0xff7eb6, [HC_TAG] = 0xf7768e, [HC_ATTR] = 0xe0af68, [HC_ENTITY] = 0x56d4dd },
      .hl_attr = { [HC_COMMENT] = ATTR_ITALIC, [HC_DOC] = ATTR_ITALIC, [HC_TODO] = ATTR_BOLD, [HC_DECL] = ATTR_BOLD, [HC_FUNCDEF] = ATTR_BOLD, [HC_TYPEDEF] = ATTR_BOLD, [HC_SELF] = ATTR_ITALIC, [HC_CHORD] = ATTR_BOLD } },
    { .name = "Cobra Light", .bg = 0xfafafa, .fg = 0x383a42, .sel_bg = 0xbfd4f2, .line_bg = 0xf0f0f0, .gutter_fg = 0xb0b4bc, .gutter_bg = 0xfafafa, .gutter_cur = 0x4b5160,
      .ui_bg = 0xeceef2, .ui_fg = 0x3a3f4b, .ui_dim = 0x8a90a0, .accent = 0x1a7f37, .tab_on_bg = 0xfafafa, .tab_off_bg = 0xdfe2e8, .status_bg = 0x1a7f37, .status_fg = 0xffffff,
      .border = 0xd0d4dc, .menu_bg = 0xf3f4f7, .menu_sel = 0xb7e4c7, .popup_bg = 0xf3f4f7, .popup_sel = 0xbfd4f2, .error = 0xd1242f, .warn = 0x9a6700, .info = 0x0969da, .ok = 0x1a7f37,
      .match_bg = 0xdbe6f7, .find_bg = 0xfff3b0, .find_cur_bg = 0xffd54a, .bracket_bg = 0xc6d4ee, .guide = 0xe4e6ea, .occurrence_bg = 0xe8edf5,
      .hl = { [HC_TEXT] = 0x383a42, [HC_COMMENT] = 0xa0a1a7, [HC_DOC] = 0x8a8c94, [HC_TODO] = 0xc2410c, [HC_STRING] = 0x50a14f, [HC_ESCAPE] = 0x0184bc, [HC_CHAR] = 0x50a14f,
              [HC_INTERP] = 0xc18401, [HC_NUMBER] = 0xb76b01, [HC_KEYWORD] = 0xa626a4, [HC_DECL] = 0xd73a49, [HC_TYPE] = 0x0e7c86, [HC_TYPEDEF] = 0x0e7c86,
              [HC_BUILTIN] = 0x0184bc, [HC_FUNC] = 0x4078f2, [HC_FUNCDEF] = 0x2f5bd0, [HC_CONST] = 0xb76b01, [HC_OPERATOR] = 0x0184bc, [HC_PUNCT] = 0x696c77,
              [HC_MODULE] = 0x3b8a1a, [HC_FIELD] = 0x4b4f5c, [HC_LABEL] = 0xb76b01, [HC_SELF] = 0xd73a49, [HC_NOTE] = 0x0e7c86, [HC_DURATION] = 0xb76b01, [HC_CHORD] = 0xc2185b, [HC_TAG] = 0xd73a49, [HC_ATTR] = 0xb76b01, [HC_ENTITY] = 0x0184bc },
      .hl_attr = { [HC_COMMENT] = ATTR_ITALIC, [HC_DOC] = ATTR_ITALIC, [HC_TODO] = ATTR_BOLD, [HC_DECL] = ATTR_BOLD, [HC_FUNCDEF] = ATTR_BOLD, [HC_TYPEDEF] = ATTR_BOLD, [HC_SELF] = ATTR_ITALIC, [HC_CHORD] = ATTR_BOLD } },
    { .name = "Monokai", .bg = 0x272822, .fg = 0xf8f8f2, .sel_bg = 0x49483e, .line_bg = 0x2f302a, .gutter_fg = 0x75715e, .gutter_bg = 0x272822, .gutter_cur = 0xc2c2b0,
      .ui_bg = 0x1e1f1c, .ui_fg = 0xd6d6c8, .ui_dim = 0x75715e, .accent = 0xa6e22e, .tab_on_bg = 0x272822, .tab_off_bg = 0x34352f, .status_bg = 0x66851a, .status_fg = 0xffffff,
      .border = 0x3a3b34, .menu_bg = 0x34352f, .menu_sel = 0x4d6b12, .popup_bg = 0x34352f, .popup_sel = 0x49483e, .error = 0xf92672, .warn = 0xe6db74, .info = 0x66d9ef, .ok = 0xa6e22e,
      .match_bg = 0x49483e, .find_bg = 0x6b5b00, .find_cur_bg = 0xa38400, .bracket_bg = 0x5a594a, .guide = 0x3a3b34, .occurrence_bg = 0x35362f,
      .hl = { [HC_TEXT] = 0xf8f8f2, [HC_COMMENT] = 0x75715e, [HC_DOC] = 0x8c8870, [HC_TODO] = 0xfd971f, [HC_STRING] = 0xe6db74, [HC_ESCAPE] = 0xae81ff, [HC_CHAR] = 0xe6db74,
              [HC_INTERP] = 0xfd971f, [HC_NUMBER] = 0xae81ff, [HC_KEYWORD] = 0xf92672, [HC_DECL] = 0x66d9ef, [HC_TYPE] = 0x66d9ef, [HC_TYPEDEF] = 0xa6e22e,
              [HC_BUILTIN] = 0x66d9ef, [HC_FUNC] = 0xa6e22e, [HC_FUNCDEF] = 0xa6e22e, [HC_CONST] = 0xae81ff, [HC_OPERATOR] = 0xf92672, [HC_PUNCT] = 0xf8f8f2,
              [HC_MODULE] = 0xfd971f, [HC_FIELD] = 0xf8f8f2, [HC_LABEL] = 0xfd971f, [HC_SELF] = 0xfd971f, [HC_NOTE] = 0xa6e22e, [HC_DURATION] = 0xfd971f, [HC_CHORD] = 0xf92672, [HC_TAG] = 0xf92672, [HC_ATTR] = 0xa6e22e, [HC_ENTITY] = 0xae81ff },
      .hl_attr = { [HC_COMMENT] = ATTR_ITALIC, [HC_DOC] = ATTR_ITALIC, [HC_TODO] = ATTR_BOLD, [HC_TYPE] = ATTR_ITALIC, [HC_SELF] = ATTR_ITALIC, [HC_CHORD] = ATTR_BOLD } },
    { .name = "Solarized Dark", .bg = 0x002b36, .fg = 0x93a1a1, .sel_bg = 0x0b4b5c, .line_bg = 0x073642, .gutter_fg = 0x586e75, .gutter_bg = 0x002b36, .gutter_cur = 0x93a1a1,
      .ui_bg = 0x00212b, .ui_fg = 0x93a1a1, .ui_dim = 0x586e75, .accent = 0x859900, .tab_on_bg = 0x002b36, .tab_off_bg = 0x073642, .status_bg = 0x268bd2, .status_fg = 0xfdf6e3,
      .border = 0x0f4350, .menu_bg = 0x073642, .menu_sel = 0x0b5a6e, .popup_bg = 0x073642, .popup_sel = 0x0b5a6e, .error = 0xdc322f, .warn = 0xb58900, .info = 0x268bd2, .ok = 0x859900,
      .match_bg = 0x0b4b5c, .find_bg = 0x5b4b00, .find_cur_bg = 0x8a7000, .bracket_bg = 0x15667a, .guide = 0x0c3a46, .occurrence_bg = 0x053a46,
      .hl = { [HC_TEXT] = 0x93a1a1, [HC_COMMENT] = 0x586e75, [HC_DOC] = 0x6c8088, [HC_TODO] = 0xcb4b16, [HC_STRING] = 0x2aa198, [HC_ESCAPE] = 0xd33682, [HC_CHAR] = 0x2aa198,
              [HC_INTERP] = 0xcb4b16, [HC_NUMBER] = 0xd33682, [HC_KEYWORD] = 0x859900, [HC_DECL] = 0x859900, [HC_TYPE] = 0xb58900, [HC_TYPEDEF] = 0xb58900,
              [HC_BUILTIN] = 0xcb4b16, [HC_FUNC] = 0x268bd2, [HC_FUNCDEF] = 0x268bd2, [HC_CONST] = 0xcb4b16, [HC_OPERATOR] = 0x859900, [HC_PUNCT] = 0x839496,
              [HC_MODULE] = 0x6c71c4, [HC_FIELD] = 0x93a1a1, [HC_LABEL] = 0xcb4b16, [HC_SELF] = 0xd33682, [HC_NOTE] = 0x2aa198, [HC_DURATION] = 0xcb4b16, [HC_CHORD] = 0xd33682, [HC_TAG] = 0x268bd2, [HC_ATTR] = 0x93a1a1, [HC_ENTITY] = 0xd33682 },
      .hl_attr = { [HC_COMMENT] = ATTR_ITALIC, [HC_DOC] = ATTR_ITALIC, [HC_TODO] = ATTR_BOLD, [HC_DECL] = ATTR_BOLD } },
    { .name = "High Contrast", .bg = 0x000000, .fg = 0xffffff, .sel_bg = 0x1d3f8c, .line_bg = 0x14141a, .gutter_fg = 0x9a9a9a, .gutter_bg = 0x000000, .gutter_cur = 0xffffff,
      .ui_bg = 0x0a0a0a, .ui_fg = 0xffffff, .ui_dim = 0xa0a0a0, .accent = 0x00ff7f, .tab_on_bg = 0x000000, .tab_off_bg = 0x1c1c1c, .status_bg = 0x005f2f, .status_fg = 0xffffff,
      .border = 0x6fc3df, .menu_bg = 0x1c1c1c, .menu_sel = 0x005f2f, .popup_bg = 0x1c1c1c, .popup_sel = 0x1d3f8c, .error = 0xff5555, .warn = 0xffff55, .info = 0x55ffff, .ok = 0x55ff55,
      .match_bg = 0x303060, .find_bg = 0x6b5000, .find_cur_bg = 0xb08000, .bracket_bg = 0x4040a0, .guide = 0x2a2a2a, .occurrence_bg = 0x202030,
      .hl = { [HC_TEXT] = 0xffffff, [HC_COMMENT] = 0x7ca668, [HC_DOC] = 0x8fbf7a, [HC_TODO] = 0xffaa00, [HC_STRING] = 0xce9178, [HC_ESCAPE] = 0xd7ba7d, [HC_CHAR] = 0xce9178,
              [HC_INTERP] = 0xffd700, [HC_NUMBER] = 0xb5cea8, [HC_KEYWORD] = 0xc586c0, [HC_DECL] = 0x569cd6, [HC_TYPE] = 0x4ec9b0, [HC_TYPEDEF] = 0x4ec9b0,
              [HC_BUILTIN] = 0x4fc1ff, [HC_FUNC] = 0xdcdcaa, [HC_FUNCDEF] = 0xffff99, [HC_CONST] = 0x4fc1ff, [HC_OPERATOR] = 0xffffff, [HC_PUNCT] = 0xffd700,
              [HC_MODULE] = 0x9cdcfe, [HC_FIELD] = 0x9cdcfe, [HC_LABEL] = 0xffaa00, [HC_SELF] = 0x569cd6, [HC_NOTE] = 0x4ec9b0, [HC_DURATION] = 0xffd700, [HC_CHORD] = 0xff79c6, [HC_TAG] = 0x569cd6, [HC_ATTR] = 0x9cdcfe, [HC_ENTITY] = 0xd7ba7d },
      .hl_attr = { [HC_COMMENT] = ATTR_ITALIC, [HC_TODO] = ATTR_BOLD, [HC_DECL] = ATTR_BOLD, [HC_FUNCDEF] = ATTR_BOLD } },
};
const int N_THEMES = (int)(sizeof THEMES / sizeof *THEMES);
