#ifndef KC_LEXER_H
#define KC_LEXER_H
#include "kc_common.h"

typedef enum {
    TK_EOF, TK_ERROR,
    /* literals */
    TK_INT, TK_FLOAT, TK_STRING, TK_IDENT, TK_TRUE, TK_FALSE,
    /* keywords */
    TK_FN, TK_LET, TK_VAR, TK_IF, TK_ELSE, TK_WHILE, TK_FOR, TK_RETURN,
    TK_STRUCT, TK_IMPORT, TK_EXTERN, TK_UNSAFE, TK_BREAK, TK_CONTINUE, TK_COMPTIME,
    TK_TRUE_TYPE, TK_NULL, TK_MATCH, TK_FROM, TK_AS, TK_IN,
    /* punctuation */
    TK_LPAREN, TK_RPAREN, TK_LBRACE, TK_RBRACE, TK_LBRACKET, TK_RBRACKET,
    TK_COMMA, TK_SEMI, TK_COLON, TK_ARROW, TK_FATARROW, TK_DOT, TK_DOTDOT,
    /* operators */
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_ASSIGN, TK_EQ, TK_NEQ, TK_LT, TK_LE, TK_GT, TK_GE,
    TK_AND, TK_OR, TK_NOT, TK_PLUSEQ, TK_MINUSEQ,
    /* bitwise / pointer operators (v0.5) */
    TK_AMP, TK_PIPE, TK_CARET, TK_TILDE, TK_SHL, TK_SHR, TK_USHR,
    TK_AMPEQ, TK_PIPEEQ, TK_CARETEQ, TK_SHLEQ, TK_SHREQ,
    /* v0.7 */
    TK_FSTRING,                          /* f"..." : raw body, split by the parser */
    TK_QUESTION, TK_QQ, TK_ELLIPSIS,     /* ?  ??  ... */
    TK_STAREQ, TK_SLASHEQ, TK_PERCENTEQ, /* *= /= %= */
    TK_INCR, TK_DECR,                    /* ++ -- (v0.8) */
} KcTokKind;

typedef struct {
    KcTokKind kind;
    const char *start;   /* pointer into source, not null-terminated */
    int len;
    KcPos pos;
    /* literal values */
    double num;
    int64_t ival;        /* exact value of a TK_INT (and of a character literal) */
    bool imin;           /* the literal 9223372036854775808: only valid as -9223372036854775808 */
    char *str;           /* owned, unescaped, for TK_STRING; raw body for TK_FSTRING */
    const char *doc;     /* the `///` comment lines directly before this token (for help()), or NULL */
} KcToken;

typedef struct {
    const char *src;
    const char *cur;
    const char *filename;
    int line, col;
    const char *doc;     /* `///` lines seen since the last token (never freed or resized in place: parser look-ahead copies the lexer) */
} KcLexer;

void kc_lexer_init(KcLexer *lx, const char *src, const char *filename);
KcToken kc_lexer_next(KcLexer *lx);
const char *kc_tok_name(KcTokKind k);

#endif
