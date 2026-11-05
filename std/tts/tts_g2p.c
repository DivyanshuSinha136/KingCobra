/* KingCobra std.tts — grapheme-to-phoneme conversion.
 *
 * Pipeline for one word (first hit wins):
 *   homograph disambiguation (read/lead/live/record/present/... by neighbouring words)
 *   -> user lexicon -> built-in dictionary -> "the" before vowels
 *   -> possessives and contractions -> morphology (plural/-ed/-ing/-er/-est/-ly/-ness/... and prefixes on known stems)
 *   -> compound splitting -> acronym spelling -> letter-to-sound rules (NRL-style context rules) + stress assignment.
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#include "tts_internal.h"
#include <ctype.h>

/* ================================================================== lexicon tables */
typedef struct Ent { char *w; TtsPh *ph; int n; struct Ent *next; } Ent;
typedef struct { Ent **t; int cap, cnt; } Lex;
static Lex L_DICT, L_USER;
static bool g_init = false;

static uint32_t hash_w(const char *s) { uint32_t h = 2166136261u; while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; } return h; }
static void lex_init(Lex *L, int cap) { L->cap = cap; L->cnt = 0; L->t = calloc((size_t)cap, sizeof(Ent *)); }
static Ent *lex_get(Lex *L, const char *w) {
    if (!L->t) return NULL;
    for (Ent *e = L->t[hash_w(w) % (uint32_t)L->cap]; e; e = e->next) if (!strcmp(e->w, w)) return e;
    return NULL;
}
static void lex_grow(Lex *L) {
    int nc = L->cap * 2; Ent **nt = calloc((size_t)nc, sizeof(Ent *));
    for (int i = 0; i < L->cap; i++) for (Ent *e = L->t[i], *nx; e; e = nx) { nx = e->next; uint32_t h = hash_w(e->w) % (uint32_t)nc; e->next = nt[h]; nt[h] = e; }
    free(L->t); L->t = nt; L->cap = nc;
}
static void lex_put(Lex *L, const char *w, const TtsPh *ph, int n) {
    if (!L->t) lex_init(L, 1024);
    Ent *e = lex_get(L, w);
    if (e) { free(e->ph); }
    else {
        if (L->cnt > L->cap * 2) lex_grow(L);
        e = calloc(1, sizeof *e); e->w = strdup(w);
        uint32_t h = hash_w(w) % (uint32_t)L->cap; e->next = L->t[h]; L->t[h] = e; L->cnt++;
    }
    e->ph = malloc((size_t)(n ? n : 1) * sizeof(TtsPh)); memcpy(e->ph, ph, (size_t)n * sizeof(TtsPh)); e->n = n;
}
static bool lex_del(Lex *L, const char *w) {
    if (!L->t) return false;
    Ent **pp = &L->t[hash_w(w) % (uint32_t)L->cap];
    for (; *pp; pp = &(*pp)->next) if (!strcmp((*pp)->w, w)) { Ent *e = *pp; *pp = e->next; free(e->w); free(e->ph); free(e); L->cnt--; return true; }
    return false;
}
static void lex_clear(Lex *L) {
    if (!L->t) return;
    for (int i = 0; i < L->cap; i++) for (Ent *e = L->t[i], *nx; e; e = nx) { nx = e->next; free(e->w); free(e->ph); free(e); }
    memset(L->t, 0, (size_t)L->cap * sizeof(Ent *)); L->cnt = 0;
}

/* ---- text substitutions (tts.add_sub) ---- */
typedef struct { char *from, *to; } Sub;
static Sub *SUBS; static int NSUBS, CAPSUBS;
void tts_add_sub(const char *from, const char *to) {
    char *k = strdup(from); for (char *p = k; *p; p++) *p = (char)tolower((unsigned char)*p);
    for (int i = 0; i < NSUBS; i++) if (!strcmp(SUBS[i].from, k)) { free(SUBS[i].to); SUBS[i].to = strdup(to); free(k); return; }
    if (NSUBS == CAPSUBS) { CAPSUBS = CAPSUBS ? CAPSUBS * 2 : 16; SUBS = realloc(SUBS, (size_t)CAPSUBS * sizeof(Sub)); }
    SUBS[NSUBS].from = k; SUBS[NSUBS].to = strdup(to); NSUBS++;
}
bool tts_sub_remove(const char *from) {
    char k[128]; snprintf(k, sizeof k, "%s", from); for (char *p = k; *p; p++) *p = (char)tolower((unsigned char)*p);
    for (int i = 0; i < NSUBS; i++) if (!strcmp(SUBS[i].from, k)) { free(SUBS[i].from); free(SUBS[i].to); SUBS[i] = SUBS[--NSUBS]; return true; }
    return false;
}
void tts_sub_clear(void) { for (int i = 0; i < NSUBS; i++) { free(SUBS[i].from); free(SUBS[i].to); } NSUBS = 0; }
const char *tts_sub_lookup(const char *word) { for (int i = 0; i < NSUBS; i++) if (!strcmp(SUBS[i].from, word)) return SUBS[i].to; return NULL; }

/* ================================================================== letter-to-sound rules */
typedef struct { const char *l, *m, *r, *o; } RuleDef;
typedef struct { const char *l, *m, *r; int ml; TtsPh ph[6]; int n; } Rule;
static Rule *RT[26]; static int RN[26];

/* context symbols: # one+ vowels  : zero+ consonants  ^ one consonant  . one voiced consonant  + front vowel (e i y)
 * & sibilant  @ t s r d l z n j th ch sh  % word-final suffix (e er es ed ing ely)  ' ' word boundary; letters are literal */
#define RL(name) static const RuleDef name[]
RL(RA) = {
    {" ", "A", " ", "AH0"}, {" ", "ARE", " ", "AA R"}, {"", "AR", "O", "AH0 R"}, {"", "AR", "#", "EH R"}, {"^", "AS", "#", "EY S"},
    {"", "A", "WA", "AH0"}, {"", "AW", "", "AO"}, {"", "A", "^+#", "EY"}, {"#:", "ALLY", "", "AH0 L IY0"}, {"", "AL", "#", "AH0 L"},
    {"", "AGAIN", "", "AH0 G EH N"}, {"#:", "AG", "E", "IH JH"}, {"", "A", "^%", "EY"}, {"", "A", "^+:#", "AE"}, {" :", "A", "^+ ", "EY"},
    {"", "ARR", "", "AE R"}, {"^", "AR", " ", "AA R"}, {"", "AR", " ", "ER"}, {"", "AR", "", "AA R"}, {"", "AIR", "", "EH R"},
    {"", "AI", "", "EY"}, {"", "AY", "", "EY"}, {"", "AU", "", "AO"}, {"#:", "AL", " ", "AH0 L"}, {"#:", "ALS", " ", "AH0 L Z"},
    {"", "ALK", "", "AO K"}, {"", "AL", "^", "AO L"}, {" :", "ABLE", "", "EY B AH0 L"}, {"", "ABLE", "", "AH0 B AH0 L"}, {"", "ANG", "+", "EY N JH"},
    {" ", "A", "BO", "AH0"}, {" ", "A", "BR", "AH0"}, {" ", "A", "V#", "AH0"}, {" ", "A", "L#", "AH0"}, {" ", "A", "M#", "AH0"}, {" ", "A", "BS", "AE"},
    {"", "A", "", "AE"}, {0}
};
RL(RB) = { {"", "B", "B", ""}, {"M", "B", " ", ""}, {"E", "B", "T", ""}, {"U", "B", "T", ""}, {"", "B", "", "B"}, {0} };
RL(RC) = {
    {"", "CH", "^", "K"}, {"^E", "CH", "", "K"}, {"", "TCH", "", "CH"}, {"", "CH", "", "CH"}, {"S", "CI", "#", "S AY"}, {"", "CI", "A", "SH"}, {"", "CI", "O", "SH"},
    {"", "CI", "EN", "SH"}, {"", "C", "+", "S"}, {"", "CK", "", "K"}, {"", "COM", "%", "K AH M"}, {"", "CC", "+", "K S"}, {"", "CC", "", "K"}, {"", "C", "", "K"}, {0}
};
RL(RD) = {
    {"#:", "DED", " ", "D IH0 D"}, {".E", "D", " ", "D"}, {"#^:E", "D", " ", "T"}, {"", "DG", "", "JH"}, {"", "D", "D", ""}, {"", "D", "", "D"}, {0}
};
RL(RE) = {
    {"#:", "E", " ", ""}, {"'^:", "E", " ", ""}, {" :", "E", " ", "IY"}, {"#", "ED", " ", "D"}, {"#:", "E", "D ", ""}, {"", "EV", "ER", "EH V"}, {"", "E", "^%", "IY"},
    {"", "ERR", "", "EH R"}, {"", "ERI", "#", "IY R IY"}, {"", "ERI", "", "EH R IH"}, {"#:", "ER", "#", "ER"}, {"", "ER", "#", "EH R"}, {"", "ER", "", "ER"}, {" ", "EVEN", "", "IY V EH N"},
    {"#:", "E", "W", ""}, {"@", "EW", "", "UW"}, {"", "EW", "", "Y UW"}, {"", "E", "O", "IY"}, {"#:&", "ES", " ", "IH Z"}, {"#:", "E", "S ", ""}, {"#:", "ELY", " ", "L IY0"},
    {"#:", "EMENT", "", "M EH N T"}, {"", "EFUL", "", "F UH L"}, {"", "EER", " ", "IH R"}, {"", "EE", "", "IY"}, {"", "EARN", "", "ER N"}, {" ", "EAR", "^", "ER"}, {"", "EAD", "", "EH D"},
    {"#:", "EA", " ", "IY AH0"}, {"", "EA", "SU", "EH"}, {"", "EA", "", "IY"}, {"", "EIGH", "", "EY"}, {"", "EI", "", "IY"}, {" ", "EYE", "", "AY"}, {"", "EY", "", "IY"},
    {"", "EU", "", "Y UW"}, {"", "E", "", "EH"}, {0}
};
RL(RF) = { {"#:", "FUL", " ", "F AH0 L"}, {"", "F", "F", ""}, {"", "F", "", "F"}, {0} };
RL(RG) = {
    {" ", "GH", "", "G"}, {" ", "G", "N", ""}, {"", "GIV", "", "G IH V"}, {"", "G", "I^", "G"}, {"", "GE", "T", "G EH"}, {"SU", "GGES", "", "G JH EH S"}, {"", "GG", "", "G"}, {"B#", "G", "", "G"},
    {"", "G", "+", "JH"}, {"", "GREAT", "", "G R EY T"}, {"#", "GH", "", ""}, {"", "GN", " ", "N"}, {"", "G", "", "G"}, {0}
};
RL(RH) = { {"", "HAV", "", "HH AE V"}, {"", "HERE", "", "HH IY R"}, {"", "HOUR", "", "AW ER"}, {"", "HOW", "", "HH AW"}, {"R", "H", "", ""}, {"", "H", "#", "HH"}, {"", "H", "", ""}, {0} };
RL(RI) = {
    {"^", "IN", "D ", "AY N"}, {"^", "IN", "DS ", "AY N"}, {"SEMI", "I", "", "IY"}, {"", "IER", "", "IY ER"}, {"#:R", "IED", " ", "IY D"}, {"^", "IED", " ", "IY D"}, {"", "IED", " ", "AY D"},
    {"", "IEN", "", "IY EH N"}, {"", "IE", "T", "AY EH"}, {"", "IE", " ", "AY"}, {" :", "I", "%", "AY"}, {"", "I", "%", "IY"}, {"", "IE", "", "IY"}, {"", "IDEA", "", "AY D IY AH0"},
    {"", "I", "TY ", "IH"}, {"", "I", "^+:#", "IH"}, {"", "IR", "#", "AY R"}, {"", "IZ", "%", "AY Z"}, {"", "IS", "%", "AY Z"}, {"I^", "I", "^#", "IH"}, {"+^", "I", "^+", "AY"}, {"#:^", "I", "^+", "IH"},
    {"", "I", "^+", "AY"}, {"", "IRR", "", "IH R"}, {"", "IR", "", "ER"}, {"", "IGH", "", "AY"}, {"", "ILD", "", "AY L D"}, {"", "IGN", " ", "AY N"}, {"", "IGN", "^", "AY N"}, {"", "IGN", "%", "AY N"},
    {"", "IQUE", "", "IY K"}, {"", "I", "", "IH"}, {0}
};
RL(RJ) = { {"", "J", "", "JH"}, {0} };
RL(RK) = { {" ", "K", "N", ""}, {"", "K", "", "K"}, {0} };
RL(RL_) = { {"", "LO", "C#", "L OW"}, {"L", "L", "", ""}, {"#:^", "L", "%", "AH0 L"}, {"", "LEAD", "", "L IY D"}, {"", "L", "", "L"}, {0} };
RL(RM) = { {"", "MOV", "", "M UW V"}, {"", "M", "M", ""}, {"", "M", "", "M"}, {0} };
RL(RN_) = {
    {"", "NG", "ING", "NG"}, {"", "NG", "L%", "NG G"}, {"", "NG", "R", "NG G"}, {"", "NG", "U", "NG G"}, {"", "NG", "+", "N JH"}, {"", "NG", "", "NG"},
    {"", "NK", "", "NG K"}, {" ", "NOW", " ", "N AW"}, {"", "N", "N", ""}, {"", "N", "", "N"}, {0}
};
RL(RO) = {
    {"", "OF", " ", "AH0 V"}, {"", "OROUGH", "", "ER OW"}, {"#:", "OR", " ", "ER"}, {"#:", "ORS", " ", "ER Z"}, {"", "ORR", "", "AO R"}, {"", "OR", "", "AO R"}, {" ", "ONE", "", "W AH N"}, {"", "OWER", "", "AW ER"},
    {"", "OW", "", "OW"}, {"", "OVER", "", "OW V ER"}, {"", "OV", "", "AH V"}, {"", "O", "^%", "OW"}, {"", "O", "^EN", "OW"}, {"", "O", "^I#", "OW"}, {"", "OL", "D", "OW L"},
    {"", "OUGHT", "", "AO T"}, {"", "OUGH", "", "AH F"}, {" ", "OU", "", "AW"}, {"H", "OU", "S#", "AW"}, {"", "OUS", "", "AH0 S"}, {"", "OUR", "", "AO R"}, {"", "OULD", "", "UH D"},
    {"^", "OU", "^L", "AH"}, {"", "OUP", "", "UW P"}, {"", "OU", "", "AW"}, {"", "OY", "", "OY"}, {"", "OING", "", "OW IH NG"}, {"", "OI", "", "OY"}, {"", "OOR", "", "AO R"},
    {"", "OOK", "", "UH K"}, {"", "OOD", "", "UH D"}, {"", "OO", "", "UW"}, {"", "O", "E", "OW"}, {"", "O", " ", "OW"}, {"", "OA", "", "OW"}, {" ", "ONLY", "", "OW N L IY0"},
    {" ", "ONCE", "", "W AH N S"}, {"C", "O", "N", "AA"}, {"", "O", "NG", "AO"}, {" :^", "O", "N", "AH"}, {"I", "ON", "", "AH0 N"}, {"#:", "ON", " ", "AH0 N"}, {"#^", "ON", "", "AH0 N"},
    {"", "O", "ST ", "OW"}, {"", "OF", "^", "AO F"}, {"", "OTHER", "", "AH DH ER"}, {"", "OSS", " ", "AO S"}, {"#:^", "OM", "", "AH M"}, {"", "O", "", "AA"}, {0}
};
RL(RP) = { {"", "PH", "", "F"}, {"", "PEOP", "", "P IY P"}, {"", "POW", "", "P AW"}, {"", "PUT", " ", "P UH T"}, {"", "P", "P", ""}, {" ", "P", "N", ""}, {" ", "P", "S", ""}, {"", "P", "", "P"}, {0} };
RL(RQ) = { {"", "QUAR", "", "K W AO R"}, {"", "QU", "", "K W"}, {"", "Q", "", "K"}, {0} };
RL(RR) = { {" ", "RE", "^#", "R IY"}, {"", "R", "R", ""}, {"", "R", "", "R"}, {0} };
RL(RS) = {
    {"", "SH", "", "SH"}, {"#", "SION", "", "ZH AH0 N"}, {"", "SOME", "", "S AH M"}, {"#", "SUR", "#", "ZH ER"}, {"", "SURE", "", "SH UH R"}, {"#", "SU", "#", "ZH UW"}, {"#", "SSU", "#", "SH UW"},
    {"#", "SED", " ", "Z D"}, {"#", "S", "#", "Z"}, {"", "SAID", "", "S EH D"}, {"^", "SION", "", "SH AH0 N"}, {"", "S", "S", ""}, {".", "S", " ", "Z"}, {"#:.E", "S", " ", "Z"},
    {"#:^#", "S", " ", "S"}, {"U", "S", " ", "S"}, {" :#", "S", " ", "Z"}, {" ", "SCH", "", "S K"}, {"", "S", "C+", ""}, {"#", "SM", "", "Z M"}, {"", "S", "", "S"}, {0}
};
RL(RT_) = {
    {" ", "THE", " ", "DH AH0"}, {" ", "THEY", "", "DH EY"}, {" ", "THERE", "", "DH EH R"}, {"", "THER", "", "DH ER"}, {"", "THEIR", "", "DH EH R"}, {" ", "THAN", " ", "DH AE N"},
    {" ", "THEM", " ", "DH EH M"}, {"", "THESE", " ", "DH IY Z"}, {" ", "THEN", "", "DH EH N"}, {"", "THROUGH", "", "TH R UW"}, {"", "THOSE", "", "DH OW Z"}, {"", "THOUGH", " ", "DH OW"},
    {" ", "THUS", "", "DH AH S"}, {"", "TH", "", "TH"}, {"#:", "TED", " ", "T IH0 D"}, {"S", "TI", "#N", "CH"}, {"", "TI", "O", "SH"}, {"", "TI", "A", "SH"}, {"", "TIEN", "", "SH AH0 N"},
    {"", "TUR", "#", "CH ER"}, {"", "TU", "A", "CH UW"}, {" ", "TWO", "", "T UW"}, {"", "TCH", "", "CH"}, {"", "T", "T", ""}, {"", "T", "", "T"}, {0}
};
RL(RU) = {
    {" ", "UN", "I", "Y UW N"}, {" ", "UN", "", "AH N"}, {" ", "UPON", "", "AH0 P AO N"}, {"@", "UR", "#", "UH R"}, {"", "UR", "#", "Y UH R"}, {"", "URR", "", "ER"}, {"", "UR", "", "ER"}, {"", "U", "^ ", "AH"},
    {"", "U", "^^", "AH"}, {"", "UY", "", "AY"}, {" G", "U", "#", ""}, {"G", "U", "%", ""}, {"G", "U", "#", "W"}, {"#N", "U", "", "Y UW"}, {"@", "U", "", "UW"}, {"", "U", "", "Y UW"}, {0}
};
RL(RV) = { {"", "VIEW", "", "V Y UW"}, {"", "V", "V", ""}, {"", "V", "", "V"}, {0} };
RL(RW) = {
    {" ", "WERE", "", "W ER"}, {"", "WA", "SH", "W AO"}, {"", "WA", "ST", "W EY"}, {"", "WA", "S ", "W AH"}, {"", "WA", "T", "W AA"}, {"", "WHERE", "", "W EH R"}, {"", "WHAT", "", "W AH T"},
    {"", "WHOL", "", "HH OW L"}, {"", "WHO", "", "HH UW"}, {"", "WH", "", "W"}, {"", "WAR", "", "W AO R"}, {"", "WOR", "^", "W ER"}, {"", "WR", "", "R"}, {"", "W", "", "W"}, {0}
};
RL(RX) = { {" ", "X", "", "Z"}, {" E", "X", "#", "G Z"}, {"", "X", "", "K S"}, {0} };
RL(RY) = {
    {"", "YOUNG", "", "Y AH NG"}, {" ", "YOU", "", "Y UW"}, {" ", "YES", "", "Y EH S"}, {" ", "Y", "", "Y"}, {"#:^", "Y", " ", "IY0"}, {"#:^", "Y", "I", "IY0"}, {" :", "Y", " ", "AY"},
    {" :", "Y", "#", "AY"}, {" :", "Y", "^+:#", "IH"}, {" :", "Y", "^#", "AY"}, {"", "Y", "^:%", "AY"}, {"", "Y", "", "IH"}, {0}
};
RL(RZ) = { {"", "Z", "Z", ""}, {"", "Z", "", "Z"}, {0} };

static const RuleDef *const RULE_SRC[26] = { RA, RB, RC, RD, RE, RF, RG, RH, RI, RJ, RK, RL_, RM, RN_, RO, RP, RQ, RR, RS, RT_, RU, RV, RW, RX, RY, RZ };

static const char *const LETTER_PH[26] = {
    "EY1", "B IY1", "S IY1", "D IY1", "IY1", "EH1 F", "JH IY1", "EY1 CH", "AY1", "JH EY1", "K EY1", "EH1 L", "EH1 M",
    "EH1 N", "OW1", "P IY1", "K Y UW1", "AA1 R", "EH1 S", "T IY1", "Y UW1", "V IY1", "D AH1 B AH0 L Y UW0", "EH1 K S", "W AY1", "Z IY1"
};

static void rules_init(void) {
    for (int c = 0; c < 26; c++) {
        int n = 0; while (RULE_SRC[c][n].m) n++;
        RT[c] = calloc((size_t)n + 1, sizeof(Rule)); RN[c] = n;
        for (int i = 0; i < n; i++) {
            const RuleDef *d = &RULE_SRC[c][i]; Rule *r = &RT[c][i];
            r->l = d->l; r->m = d->m; r->r = d->r; r->ml = (int)strlen(d->m);
            r->n = d->o[0] ? tts_phseq_parse_ex(d->o, r->ph, 6, NULL, 0, ST_UNSET) : 0;
            if (r->n < 0) { fprintf(stderr, "std.tts: internal rule error in '%s'\n", d->o); r->n = 0; }
        }
    }
}

static bool vow_c(char c) { return c && strchr("aeiouy", c); }
static bool cons_c(char c) { return c && isalpha((unsigned char)c) && !vow_c(c); }
static bool voiced_c(char c) { return c && strchr("bdgjlmnrvwz", c); }

static bool rmatch(const char *p, const char *s) {
    if (!*p) return true;
    switch (*p) {
    case '#': { int run = 0; while (vow_c(s[run])) run++; for (int k = run; k >= 1; k--) if (rmatch(p + 1, s + k)) return true; return false; }
    case ':': { int run = 0; while (cons_c(s[run])) run++; for (int k = run; k >= 0; k--) if (rmatch(p + 1, s + k)) return true; return false; }
    case '^': return cons_c(*s) && rmatch(p + 1, s + 1);
    case '.': return voiced_c(*s) && rmatch(p + 1, s + 1);
    case '+': return (*s == 'e' || *s == 'i' || *s == 'y') && rmatch(p + 1, s + 1);
    case '&': if ((s[0] == 'c' || s[0] == 's') && s[1] == 'h') { if (rmatch(p + 1, s + 2)) return true; }
              return s[0] && strchr("scgzxj", s[0]) && rmatch(p + 1, s + 1);
    case '@': if ((s[0] == 't' || s[0] == 'c' || s[0] == 's') && s[1] == 'h') { if (rmatch(p + 1, s + 2)) return true; }
              return s[0] && strchr("tsrdlznj", s[0]) && rmatch(p + 1, s + 1);
    case '%': { static const char *const SUF[] = { "ing", "ely", "er", "es", "ed", "e" };
        for (int i = 0; i < 6; i++) { size_t L = strlen(SUF[i]); if (!strncmp(s, SUF[i], L) && s[L] == ' ' && rmatch(p + 1, s + L)) return true; } return false; }
    case ' ': return *s == ' ' && rmatch(p + 1, s + 1);
    default: return *s == (char)tolower((unsigned char)*p) && rmatch(p + 1, s + 1);
    }
}
/* match pattern [ps, pe) leftwards ending just before s (exclusive) */
static bool lmatch(const char *ps, const char *pe, const char *base, const char *s) {
    if (pe == ps) return true;
    char c = pe[-1];
    switch (c) {
    case '#': { int run = 0; while (s - run > base && vow_c(s[-run - 1])) run++; for (int k = run; k >= 1; k--) if (lmatch(ps, pe - 1, base, s - k)) return true; return false; }
    case ':': { int run = 0; while (s - run > base && cons_c(s[-run - 1])) run++; for (int k = run; k >= 0; k--) if (lmatch(ps, pe - 1, base, s - k)) return true; return false; }
    case '^': return s > base && cons_c(s[-1]) && lmatch(ps, pe - 1, base, s - 1);
    case '.': return s > base && voiced_c(s[-1]) && lmatch(ps, pe - 1, base, s - 1);
    case '+': return s > base && (s[-1] == 'e' || s[-1] == 'i' || s[-1] == 'y') && lmatch(ps, pe - 1, base, s - 1);
    case '&': if (s - 1 > base && (s[-2] == 'c' || s[-2] == 's') && s[-1] == 'h' && lmatch(ps, pe - 1, base, s - 2)) return true;
              return s > base && strchr("scgzxj", s[-1]) && lmatch(ps, pe - 1, base, s - 1);
    case '@': if (s - 1 > base && (s[-2] == 't' || s[-2] == 'c' || s[-2] == 's') && s[-1] == 'h' && lmatch(ps, pe - 1, base, s - 2)) return true;
              return s > base && strchr("tsrdlznj", s[-1]) && lmatch(ps, pe - 1, base, s - 1);
    case ' ': return s > base && s[-1] == ' ' && lmatch(ps, pe - 1, base, s - 1);
    default: return s > base && s[-1] == (char)tolower((unsigned char)c) && lmatch(ps, pe - 1, base, s - 1);
    }
}

/* letter-to-sound: phones with ST_UNSET vowels (or explicit stress where a rule says so) */
static int rules_word(const char *word, TtsPh *out, int cap) {
    char buf[130]; size_t L = strlen(word); if (L > 120) L = 120;
    buf[0] = ' '; memcpy(buf + 1, word, L); buf[L + 1] = ' '; buf[L + 2] = 0;
    int n = 0;
    for (int pos = 1; pos <= (int)L; ) {
        char ch = buf[pos];
        if (ch < 'a' || ch > 'z') { pos++; continue; }
        int c = ch - 'a', adv = 1; bool hit = false;
        for (int i = 0; i < RN[c]; i++) {
            const Rule *r = &RT[c][i];
            if (strncmp(buf + pos, r->m, (size_t)r->ml)) { bool ok = true; for (int k = 0; k < r->ml; k++) if (buf[pos + k] != (char)tolower((unsigned char)r->m[k])) { ok = false; break; } if (!ok) continue; }
            if (!rmatch(r->r, buf + pos + r->ml)) continue;
            if (!lmatch(r->l, r->l + strlen(r->l), buf, buf + pos)) continue;
            for (int k = 0; k < r->n && n < cap; k++) out[n++] = r->ph[k];
            adv = r->ml; hit = true; break;
        }
        (void)hit; pos += adv;
    }
    return n;
}

/* ================================================================== stress */
int tts_syllables(const TtsPh *p, int n) { int k = 0; for (int i = 0; i < n; i++) if (tts_is_vowel(p[i].ph)) k++; return k; }
static bool ends_with(const char *w, const char *suf) { size_t a = strlen(w), b = strlen(suf); return a >= b && !strcmp(w + a - b, suf); }
static bool starts_with(const char *w, const char *pre) { return !strncmp(w, pre, strlen(pre)); }

void tts_assign_stress(TtsPh *p, int n, const char *word) {
    int vi[64], k = 0; bool has_primary = false;
    for (int i = 0; i < n && k < 64; i++) if (tts_is_vowel(p[i].ph)) { vi[k++] = i; if (p[i].st == 1) has_primary = true; }
    if (!k) return;
    if (!has_primary) {
        int prim;
        if (k == 1) prim = 0;
        else {
            int suf_syl = 0;
            if (ends_with(word, "tion") || ends_with(word, "sion") || ends_with(word, "cian") || ends_with(word, "tial") || ends_with(word, "cial") || ends_with(word, "ious") || ends_with(word, "eous") || ends_with(word, "ian") || ends_with(word, "ial")) { prim = k - 2; goto done; }
            if (ends_with(word, "eer")) { prim = k - 1; goto done; }
            if (ends_with(word, "ology") || ends_with(word, "ography") || ends_with(word, "ometry") || ends_with(word, "ophy")) { prim = k - 3; if (prim < 0) prim = 0; goto done; }
            if (ends_with(word, "ical") || ends_with(word, "ity") || ends_with(word, "ify") || ends_with(word, "ically")) { prim = k - 3; goto done; }
            if (ends_with(word, "ic") || ends_with(word, "ics")) { prim = k - 2; goto done; }
            if (ends_with(word, "ing") || ends_with(word, "ings") || ends_with(word, "ed") || ends_with(word, "er") || ends_with(word, "ers") || ends_with(word, "ly") || ends_with(word, "ness") || ends_with(word, "less") || ends_with(word, "ment") || ends_with(word, "ful")) suf_syl = 1;
            else if (ends_with(word, "y") && k >= 3) suf_syl = 1;
            int stem = k - suf_syl; if (stem < 1) stem = 1;
            if (stem == 1) prim = 0;
            else if (stem == 2) {
                static const char *const PRE[] = { "be", "de", "re", "ex", "dis", "mis", NULL };
                prim = 0;
                for (int i = 0; PRE[i]; i++) if (starts_with(word, PRE[i]) && strlen(word) >= strlen(PRE[i]) + 3 && cons_c(word[strlen(PRE[i])]) && !starts_with(word, "dist")) { prim = 1; break; }
                if (prim == 0 && p[vi[0]].ph == P_AH && p[vi[0]].st == 0) prim = 1;       /* an explicitly weak first syllable (a-bout, a-way) */
            } else {
                /* Latin stress rule on the stem: heavy penult (long vowel/diphthong or closed) else antepenult */
                int pen = stem - 2, v = p[vi[pen]].ph;
                int after = 0; for (int j = vi[pen] + 1; j < n && !tts_is_vowel(p[j].ph); j++) after++;
                bool heavy = after >= 2 || v == P_AW || v == P_AY || v == P_EY || v == P_OW || v == P_OY || v == P_IY || v == P_UW || v == P_AO || v == P_ER;
                prim = heavy ? pen : stem - 3;
                if (prim < 0) prim = 0;
            }
        }
done:
        if (prim < 0) prim = 0;
        if (prim > k - 1) prim = k - 1;
        if (prim > 63) prim = 63;
        p[vi[prim]].st = 1;
        if (k >= 4) { int s2 = prim - 2; if (s2 < 0 && prim >= 2) s2 = 0; if (s2 >= 0 && p[vi[s2]].st == ST_UNSET) p[vi[s2]].st = 2; }
        int last = p[vi[k - 1]].ph;
        if (k >= 3 && (last == P_EY || last == P_AY) && p[vi[k - 1]].st == ST_UNSET && prim < k - 1) p[vi[k - 1]].st = 2;
    }
    /* unset -> unstressed, and vowel reduction for unstressed vowels */
    for (int j = 0; j < k; j++) {
        TtsPh *x = &p[vi[j]];
        if (x->st == ST_UNSET) x->st = 0;
        if (x->st != 0) continue;
        int next = vi[j] + 1 < n ? p[vi[j] + 1].ph : 0;
        switch (x->ph) {
        case P_AE: case P_AA: case P_AO: x->ph = P_AH; break;
        case P_EH: x->ph = (next == P_S || next == P_Z || next == P_T || next == P_D || next == P_SH || next == P_CH || next == P_JH || next == P_K) ? P_IH : P_AH; break;
        default: break;
        }
    }
}

/* ================================================================== morphology helpers */
#define PCAP 96
static void app(TtsPh *o, int *n, const char *pron) { int m = tts_phseq_parse_ex(pron, o + *n, PCAP - *n, NULL, 0, 0); if (m > 0) *n += m; }
static void suffix_s(TtsPh *o, int *n) {
    int l = *n ? o[*n - 1].ph : 0;
    if (l == P_S || l == P_Z || l == P_SH || l == P_ZH || l == P_CH || l == P_JH) app(o, n, "IH0 Z");
    else if (l == P_P || l == P_T || l == P_K || l == P_F || l == P_TH) app(o, n, "S");
    else app(o, n, "Z");
}
static void suffix_d(TtsPh *o, int *n) {
    int l = *n ? o[*n - 1].ph : 0;
    if (l == P_T || l == P_D) app(o, n, "IH0 D");
    else if (l == P_P || l == P_K || l == P_F || l == P_S || l == P_SH || l == P_CH || l == P_TH) app(o, n, "T");
    else app(o, n, "D");
}

static bool lex_find(const char *w, TtsPh *out, int *n, int *src) {
    Ent *e = lex_get(&L_USER, w);
    if (e) { memcpy(out, e->ph, (size_t)e->n * sizeof(TtsPh)); *n = e->n; if (src) *src = SRC_LEXICON; return true; }
    e = lex_get(&L_DICT, w);
    if (e) { memcpy(out, e->ph, (size_t)e->n * sizeof(TtsPh)); *n = e->n; if (src) *src = SRC_DICT; return true; }
    return false;
}
bool tts_lex_lookup(const char *word, TtsPh *out, int *n, int cap, int *src) {
    if (!g_init) tts_g2p_init();
    char k[128]; snprintf(k, sizeof k, "%s", word); for (char *p = k; *p; p++) *p = (char)tolower((unsigned char)*p);
    TtsPh t[PCAP]; int m;
    if (!lex_find(k, t, &m, src)) return false;
    if (m > cap) m = cap;
    memcpy(out, t, (size_t)m * sizeof(TtsPh)); *n = m; return true;
}

static bool morph(const char *w, TtsPh *out, int *n, int depth, int *src);

/* stem candidates for a suffix: try found-in-lexicon/morph stems */
static bool stem_lookup(const char *stem, TtsPh *out, int *n, int depth, int *src) {
    if (strlen(stem) < 2) return false;
    if (lex_find(stem, out, n, src)) return true;
    return depth < 2 && morph(stem, out, n, depth + 1, src);
}

static bool morph(const char *w, TtsPh *out, int *n, int depth, int *src) {
    size_t L = strlen(w); char st[128]; if (L >= 120 || L < 3) return false;
    TtsPh t[PCAP]; int m;
    /* plural / 3rd person */
    if (L > 3 && ends_with(w, "ies")) { snprintf(st, sizeof st, "%.*sy", (int)L - 3, w); if (stem_lookup(st, t, &m, depth, src)) { memcpy(out, t, (size_t)m * sizeof(TtsPh)); *n = m; suffix_s(out, n); return true; } }
    if (L > 3 && ends_with(w, "es")) {
        snprintf(st, sizeof st, "%.*s", (int)L - 2, w);
        if (strchr("sxz", st[L - 3]) || ends_with(st, "ch") || ends_with(st, "sh")) { if (stem_lookup(st, t, &m, depth, src)) { memcpy(out, t, (size_t)m * sizeof(TtsPh)); *n = m; suffix_s(out, n); return true; } }
        snprintf(st, sizeof st, "%.*s", (int)L - 1, w);
        if (stem_lookup(st, t, &m, depth, src)) { memcpy(out, t, (size_t)m * sizeof(TtsPh)); *n = m; suffix_s(out, n); return true; }
    }
    if (L > 2 && w[L - 1] == 's' && w[L - 2] != 's' && w[L - 2] != 'u' && w[L - 2] != 'i') { snprintf(st, sizeof st, "%.*s", (int)L - 1, w); if (stem_lookup(st, t, &m, depth, src)) { memcpy(out, t, (size_t)m * sizeof(TtsPh)); *n = m; suffix_s(out, n); return true; } }
    /* -ed / -ing / -er / -est : stem, stem+e, undoubled stem, y-stem */
    static const struct { const char *suf; int kind; } SUFS[] = { { "ing", 1 }, { "ed", 2 }, { "er", 3 }, { "est", 4 } };
    for (int si = 0; si < 4; si++) {
        const char *suf = SUFS[si].suf; size_t sl = strlen(suf);
        if (L <= sl + 2 || !ends_with(w, suf)) continue;
        char cand[4][128]; int nc = 0; size_t b = L - sl;
        snprintf(cand[nc++], 128, "%.*s", (int)b, w);
        snprintf(cand[nc++], 128, "%.*se", (int)b, w);
        if (b > 2 && w[b - 1] == w[b - 2] && cons_c(w[b - 1])) snprintf(cand[nc++], 128, "%.*s", (int)b - 1, w);
        if (b > 2 && w[b - 1] == 'i') snprintf(cand[nc++], 128, "%.*sy", (int)b - 1, w);
        for (int c = 0; c < nc; c++) {
            if (!stem_lookup(cand[c], t, &m, depth, src)) continue;
            memcpy(out, t, (size_t)m * sizeof(TtsPh)); *n = m;
            switch (SUFS[si].kind) { case 1: app(out, n, "IH0 NG"); break; case 2: suffix_d(out, n); break; case 3: app(out, n, "ER0"); break; default: app(out, n, "AH0 S T"); }
            return true;
        }
    }
    /* derivational suffixes */
    static const struct { const char *suf; const char *pron; } DER[] = {
        { "ly", "L IY0" }, { "ness", "N AH0 S" }, { "less", "L AH0 S" }, { "ment", "M AH0 N T" }, { "ful", "F AH0 L" }, { "able", "AH0 B AH0 L" },
        { "ish", "IH0 SH" }, { "ship", "SH IH2 P" }, { "hood", "HH UH2 D" }, { "dom", "D AH0 M" }, { "ize", "AY2 Z" }, { "ist", "IH0 S T" }, { "ism", "IH2 Z AH0 M" },
    };
    for (size_t i = 0; i < sizeof DER / sizeof DER[0]; i++) {
        size_t sl = strlen(DER[i].suf);
        if (L <= sl + 2 || !ends_with(w, DER[i].suf)) continue;
        char cand[3][128]; int nc = 0; size_t b = L - sl;
        snprintf(cand[nc++], 128, "%.*s", (int)b, w);
        snprintf(cand[nc++], 128, "%.*se", (int)b, w);
        if (b > 2 && w[b - 1] == 'i') snprintf(cand[nc++], 128, "%.*sy", (int)b - 1, w);
        for (int c = 0; c < nc; c++) if (stem_lookup(cand[c], t, &m, depth, src)) {
            memcpy(out, t, (size_t)m * sizeof(TtsPh)); *n = m;
            if (!strcmp(DER[i].suf, "ly") && m && t[m - 1].ph == P_L) app(out, n, "IY0"); else app(out, n, DER[i].pron);
            return true;
        }
    }
    /* prefixes on known stems */
    static const struct { const char *pre; const char *pron; } PRE[] = {
        { "un", "AH0 N" }, { "re", "R IY0" }, { "pre", "P R IY0" }, { "dis", "D IH0 S" }, { "mis", "M IH0 S" }, { "non", "N AA2 N" }, { "over", "OW2 V ER0" }, { "under", "AH2 N D ER0" },
        { "out", "AW2 T" }, { "sub", "S AH0 B" }, { "inter", "IH2 N T ER0" }, { "anti", "AE2 N T IY0" }, { "semi", "S EH2 M IY0" }, { "multi", "M AH2 L T IY0" }, { "super", "S UW2 P ER0" }, { "co", "K OW2" }, { "de", "D IY0" },
    };
    for (size_t i = 0; i < sizeof PRE / sizeof PRE[0]; i++) {
        size_t pl = strlen(PRE[i].pre);
        if (L < pl + 3 || strncmp(w, PRE[i].pre, pl)) continue;
        if (!stem_lookup(w + pl, t, &m, depth, src)) continue;
        *n = 0; app(out, n, PRE[i].pron);
        memcpy(out + *n, t, (size_t)m * sizeof(TtsPh)); *n += m; return true;
    }
    return false;
}

static bool compound(const char *w, TtsPh *out, int *n) {
    size_t L = strlen(w); if (L < 7 || L > 40) return false;
    int best = -1, bscore = 0; TtsPh a[PCAP], b[PCAP]; int na, nb;
    for (size_t k = 3; k + 4 <= L; k++) {
        char l[64], r[64]; snprintf(l, sizeof l, "%.*s", (int)k, w); snprintf(r, sizeof r, "%s", w + k);
        int sa = 0; if (!lex_find(l, a, &na, &sa) || !(lex_find(r, b, &nb, &sa) || morph(r, b, &nb, 1, &sa))) continue;
        int sc = (int)(k < L - k ? k : L - k);
        if (sc > bscore) { bscore = sc; best = (int)k; }
    }
    if (best < 0) return false;
    char l[64], r[64]; snprintf(l, sizeof l, "%.*s", best, w); snprintf(r, sizeof r, "%s", w + best);
    int sa; lex_find(l, a, &na, &sa); if (!(lex_find(r, b, &nb, &sa))) morph(r, b, &nb, 1, &sa);
    if (na + nb > PCAP) return false;
    memcpy(out, a, (size_t)na * sizeof(TtsPh));
    for (int i = 0; i < nb; i++) { b[i].st = (uint8_t)(b[i].st == 1 ? 2 : b[i].st); }
    memcpy(out + na, b, (size_t)nb * sizeof(TtsPh)); *n = na + nb; return true;
}

/* ================================================================== function words */
bool tts_is_function_word(const char *w) {
    static const char *const FW[] = {
        "the", "a", "an", "of", "to", "in", "on", "at", "for", "with", "and", "or", "but", "if", "as", "by", "from", "into", "onto", "than", "that", "this", "these", "those", "there", "here",
        "is", "are", "was", "were", "be", "been", "being", "am", "do", "does", "did", "has", "have", "had", "will", "would", "shall", "should", "can", "could", "may", "might", "must",
        "i", "you", "he", "she", "it", "we", "they", "me", "him", "her", "us", "them", "my", "your", "his", "its", "our", "their", "mine", "yours", "ours", "theirs", "not", "so", "too", "very",
        "just", "then", "now", "up", "down", "out", "off", "over", "under", "again", "once", "about", "above", "after", "before", "between", "through", "during", "against", "while", "because",
        "until", "since", "although", "though", "whether", "either", "neither", "nor", "unless", "within", "without", "around", "among", "across", "along", "toward", "towards", "upon", "per", "via",
        "what", "which", "who", "whom", "whose", "when", "where", "why", "how", "all", "any", "some", "each", "every", "both", "few", "more", "most", "other", "such", "own", "same", "also", "only",
        "oh", "uh", "um", "ah", "yet", "even", "ever", "still", "no", "yes", "okay", "ok", NULL };
    size_t L = strlen(w);
    if (L > 2 && (ends_with(w, "'s") || ends_with(w, "'t") || ends_with(w, "'ll") || ends_with(w, "'ve") || ends_with(w, "'re") || ends_with(w, "'d") || ends_with(w, "'m"))) return true;
    for (int i = 0; FW[i]; i++) if (!strcmp(w, FW[i])) return true;
    return false;
}

/* ================================================================== homographs */
typedef struct { const char *w, *noun, *verb; int dflt; /* 0 noun, 1 verb */ } HgNV;
static const HgNV HG_NV[] = {
    { "record", "R EH1 K ER0 D", "R IH0 K AO1 R D", 0 }, { "present", "P R EH1 Z AH0 N T", "P R IH0 Z EH1 N T", 0 }, { "object", "AA1 B JH EH0 K T", "AH0 B JH EH1 K T", 0 },
    { "project", "P R AA1 JH EH0 K T", "P R AH0 JH EH1 K T", 0 }, { "permit", "P ER1 M IH2 T", "P ER0 M IH1 T", 0 }, { "produce", "P R OW1 D UW0 S", "P R AH0 D UW1 S", 1 },
    { "refuse", "R EH1 F Y UW2 S", "R IH0 F Y UW1 Z", 1 }, { "subject", "S AH1 B JH IH0 K T", "S AH0 B JH EH1 K T", 0 }, { "contract", "K AA1 N T R AE2 K T", "K AH0 N T R AE1 K T", 0 },
    { "conduct", "K AA1 N D AH0 K T", "K AH0 N D AH1 K T", 0 }, { "progress", "P R AA1 G R EH2 S", "P R AH0 G R EH1 S", 0 }, { "increase", "IH1 N K R IY2 S", "IH0 N K R IY1 S", 0 },
    { "decrease", "D IY1 K R IY2 S", "D IH0 K R IY1 S", 0 }, { "import", "IH1 M P AO0 R T", "IH0 M P AO1 R T", 0 }, { "export", "EH1 K S P AO0 R T", "IH0 K S P AO1 R T", 0 },
    { "survey", "S ER1 V EY2", "S ER0 V EY1", 0 }, { "insult", "IH1 N S AH2 L T", "IH0 N S AH1 L T", 0 }, { "rebel", "R EH1 B AH0 L", "R IH0 B EH1 L", 0 }, { "desert", "D EH1 Z ER0 T", "D IH0 Z ER1 T", 0 },
    { "convict", "K AA1 N V IH0 K T", "K AH0 N V IH1 K T", 0 }, { "contrast", "K AA1 N T R AE0 S T", "K AH0 N T R AE1 S T", 0 }, { "discount", "D IH1 S K AW0 N T", "D IH0 S K AW1 N T", 0 },
    { "suspect", "S AH1 S P EH0 K T", "S AH0 S P EH1 K T", 0 }, { "transport", "T R AE1 N S P AO0 R T", "T R AE0 N S P AO1 R T", 0 }, { "update", "AH1 P D EY2 T", "AH0 P D EY1 T", 0 },
    { "separate", "S EH1 P ER0 AH0 T", "S EH1 P ER0 EY2 T", 0 }, { "estimate", "EH1 S T AH0 M AH0 T", "EH1 S T AH0 M EY2 T", 0 }, { "duplicate", "D UW1 P L AH0 K AH0 T", "D UW1 P L AH0 K EY2 T", 0 },
    { "moderate", "M AA1 D ER0 AH0 T", "M AA1 D ER0 EY2 T", 0 }, { "graduate", "G R AE1 JH AH0 W AH0 T", "G R AE1 JH AH0 W EY2 T", 0 }, { "approximate", "AH0 P R AA1 K S AH0 M AH0 T", "AH0 P R AA1 K S AH0 M EY2 T", 0 },
    { "associate", "AH0 S OW1 S IY0 AH0 T", "AH0 S OW1 S IY0 EY2 T", 0 }, { "alternate", "AO1 L T ER0 N AH0 T", "AO1 L T ER0 N EY2 T", 0 },
    { "use", "Y UW1 S", "Y UW1 Z", 1 }, { "uses", "Y UW1 S AH0 Z", "Y UW1 Z AH0 Z", 1 }, { "close", "K L OW1 S", "K L OW1 Z", 1 }, { "live", "L AY1 V", "L IH1 V", 1 },
    { "wind", "W IH1 N D", "W AY1 N D", 0 }, { "wound", "W UW1 N D", "W AW1 N D", 0 }, { "tear", "T IH1 R", "T EH1 R", 1 }, { "lead", "L EH1 D", "L IY1 D", 1 }, { "bass", "B EY1 S", "B EY1 S", 0 },
    { "bow", "B OW1", "B AW1", 0 }, { "row", "R OW1", "R AW1", 0 }, { "minute", "M IH1 N AH0 T", "M AY0 N UW1 T", 0 },
    { NULL, NULL, NULL, 0 }
};
static bool in_list(const char *w, const char *const *l) { for (; *l; l++) if (!strcmp(w, *l)) return true; return false; }
static const char *const VERB_PREV[] = { "to", "will", "would", "can", "could", "shall", "should", "may", "might", "must", "i", "you", "we", "they", "don't", "doesn't", "didn't", "not", "please", "let's", "cannot", "won't", "can't", "wouldn't", "couldn't", "shouldn't", "do", "did", "does", "then", "and", NULL };
static const char *const NOUN_PREV[] = { "the", "a", "an", "this", "that", "my", "your", "his", "her", "its", "our", "their", "some", "any", "no", "each", "every", "another", "these", "those", "new", "old", "first", "last", "main", "own", "best", "good", "large", "small", "big", "great", "of", "in", "for", "make", "makes", "making", "made", "much", "full", "little", "more", "less", "same", "heavy", NULL };

static const char *homograph(const char *w, const char *prev, const char *next) {
    prev = prev ? prev : ""; next = next ? next : "";
    if (!strcmp(w, "read")) { static const char *const PAST[] = { "had", "has", "have", "having", "was", "were", "been", "i've", "you've", "we've", "they've", "already", NULL }; return in_list(prev, PAST) ? "R EH1 D" : "R IY1 D"; }
    if (!strcmp(w, "used")) { return !strcmp(next, "to") && !in_list(prev, (const char *const[]){ "is", "are", "was", "were", "be", "been", "being", NULL }) ? "Y UW1 S T" : "Y UW1 Z D"; }
    if (!strcmp(w, "lead")) {
        if (in_list(next, (const char *const[]){ "pipe", "pipes", "paint", "poisoning", "weight", "weights", "bullet", "bullets", "shot", "acid", "free", "levels", NULL })) return "L EH1 D";
        if (in_list(prev, VERB_PREV)) return "L IY1 D"; return "L IY1 D";
    }
    if (!strcmp(w, "bass")) {
        if (in_list(next, (const char *const[]){ "fishing", "fish", "lake", "pro", "boat", NULL })) return "B AE1 S"; return "B EY1 S";
    }
    if (!strcmp(w, "live")) {
        if (in_list(prev, (const char *const[]){ "to", "will", "can", "could", "would", "i", "you", "we", "they", "do", "did", "don't", "where", "who", "and", "let", "should", "must", NULL })) return "L IH1 V";
        if (in_list(prev, (const char *const[]){ "a", "the", "is", "are", "was", "this", "that", "be", "been", "go", "going", "goes", "went", "stay", "stays", "it's", NULL }) ||
            in_list(next, (const char *const[]){ "show", "music", "concert", "stream", "streaming", "broadcast", "wire", "performance", "audience", "tv", "television", "event", "coverage", "video", "action", "ammunition", "bait", "animal", "recording", "chat", "demo", "feed", "band", NULL })) return "L AY1 V";
        return "L IH1 V";
    }
    if (!strcmp(w, "close")) {
        if (in_list(prev, VERB_PREV) || (!*prev && in_list(next, (const char *const[]){ "the", "a", "this", "that", "it", "your", "my", "all", "up", "down", "out", "any", "every", "each", "both", "these", "those", NULL }))) return "K L OW1 Z";
        if (in_list(prev, (const char *const[]){ "very", "so", "too", "quite", "really", "how", "is", "are", "was", "were", "be", "being", "feel", "feels", "pretty", "fairly", "extremely", "a", "the", "come", "came", "comes", "stay", "stays", NULL }) ||
            in_list(next, (const char *const[]){ "to", "by", "friends", "friend", "call", "race", "enough", "together", "relationship", "family", "attention", "look", "contact", NULL })) return "K L OW1 S";
        return "K L OW1 Z";
    }
    if (!strcmp(w, "wound")) { if (in_list(prev, (const char *const[]){ "had", "has", "have", "it", "he", "she", "they", "we", "who", "that", "and", "i", "you", NULL })) return "W AW1 N D"; return "W UW1 N D"; }
    if (!strcmp(w, "wind")) { if (in_list(prev, (const char *const[]){ "to", "will", "can", "could", "would", "please", "and", "not", NULL })) return "W AY1 N D"; return "W IH1 N D"; }
    if (!strcmp(w, "tear")) { if (in_list(next, (const char *const[]){ "drop", "drops", "gas", "falls", "fell", "ran", "rolled", "streamed", "of", "duct", "stain", "stained", NULL })) return "T IH1 R"; return "T EH1 R"; }
    if (!strcmp(w, "bow")) { if (in_list(prev, VERB_PREV)) return "B AW1"; return "B OW1"; }
    if (!strcmp(w, "row")) { return "R OW1"; }
    if (!strcmp(w, "minute")) { if (in_list(next, (const char *const[]){ "detail", "details", "particles", "amounts", "differences", "changes", "quantity", "quantities", NULL })) return "M AY0 N UW1 T"; return "M IH1 N AH0 T"; }
    for (const HgNV *h = HG_NV; h->w; h++) {
        if (strcmp(w, h->w)) continue;
        bool verb = h->dflt == 1;
        if (in_list(prev, VERB_PREV)) verb = true; else if (in_list(prev, NOUN_PREV)) verb = false;
        if (!strcmp(next, "to") && h->dflt == 0 && !in_list(prev, NOUN_PREV)) verb = verb || false;
        return verb ? h->verb : h->noun;
    }
    return NULL;
}

/* ================================================================== lexicon text I/O */
int tts_lex_size(void) { if (!g_init) tts_g2p_init(); return L_USER.cnt; }
int tts_dict_size(void) { if (!g_init) tts_g2p_init(); return L_DICT.cnt; }
void tts_lex_add(const char *word, const TtsPh *ph, int n) {
    if (!g_init) tts_g2p_init();
    char k[128]; snprintf(k, sizeof k, "%s", word); for (char *p = k; *p; p++) *p = (char)tolower((unsigned char)*p);
    lex_put(&L_USER, k, ph, n);
}
bool tts_lex_remove(const char *word) {
    char k[128]; snprintf(k, sizeof k, "%s", word); for (char *p = k; *p; p++) *p = (char)tolower((unsigned char)*p);
    return lex_del(&L_USER, k);
}
void tts_lex_clear(void) { lex_clear(&L_USER); }

int tts_lex_load_text(const char *text, KcPos pos, const char *who, int *errors) {
    if (!g_init) tts_g2p_init();
    int added = 0; if (errors) *errors = 0;
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n'); size_t L = e ? (size_t)(e - p) : strlen(p);
        char line[512]; if (L >= sizeof line) L = sizeof line - 1;
        memcpy(line, p, L); line[L] = 0; p = e ? e + 1 : p + L;
        char *s = line; while (*s == ' ' || *s == '\t' || *s == '\r') s++;
        size_t sl = strlen(s); while (sl && (s[sl - 1] == '\r' || s[sl - 1] == ' ' || s[sl - 1] == '\t')) s[--sl] = 0;
        if (!*s || !strncmp(s, ";;;", 3) || *s == '#' || !strncmp(s, "//", 2)) continue;
        char *pron = NULL; char word[128];
        char *eq = strchr(s, '=');
        if (eq) { *eq = 0; pron = eq + 1; } else { pron = s; while (*pron && *pron != ' ' && *pron != '\t') pron++; if (*pron) *pron++ = 0; }
        snprintf(word, sizeof word, "%s", s);
        { size_t wl = strlen(word); while (wl && (word[wl - 1] == ' ' || word[wl - 1] == '\t')) word[--wl] = 0; }
        while (*pron == ' ' || *pron == '\t') pron++;
        char *par = strchr(word, '(');
        if (par) { int var = atoi(par + 1); *par = 0; if (var > 1) continue; }
        if (!*word || !*pron) { if (errors) (*errors)++; continue; }
        TtsPh ph[PCAP]; int n; char bad[32] = "";
        bool ascii = true; for (const char *q = pron; *q; q++) if ((unsigned char)*q >= 0x80) ascii = false;
        n = ascii ? tts_phseq_parse(pron, ph, PCAP, bad, sizeof bad) : tts_ipa_parse(pron, ph, PCAP);
        if (n <= 0) { if (errors) (*errors)++; continue; }
        tts_lex_add(word, ph, n); added++;
    }
    (void)pos; (void)who;
    return added;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
char *tts_lex_dump(void) {
    TBuf b; tb_init(&b);
    if (L_USER.t) {
        char **names = malloc((size_t)(L_USER.cnt + 1) * sizeof(char *)); int k = 0;
        for (int i = 0; i < L_USER.cap; i++) for (Ent *e = L_USER.t[i]; e; e = e->next) names[k++] = e->w;
        qsort(names, (size_t)k, sizeof(char *), cmp_str);
        for (int i = 0; i < k; i++) { Ent *e = lex_get(&L_USER, names[i]); char *s = tts_phseq_format(e->ph, e->n, false, true); tb_printf(&b, "%s  %s\n", names[i], s); free(s); }
        free(names);
    }
    return tb_take(&b);
}

/* ================================================================== public */
void tts_spell_word(const char *w, TtsPh *out, int *n, int cap) {
    *n = 0;
    for (; *w; w++) {
        char c = (char)tolower((unsigned char)*w); if (c < 'a' || c > 'z') continue;
        int m = tts_phseq_parse_ex(LETTER_PH[c - 'a'], out + *n, cap - *n, NULL, 0, 1);
        if (m > 0) *n += m;
        if (*n >= cap - 8) break;
    }
}

void tts_g2p_init(void) {
    if (g_init) return;
    g_init = true;
    lex_init(&L_DICT, 4096); lex_init(&L_USER, 256);
    rules_init();
    for (int i = 0; i < TTS_DICT_N; i++) {
        const char *s = TTS_DICT[i]; const char *sp = strchr(s, ' '); if (!sp) continue;
        char w[96]; size_t wl = (size_t)(sp - s); if (wl >= sizeof w) continue;
        memcpy(w, s, wl); w[wl] = 0;
        TtsPh ph[PCAP]; int n = tts_phseq_parse(sp + 1, ph, PCAP, NULL, 0);
        if (n > 0) lex_put(&L_DICT, w, ph, n);
    }
}

void tts_g2p_word(TtsWord *w, const char *prev, const char *next, const TtsOpts *o) {
    if (!g_init) tts_g2p_init();
    TtsPh out[PCAP]; int n = 0, src = SRC_RULES;
    const char *t = w->text; size_t L = strlen(t);
    free(w->ph); w->ph = NULL; w->nph = 0;
    bool done = false;
    const char *hg = homograph(t, prev, next);
    if (hg && !lex_get(&L_USER, t)) { n = tts_phseq_parse(hg, out, PCAP, NULL, 0); if (n > 0) { src = SRC_HOMOGRAPH; done = true; } }
    if (!done && lex_find(t, out, &n, &src)) done = true;
    if (done && !strcmp(t, "the") && next && strchr("aeiou", next[0]) && strncmp(next, "uni", 3) && strncmp(next, "use", 3) && strncmp(next, "one", 3) && strncmp(next, "eu", 2)) {
        n = 0; app(out, &n, "DH IY0");
    }
    if (!done && w->caps && L >= 2 && L <= 6) {   /* acronyms: spell unless pronounceable */
        int nv = 0; for (size_t i = 0; i < L; i++) if (vow_c(t[i])) nv++;
        bool spell = L <= 3 || nv == 0 || (o && o->spell_unknown && L <= 4);
        if (!spell && L >= 4) { int run = 0, mx = 0; for (size_t i = 0; i < L; i++) { if (cons_c(t[i])) { run++; if (run > mx) mx = run; } else run = 0; } if (mx >= 3) spell = true; }
        if (spell) { tts_spell_word(t, out, &n, PCAP); src = SRC_ACRONYM; done = true; }
    }
    if (!done) {
        /* possessives and contractions on arbitrary stems */
        char stem[128]; TtsPh sp[PCAP]; int sn, ss;
        if (L > 3 && ends_with(t, "'s")) {
            snprintf(stem, sizeof stem, "%.*s", (int)L - 2, t);
            TtsWord tmp = *w; tmp.text = stem; tmp.ph = NULL; tmp.nph = 0; tts_g2p_word(&tmp, NULL, NULL, o);
            if (tmp.nph > 0) { memcpy(out, tmp.ph, (size_t)tmp.nph * sizeof(TtsPh)); n = tmp.nph; suffix_s(out, &n); src = tmp.src; done = true; }
            free(tmp.ph);
        } else if (L > 4 && ends_with(t, "n't")) {
            snprintf(stem, sizeof stem, "%.*s", (int)L - 3, t);
            TtsWord tmp = *w; tmp.text = stem; tmp.ph = NULL; tmp.nph = 0; tts_g2p_word(&tmp, NULL, NULL, o);
            if (tmp.nph > 0) { memcpy(out, tmp.ph, (size_t)tmp.nph * sizeof(TtsPh)); n = tmp.nph; app(out, &n, "AH0 N T"); src = tmp.src; done = true; }
            free(tmp.ph);
        } else if (L > 4 && (ends_with(t, "'ll") || ends_with(t, "'ve") || ends_with(t, "'re") || ends_with(t, "'d") || ends_with(t, "'m"))) {
            const char *ap = strrchr(t, '\''); size_t sl = (size_t)(ap - t);
            snprintf(stem, sizeof stem, "%.*s", (int)sl, t);
            TtsWord tmp = *w; tmp.text = stem; tmp.ph = NULL; tmp.nph = 0; tts_g2p_word(&tmp, NULL, NULL, o);
            if (tmp.nph > 0) {
                memcpy(out, tmp.ph, (size_t)tmp.nph * sizeof(TtsPh)); n = tmp.nph; src = tmp.src; done = true;
                if (!strcmp(ap, "'ll")) app(out, &n, "AH0 L"); else if (!strcmp(ap, "'ve")) app(out, &n, "AH0 V"); else if (!strcmp(ap, "'re")) app(out, &n, "ER0");
                else if (!strcmp(ap, "'d")) app(out, &n, "D"); else app(out, &n, "M");
            }
            free(tmp.ph);
        }
        (void)sp; (void)sn; (void)ss;
    }
    if (!done && morph(t, out, &n, 0, &src)) { src = SRC_MORPH; done = true; }
    if (!done && compound(t, out, &n)) { src = SRC_COMPOUND; done = true; }
    if (!done) {
        char clean[128]; int k = 0;
        for (size_t i = 0; i < L && k < 120; i++) if (t[i] != '\'') clean[k++] = t[i];
        clean[k] = 0;
        n = rules_word(clean, out, PCAP);
        tts_assign_stress(out, n, clean);
        src = SRC_RULES;
    }
    for (int i = 0; i < n; i++) if (out[i].st == ST_UNSET) out[i].st = 0;
    w->ph = malloc((size_t)(n ? n : 1) * sizeof(TtsPh)); memcpy(w->ph, out, (size_t)n * sizeof(TtsPh)); w->nph = n; w->src = (uint8_t)src;
}
