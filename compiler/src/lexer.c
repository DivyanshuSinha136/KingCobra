#include "lexer.h"
#include <ctype.h>

static const struct { const char *kw; KcTokKind k; } KEYWORDS[] = {
    {"fn", TK_FN}, {"let", TK_LET}, {"var", TK_VAR}, {"if", TK_IF},
    {"else", TK_ELSE}, {"while", TK_WHILE}, {"for", TK_FOR},
    {"return", TK_RETURN}, {"struct", TK_STRUCT}, {"import", TK_IMPORT},
    {"extern", TK_EXTERN}, {"unsafe", TK_UNSAFE}, {"comptime", TK_COMPTIME}, {"break", TK_BREAK},
    {"continue", TK_CONTINUE}, {"true", TK_TRUE}, {"false", TK_FALSE},
    {"null", TK_NULL}, {"match", TK_MATCH}, {"from", TK_FROM}, {"as", TK_AS}, {"in", TK_IN},
    {NULL, TK_EOF}
};

void kc_lexer_init(KcLexer *lx, const char *src, const char *filename) {
    lx->src = src; lx->cur = src; lx->filename = filename;
    lx->line = 1; lx->col = 1; lx->doc = NULL;
}

static int peekc(KcLexer *lx) { return *lx->cur; }
static int peekc2(KcLexer *lx) { return lx->cur[0] ? lx->cur[1] : 0; }

static int advc(KcLexer *lx) {
    int c = *lx->cur++;
    if (c == '\n') { lx->line++; lx->col = 1; } else { lx->col++; }
    return c;
}

static KcPos here(KcLexer *lx) {
    KcPos p = { lx->filename, lx->line, lx->col };
    return p;
}

static void skip_ws_and_comments(KcLexer *lx) {
    for (;;) {
        int c = peekc(lx);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { advc(lx); continue; }
        if (c == '/' && peekc2(lx) == '/') {
            bool is_doc = lx->cur[2] == '/' && lx->cur[3] != '/';          /* `/// text` documents the next declaration */
            const char *from = lx->cur + (is_doc ? 3 : 2);
            while (peekc(lx) && peekc(lx) != '\n') advc(lx);
            if (is_doc) {
                const char *to = lx->cur;
                if (from < to && *from == ' ') from++;
                while (to > from && (to[-1] == '\r' || to[-1] == ' ')) to--;
                size_t old = lx->doc ? strlen(lx->doc) : 0, add = (size_t)(to - from);
                char *nd = kc_malloc(old + add + 2);
                if (old) memcpy(nd, lx->doc, old);
                memcpy(nd + old, from, add); nd[old + add] = '\n'; nd[old + add + 1] = 0;
                lx->doc = nd;
            }
            continue;
        }
        if (c == '/' && peekc2(lx) == '*') {
            KcPos open = here(lx);
            advc(lx); advc(lx);
            while (peekc(lx) && !(peekc(lx) == '*' && peekc2(lx) == '/')) advc(lx);
            if (!peekc(lx)) kc_error_at(open, "unterminated block comment (missing */)");
            advc(lx); advc(lx);
            continue;
        }
        break;
    }
}

static KcToken mktok(KcLexer *lx, KcTokKind k, const char *start, KcPos pos) {
    KcToken t; memset(&t, 0, sizeof t);
    t.kind = k; t.start = start; t.len = (int)(lx->cur - start); t.pos = pos;
    return t;
}

/* append a Unicode code point to buf as UTF-8 */
static void put_utf8(char **buf, size_t *len, size_t *cap, unsigned long cp) {
    char tmp[4]; int n;
    if (cp < 0x80) { tmp[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { tmp[0] = (char)(0xC0 | (cp >> 6)); tmp[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { tmp[0] = (char)(0xE0 | (cp >> 12)); tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    else { tmp[0] = (char)(0xF0 | (cp >> 18)); tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
    if (*len + 5 >= *cap) { *cap *= 2; *buf = kc_realloc(*buf, *cap); }
    memcpy(*buf + *len, tmp, (size_t)n); *len += (size_t)n;
}

static int hexval(int c) { return isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10); }

/* Reads one escape sequence after the backslash. Sets *cp when it denotes a code point
 * (\xHH, \u{...}); otherwise returns the single byte in *byte and *is_cp = false. */
static void read_escape(KcLexer *lx, KcPos pos, int *byte, unsigned long *cp, bool *is_cp) {
    *is_cp = false;
    int e = advc(lx);
    switch (e) {
        case 'n': *byte = '\n'; return;
        case 't': *byte = '\t'; return;
        case 'r': *byte = '\r'; return;
        case '0': *byte = '\0'; return;
        case '\\': *byte = '\\'; return;
        case '"': *byte = '"'; return;
        case '\'': *byte = '\''; return;
        case 'x': {
            if (!isxdigit(peekc(lx)) || !isxdigit(peekc2(lx))) kc_error_at(pos, "\\x needs two hex digits (e.g. \\x41)");
            int v = hexval(advc(lx)) * 16; v += hexval(advc(lx));
            *byte = v; return;                 /* a raw byte, so \xFF can build binary strings */
        }
        case 'u': {
            if (peekc(lx) != '{') kc_error_at(pos, "\\u needs braces, e.g. \\u{1F600}");
            advc(lx);
            unsigned long v = 0; int nd = 0;
            while (isxdigit(peekc(lx))) { v = v * 16 + (unsigned long)hexval(advc(lx)); nd++; if (nd > 6) kc_error_at(pos, "\\u{...} has too many digits"); }
            if (peekc(lx) != '}' || nd == 0) kc_error_at(pos, "malformed \\u{...} escape");
            advc(lx);
            if (v > 0x10FFFF) kc_error_at(pos, "\\u{...} is not a valid Unicode code point");
            *cp = v; *is_cp = true; return;
        }
        default: *byte = e; return;            /* unknown escapes stay lenient: \q is q */
    }
}

static KcToken lex_string(KcLexer *lx, KcPos pos) {
    const char *start = lx->cur - 1;
    size_t cap = 16, len = 0;
    char *buf = kc_malloc(cap);
    while (peekc(lx) && peekc(lx) != '"') {
        int c = advc(lx);
        if (c == '\\') {
            int byte = 0; unsigned long cp = 0; bool is_cp = false;
            read_escape(lx, pos, &byte, &cp, &is_cp);
            if (is_cp) { put_utf8(&buf, &len, &cap, cp); continue; }
            c = byte;
        }
        if (len + 1 >= cap) { cap *= 2; buf = kc_realloc(buf, cap); }
        buf[len++] = (char)c;
    }
    if (peekc(lx) != '"') kc_error_at(pos, "unterminated string literal (missing closing \")");
    advc(lx);
    buf[len] = 0;
    KcToken t = mktok(lx, TK_STRING, start, pos);
    t.str = buf;
    return t;
}

/* f"text {expr} more": returns the RAW body (the parser splits and unescapes it). The scan
 * only has to find the real end quote, so it tracks {…} depth and skips string literals that
 * appear inside an embedded expression. */
static KcToken lex_fstring(KcLexer *lx, KcPos pos, const char *start) {
    size_t cap = 32, len = 0;
    char *buf = kc_malloc(cap);
    #define PUSH(ch) do { if (len + 2 >= cap) { cap *= 2; buf = kc_realloc(buf, cap); } buf[len++] = (char)(ch); } while (0)
    int depth = 0;                                 /* > 0 while inside {expression} */
    for (;;) {
        int c = peekc(lx);
        if (!c) kc_error_at(pos, "unterminated f-string (missing closing \")");
        if (depth == 0) {
            if (c == '"') break;
            if (c == '\\') { PUSH(advc(lx)); if (peekc(lx)) PUSH(advc(lx)); continue; }
            if (c == '{' && peekc2(lx) == '{') { PUSH(advc(lx)); PUSH(advc(lx)); continue; }   /* {{ literal brace */
            if (c == '}' && peekc2(lx) == '}') { PUSH(advc(lx)); PUSH(advc(lx)); continue; }   /* }} literal brace */
            if (c == '{') depth = 1;
            PUSH(advc(lx));
            continue;
        }
        if (c == '"') {                            /* a string literal inside the expression: copy it whole */
            PUSH(advc(lx));
            while (peekc(lx) && peekc(lx) != '"') {
                if (peekc(lx) == '\\') PUSH(advc(lx));
                if (!peekc(lx)) break;
                PUSH(advc(lx));
            }
            if (!peekc(lx)) kc_error_at(pos, "unterminated string inside an f-string expression (is a '{' missing its '}'?)");
            PUSH(advc(lx));
            continue;
        }
        if (c == '{') depth++;
        else if (c == '}') depth--;
        PUSH(advc(lx));
    }
    #undef PUSH
    advc(lx);                                      /* closing quote */
    buf[len] = 0;
    KcToken t = mktok(lx, TK_FSTRING, start, pos);
    t.str = buf;
    return t;
}

/* 'a' '\n' '\x41' '\u{e9}' 'é' -> an int holding the Unicode code point */
static KcToken lex_char(KcLexer *lx, KcPos pos) {
    const char *start = lx->cur - 1;
    unsigned long cp = 0;
    int c = peekc(lx);
    if (!c || c == '\n') kc_error_at(pos, "unterminated character literal");
    if (c == '\'') kc_error_at(pos, "empty character literal ('')");
    if (c == '\\') {
        advc(lx);
        int byte = 0; bool is_cp = false;
        read_escape(lx, pos, &byte, &cp, &is_cp);
        if (!is_cp) cp = (unsigned long)(unsigned char)byte;
    } else {
        unsigned char b = (unsigned char)advc(lx);          /* decode one UTF-8 sequence */
        int extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0;
        cp = extra == 3 ? (b & 0x07) : extra == 2 ? (b & 0x0F) : extra == 1 ? (b & 0x1F) : b;
        for (int i = 0; i < extra; i++) {
            if ((peekc(lx) & 0xC0) != 0x80) kc_error_at(pos, "invalid UTF-8 in character literal");
            cp = (cp << 6) | ((unsigned long)advc(lx) & 0x3F);
        }
    }
    if (peekc(lx) != '\'') {
        /* C / B multi-character constant: 'ab' packs its bytes big-endian into one int (up to 8). */
        if (cp > 0xFF) kc_error_at(pos, "a character literal holds exactly one character (use \"...\" for strings)");
        int nb = 1;
        while (peekc(lx) && peekc(lx) != '\'' && peekc(lx) != '\n') {
            int byte;
            if (peekc(lx) == '\\') {
                advc(lx);
                unsigned long cp2 = 0; bool is_cp2 = false; byte = 0;
                read_escape(lx, pos, &byte, &cp2, &is_cp2);
                if (is_cp2) byte = (int)(cp2 & 0xFF);
            } else byte = advc(lx);
            if (++nb > 8) kc_error_at(pos, "a multi-character constant holds at most 8 characters");
            cp = (cp << 8) | ((unsigned long)byte & 0xFF);
        }
        if (peekc(lx) != '\'') kc_error_at(pos, "unterminated character literal");
    }
    advc(lx);
    KcToken t = mktok(lx, TK_INT, start, pos);
    t.ival = (int64_t)cp; t.num = (double)cp;
    return t;
}

/* Integers are parsed into an exact 64-bit value (no detour through double), `_` may separate
 * digits (1_000_000), and floats accept exponents (1e9, 2.5E-3). */
static KcToken lex_number(KcLexer *lx, KcPos pos) {
    const char *start = lx->cur - 1;
    int first = start[0];

    if (first == '0' && (peekc(lx) == 'x' || peekc(lx) == 'X' || peekc(lx) == 'b' || peekc(lx) == 'B' || peekc(lx) == 'o' || peekc(lx) == 'O') &&
        (isxdigit((unsigned char)peekc2(lx)) || peekc2(lx) == '_')) {
        int base = (tolower(peekc(lx)) == 'x') ? 16 : (tolower(peekc(lx)) == 'b') ? 2 : 8;
        advc(lx);
        uint64_t v = 0; int nd = 0;
        for (;;) {
            int c = peekc(lx);
            if (c == '_') { advc(lx); continue; }
            if (!isxdigit(c)) break;
            int d = hexval(c);
            if (d >= base) { if (base == 16) break; kc_error_at(pos, "invalid digit '%c' in base-%d literal", c, base); }
            advc(lx);
            if (v >> (64 - (base == 16 ? 4 : base == 8 ? 3 : 1))) kc_error_at(pos, "integer literal does not fit in 64 bits");
            v = v * (uint64_t)base + (uint64_t)d; nd++;
        }
        if (nd == 0) kc_error_at(pos, "expected digits after the base prefix");
        KcToken t = mktok(lx, TK_INT, start, pos);
        t.ival = (int64_t)v;                  /* 0xFFFFFFFFFFFFFFFF is the bit pattern -1, as in C */
        t.num = (double)t.ival;
        return t;
    }

    char tmp[128]; int tn = 0;
    tmp[tn++] = (char)first;
    uint64_t mag = (uint64_t)(first - '0');
    bool overflow = false;
    #define TAKE_DIGITS() while (isdigit((unsigned char)peekc(lx)) || (peekc(lx) == '_' && isdigit((unsigned char)peekc2(lx)))) { \
        int c_ = advc(lx); if (c_ == '_') continue; \
        if (tn < (int)sizeof tmp - 2) tmp[tn++] = (char)c_; \
        if (mag > (UINT64_MAX - (uint64_t)(c_ - '0')) / 10) overflow = true; else mag = mag * 10 + (uint64_t)(c_ - '0'); }
    TAKE_DIGITS();
    bool isf = false;
    if (peekc(lx) == '.' && isdigit((unsigned char)peekc2(lx))) {
        isf = true; tmp[tn++] = (char)advc(lx);
        while (isdigit((unsigned char)peekc(lx)) || (peekc(lx) == '_' && isdigit((unsigned char)peekc2(lx)))) {
            int c = advc(lx); if (c == '_') continue; if (tn < (int)sizeof tmp - 2) tmp[tn++] = (char)c;
        }
    }
    if ((peekc(lx) == 'e' || peekc(lx) == 'E') &&
        (isdigit((unsigned char)peekc2(lx)) || ((peekc2(lx) == '+' || peekc2(lx) == '-') && isdigit((unsigned char)lx->cur[2])))) {
        isf = true; tmp[tn++] = (char)advc(lx);
        if (peekc(lx) == '+' || peekc(lx) == '-') tmp[tn++] = (char)advc(lx);
        while (isdigit((unsigned char)peekc(lx))) { int c = advc(lx); if (tn < (int)sizeof tmp - 2) tmp[tn++] = (char)c; }
    }
    #undef TAKE_DIGITS
    tmp[tn] = 0;
    if (isf) {
        KcToken t = mktok(lx, TK_FLOAT, start, pos);
        t.num = strtod(tmp, NULL);
        return t;
    }
    KcToken t = mktok(lx, TK_INT, start, pos);
    if (overflow || mag > (uint64_t)INT64_MAX + 1u)
        kc_error_at(pos, "integer literal is too large for a 64-bit int (max 9223372036854775807); write it as a float (e.g. 1e19) if you mean one");
    if (mag == (uint64_t)INT64_MAX + 1u) { t.imin = true; t.ival = INT64_MIN; }
    else t.ival = (int64_t)mag;
    t.num = (double)t.ival;
    return t;
}

static KcToken lex_ident(KcLexer *lx, KcPos pos) {
    const char *start = lx->cur - 1;
    while (isalnum((unsigned char)peekc(lx)) || peekc(lx) == '_') advc(lx);
    int len = (int)(lx->cur - start);
    if (len == 1 && start[0] == 'f' && peekc(lx) == '"') { advc(lx); return lex_fstring(lx, pos, start); }
    for (int i = 0; KEYWORDS[i].kw; i++) {
        if ((int)strlen(KEYWORDS[i].kw) == len && memcmp(KEYWORDS[i].kw, start, len) == 0)
            return mktok(lx, KEYWORDS[i].k, start, pos);
    }
    return mktok(lx, TK_IDENT, start, pos);
}

static KcToken lex_one(KcLexer *lx) {
    skip_ws_and_comments(lx);
    KcPos pos = here(lx);
    if (!peekc(lx)) return mktok(lx, TK_EOF, lx->cur, pos);
    const char *start0 = lx->cur;
    int c = advc(lx);

    if (isdigit((unsigned char)c)) return lex_number(lx, pos);
    if (isalpha((unsigned char)c) || c == '_') return lex_ident(lx, pos);
    if (c == '"') return lex_string(lx, pos);
    if (c == '\'') return lex_char(lx, pos);

    switch (c) {
        case '(': return mktok(lx, TK_LPAREN, start0, pos);
        case ')': return mktok(lx, TK_RPAREN, start0, pos);
        case '{': return mktok(lx, TK_LBRACE, start0, pos);
        case '}': return mktok(lx, TK_RBRACE, start0, pos);
        case '[': return mktok(lx, TK_LBRACKET, start0, pos);
        case ']': return mktok(lx, TK_RBRACKET, start0, pos);
        case ',': return mktok(lx, TK_COMMA, start0, pos);
        case ';': return mktok(lx, TK_SEMI, start0, pos);
        case '.':
            if (peekc(lx) == '.') {
                advc(lx);
                if (peekc(lx) == '.') { advc(lx); return mktok(lx, TK_ELLIPSIS, start0, pos); }
                return mktok(lx, TK_DOTDOT, start0, pos);
            }
            return mktok(lx, TK_DOT, start0, pos);
        case '?':
            if (peekc(lx) == '?') { advc(lx); return mktok(lx, TK_QQ, start0, pos); }
            return mktok(lx, TK_QUESTION, start0, pos);
        case ':': return mktok(lx, TK_COLON, start0, pos);
        case '+':
            if (peekc(lx) == '+') { advc(lx); return mktok(lx, TK_INCR, start0, pos); }
            if (peekc(lx) == '=') { advc(lx); return mktok(lx, TK_PLUSEQ, start0, pos); }
            return mktok(lx, TK_PLUS, start0, pos);
        case '-':
            if (peekc(lx) == '>') { advc(lx); return mktok(lx, TK_ARROW, start0, pos); }
            if (peekc(lx) == '-') { advc(lx); return mktok(lx, TK_DECR, start0, pos); }
            if (peekc(lx) == '=') { advc(lx); return mktok(lx, TK_MINUSEQ, start0, pos); }
            return mktok(lx, TK_MINUS, start0, pos);
        case '*':
            if (peekc(lx) == '=' && peekc2(lx) != '=') { advc(lx); return mktok(lx, TK_STAREQ, start0, pos); }
            return mktok(lx, TK_STAR, start0, pos);
        case '/':
            if (peekc(lx) == '=') { advc(lx); return mktok(lx, TK_SLASHEQ, start0, pos); }
            return mktok(lx, TK_SLASH, start0, pos);
        case '%':
            if (peekc(lx) == '=') { advc(lx); return mktok(lx, TK_PERCENTEQ, start0, pos); }
            return mktok(lx, TK_PERCENT, start0, pos);
        case '=':
            if (peekc(lx) == '=') { advc(lx); return mktok(lx, TK_EQ, start0, pos); }
            if (peekc(lx) == '>') { advc(lx); return mktok(lx, TK_FATARROW, start0, pos); }
            return mktok(lx, TK_ASSIGN, start0, pos);
        case '!':
            if (peekc(lx) == '=') { advc(lx); return mktok(lx, TK_NEQ, start0, pos); }
            return mktok(lx, TK_NOT, start0, pos);
        case '<':
            if (peekc(lx) == '<') {
                advc(lx);
                if (peekc(lx) == '=') { advc(lx); return mktok(lx, TK_SHLEQ, start0, pos); }
                return mktok(lx, TK_SHL, start0, pos);
            }
            if (peekc(lx) == '=') { advc(lx); return mktok(lx, TK_LE, start0, pos); }
            return mktok(lx, TK_LT, start0, pos);
        case '>':
            if (peekc(lx) == '>') {
                advc(lx);
                if (peekc(lx) == '>') { advc(lx); return mktok(lx, TK_USHR, start0, pos); }
                if (peekc(lx) == '=') { advc(lx); return mktok(lx, TK_SHREQ, start0, pos); }
                return mktok(lx, TK_SHR, start0, pos);
            }
            if (peekc(lx) == '=') { advc(lx); return mktok(lx, TK_GE, start0, pos); }
            return mktok(lx, TK_GT, start0, pos);
        case '&':
            if (peekc(lx) == '&') { advc(lx); return mktok(lx, TK_AND, start0, pos); }
            if (peekc(lx) == '=') { advc(lx); return mktok(lx, TK_AMPEQ, start0, pos); }
            return mktok(lx, TK_AMP, start0, pos);
        case '|':
            if (peekc(lx) == '|') { advc(lx); return mktok(lx, TK_OR, start0, pos); }
            if (peekc(lx) == '=') { advc(lx); return mktok(lx, TK_PIPEEQ, start0, pos); }
            return mktok(lx, TK_PIPE, start0, pos);
        case '^':
            if (peekc(lx) == '=') { advc(lx); return mktok(lx, TK_CARETEQ, start0, pos); }
            return mktok(lx, TK_CARET, start0, pos);
        case '~': return mktok(lx, TK_TILDE, start0, pos);
    }
    KcToken t = mktok(lx, TK_ERROR, start0, pos);
    return t;
}

KcToken kc_lexer_next(KcLexer *lx) {
    lx->doc = NULL;                      /* `///` lines count only when they directly precede THIS token */
    KcToken t = lex_one(lx);
    t.doc = lx->doc; lx->doc = NULL;
    return t;
}

const char *kc_tok_name(KcTokKind k) {
    switch (k) {
        case TK_EOF: return "eof"; case TK_ERROR: return "error";
        case TK_INT: return "int"; case TK_FLOAT: return "float";
        case TK_STRING: return "string"; case TK_IDENT: return "ident";
        case TK_FN: return "fn"; case TK_LET: return "let"; case TK_VAR: return "var";
        case TK_IF: return "if"; case TK_ELSE: return "else"; case TK_WHILE: return "while";
        case TK_FOR: return "for"; case TK_RETURN: return "return";
        case TK_LBRACE: return "{"; case TK_RBRACE: return "}";
        case TK_LPAREN: return "("; case TK_RPAREN: return ")";
        case TK_SEMI: return ";"; case TK_COMMA: return ",";
        default: return "<tok>";
    }
}
