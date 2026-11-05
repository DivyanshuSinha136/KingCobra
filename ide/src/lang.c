/* kcide — highlighters for the KingCobra data languages:
 *   KCM (music)  std.music_note scores: tracks, notes, chords, durations, patterns, samples, timestamps
 *   KCM (model)  std.game "kcmodel" text models: part / v / f / primitives
 *   SSM          SSML speech markup understood by std.tts (XML subset)
 * plus language detection, comment styles and completion vocabularies. */
#include "syntax.h"
#include <ctype.h>

static bool in_set(const char *const *l, const char *w, int n) {
    for (int i = 0; l[i]; i++) if ((int)strlen(l[i]) == n && !strncmp(l[i], w, (size_t)n)) return true;
    return false;
}
static void fill(uint8_t *o, int a, int b, uint8_t c) { for (int i = a; i < b; i++) o[i] = c; }

/* ---------------------------------------------------------------- detection */
Lang lang_from_path(const char *path) {
    const char *e = path_ext(path);
    if (!strcmp(e, ".kc")) return LANG_KC;
    if (!strcasecmp(e, ".kcm")) return LANG_KCM_MUSIC;
    if (!strcasecmp(e, ".ssm") || !strcasecmp(e, ".ssml")) return LANG_SSM;
    return LANG_PLAIN;
}
Lang lang_sniff_kcm(const char *t, size_t n) {
    size_t i = 0; int lines = 0;
    while (i < n && lines < 60) {
        size_t j = i; while (j < n && t[j] != '\n') j++;
        size_t a = i; while (a < j && (t[a] == ' ' || t[a] == '\t' || t[a] == '\r')) a++;
        if (a < j && t[a] != '#' && t[a] != ';' && !(t[a] == '/' && a + 1 < j && t[a + 1] == '/')) return (j - a >= 7 && !strncmp(t + a, "kcmodel", 7)) ? LANG_KCM_MODEL : LANG_KCM_MUSIC;
        i = j + 1; lines++;
    }
    return LANG_KCM_MUSIC;
}
const char *lang_name(Lang l) {
    static const char *N[] = { "Plain Text", "KingCobra", "KCM (music)", "KCM (model)", "SSM (SSML)" };
    return N[l];
}
const char *lang_comment(Lang l) { return l == LANG_KC ? "// " : (l == LANG_KCM_MUSIC || l == LANG_KCM_MODEL) ? "# " : NULL; }

/* ---------------------------------------------------------------- KCM music */
static const char *const MUS_STRUCT[] = { "track", "pattern", "end", "sample", "include", "length", "mark", NULL };
static const char *const MUS_TEXTDIR[] = { "title", "composer", NULL };
static const char *const MUS_OPT[] = { "tempo", "time", "key", "inst", "vol", "pan", "reverb", "delay", "delay_time", "delay_fb", "chorus", "lowpass", "highpass", "swing", "swing16",
    "transpose", "strum", "gate", "vel", "glide", "adsr", "vibrato", "detune", "at", "wait", "octave", "l", "len", "dyn", "attack", "decay", "sustain", "release", "gain", "root", "bpm",
    "start", "fadein", "fadeout", "reverse", "oneshot", "cut", "data", "sample", "strict_bars", "o", NULL };
static const char *const MUS_INST[] = { "sine", "square", "saw", "triangle", "pulse", "organ", "piano", "epiano", "bell", "pluck", "bass", "strings", "pad", "brass", "flute", "lead", "marimba",
    "xylophone", "harp", "choir", "noise", "808", "cowbell", "clav", "slap", "wah", "stab", "drums", "guitar", "violin", "cello", "rhodes", "trumpet", "horn", "voice", "vox", "kit", NULL };
static const char *const MUS_DRUM[] = { "kick", "snare", "rim", "clap", "hat", "ohat", "tom_lo", "tom_mid", "tom_hi", "crash", "ride", "shaker", "tambourine", "conga", "clave", "kick808", "snap",
    "bd", "sd", "hh", "oh", "cp", "cr", "rd", "cb", "tamb", NULL };
static const char *const MUS_DYN[] = { "pp", "ppp", "p", "mp", "mf", "ff", "fff", NULL };

static bool is_note_word(const char *w, int n) {
    if (n < 1 || n > 4) return false; int i = 0;
    if (!strchr("ABCDEFGabcdefg", w[i])) return false; i++;
    if (i < n && (w[i] == '#' || w[i] == 'b')) i++;
    if (i < n && w[i] == '-') i++;
    if (i < n && isdigit((unsigned char)w[i])) i++;
    return i == n;
}
static bool numeric_start(const char *s, int i, int len) { return isdigit((unsigned char)s[i]) || ((s[i] == '-' || s[i] == '.') && i + 1 < len && isdigit((unsigned char)s[i + 1])); }
static bool is_wordc(char c) { return isalnum((unsigned char)c) || c == '_' || c == '#' || c == '.' || c == '-'; }

static uint8_t hl_music(const char *s, int len, uint8_t st, uint8_t *o) {
    int mode = st & 7, i = 0; bool first = true; int ctx = 0;   /* ctx: 1 track name, 2 track instrument, 3 inst value, 4 pattern/sample name, 5 mark name, 6 free text */
    char head[16] = "";
    if (mode == HS_BLOCK) {
        int j = 0; while (j < len && !(s[j] == '*' && j + 1 < len && s[j + 1] == '/')) j++;
        if (j < len) { j += 2; mode = HS_NORMAL; } fill(o, 0, j, HC_COMMENT); i = j;
    } else if (mode == 5) {                                    /* inline base64 sample data until a line `end` */
        int a = 0; while (a < len && (s[a] == ' ' || s[a] == '\t')) a++;
        int b = len; while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
        if (b - a == 3 && !strncmp(s + a, "end", 3)) { fill(o, 0, a, HC_TEXT); fill(o, a, b, HC_DECL); fill(o, b, len, HC_TEXT); return HS_NORMAL; }
        fill(o, 0, len, HC_STRING); return 5;
    }
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ' || c == '\t' || c == '\r') { o[i++] = HC_TEXT; continue; }
        if (ctx == 6) { fill(o, i, len, HC_STRING); i = len; break; }
        if (c == '#' || c == ';' || (c == '/' && i + 1 < len && s[i + 1] == '/')) { fill(o, i, len, HC_COMMENT); i = len; break; }
        if (c == '/' && i + 1 < len && s[i + 1] == '*') {
            int j = i + 2; while (j < len && !(s[j] == '*' && j + 1 < len && s[j + 1] == '/')) j++;
            if (j < len) j += 2; else mode = HS_BLOCK;
            fill(o, i, j, HC_COMMENT); i = j; continue;
        }
        if (c == '"') { int j = i + 1; while (j < len && s[j] != '"') j++; if (j < len) j++; fill(o, i, j, HC_STRING); i = j; first = false; continue; }
        if (c == '\\' && i + 1 >= len) { o[i++] = HC_OPERATOR; continue; }
        if (c == '|') { int j = i + 1; if (j < len && s[j] == ':') j++; fill(o, i, j, HC_OPERATOR); i = j; first = false; continue; }
        if (c == ':' && i + 1 < len && s[i + 1] == '|') { int j = i + 2; if (j < len && s[j] == 'x') { j++; while (j < len && isdigit((unsigned char)s[j])) j++; } fill(o, i, j, HC_OPERATOR); i = j; first = false; continue; }
        if (c == ':') { int j = i + 1; while (j < len && (isdigit((unsigned char)s[j]) || strchr("whqestxb./+*", s[j]))) j++; fill(o, i, j, HC_DURATION); i = j; continue; }
        if (c == '{') { int j = i + 1; while (j < len && s[j] != '}') j++; if (j < len) j++; fill(o, i, j, HC_CHORD); i = j; first = false; continue; }
        if (c == '[' || c == ']') { o[i++] = HC_PUNCT; first = false; continue; }
        if (c == '~' || c == '\'' || c == '^' || c == '!' || c == '>' || c == '<') { o[i++] = HC_OPERATOR; continue; }
        if (c == '$') { int j = i + 1; while (j < len && (isalnum((unsigned char)s[j]) || s[j] == '_' || s[j] == '-')) j++; fill(o, i, j, HC_FUNC); i = j; if (i < len && s[i] == '*') { o[i++] = HC_OPERATOR; while (i < len && isdigit((unsigned char)s[i])) o[i++] = HC_NUMBER; } first = false; continue; }
        if (c == '&') { int j = i + 1; while (j < len && (isalnum((unsigned char)s[j]) || s[j] == '_' || s[j] == '-')) j++; fill(o, i, j, HC_MODULE); i = j; first = false; continue; }
        if (c == '@') { int j = i + 1; while (j < len && isalnum((unsigned char)s[j])) j++; fill(o, i, j, HC_CONST); i = j; first = false; continue; }
        if (c == '=') { o[i++] = HC_OPERATOR; continue; }
        if (numeric_start(s, i, len)) { int j = i + 1; while (j < len && (isalnum((unsigned char)s[j]) || s[j] == '.' || s[j] == ':' || s[j] == ',')) j++; fill(o, i, j, HC_NUMBER); i = j; first = false; continue; }
        if (is_wordc((char)c)) {
            int j = i; while (j < len && is_wordc(s[j])) j++;
            int n = j - i; const char *w = s + i; uint8_t cls = HC_TEXT; bool was_first = first; first = false;
            if (j < len && s[j] == '=') {                                  /* key=value option */
                cls = in_set(MUS_OPT, w, n) || in_set(MUS_STRUCT, w, n) ? HC_BUILTIN : HC_FIELD; fill(o, i, j, cls); o[j] = HC_OPERATOR;
                int v = j + 1, e = v; while (e < len && s[e] != ' ' && s[e] != '\t' && s[e] != '\r') e++;
                bool isnum = v < e && numeric_start(s, v, len);
                if (v < e && s[v] == '"') { int q = v + 1; while (q < e && s[q] != '"') q++; if (q < e) q++; fill(o, v, q, HC_STRING); e = q; }
                else for (int k = v; k < e; k++) o[k] = isnum ? HC_NUMBER : (s[k] == ',' ? HC_PUNCT : HC_CONST);
                if (n == 4 && !strncmp(w, "mark", 4)) fill(o, v, e, HC_LABEL);
                if (n == 4 && !strncmp(w, "inst", 4) && !isnum) fill(o, v, e, HC_TYPE);
                i = e; continue;
            }
            if (was_first && in_set(MUS_STRUCT, w, n)) { cls = HC_DECL; snprintf(head, sizeof head, "%.*s", n < 15 ? n : 15, w); ctx = !strcmp(head, "track") ? 1 : !strcmp(head, "pattern") || !strcmp(head, "sample") ? 4 : !strcmp(head, "mark") ? 5 : 0; }
            else if (was_first && in_set(MUS_TEXTDIR, w, n)) { cls = HC_DECL; ctx = 6; }
            else if (was_first && in_set(MUS_OPT, w, n)) { cls = HC_KEYWORD; if (n == 4 && !strncmp(w, "inst", 4)) ctx = 3; }
            else if (ctx == 1) { cls = HC_FUNCDEF; ctx = 2; }
            else if (ctx == 2 || ctx == 3) { cls = HC_TYPE; ctx = 0; }
            else if (ctx == 4) { cls = HC_FUNCDEF; ctx = 0; }
            else if (ctx == 5) { cls = HC_LABEL; ctx = 0; }
            else if (n == 1 && w[0] == '-') cls = HC_PUNCT;
            else if ((n == 1 && (w[0] == 'r')) || (n == 4 && !strncmp(w, "rest", 4))) cls = HC_PUNCT;
            else if (in_set(MUS_DYN, w, n)) cls = HC_CONST;
            else if (in_set(MUS_DRUM, w, n)) cls = HC_BUILTIN;
            else if (is_note_word(w, n)) cls = HC_NOTE;
            else if (in_set(MUS_INST, w, n)) cls = HC_TYPE;
            else if (in_set(MUS_OPT, w, n)) cls = HC_FIELD;
            fill(o, i, j, cls); i = j; continue;
        }
        o[i++] = HC_TEXT; first = false;
    }
    if (mode == HS_NORMAL) {                                              /* `sample ... data` opens an inline data block */
        int e = len; while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) e--;
        if (!strcmp(head, "sample") && e >= 4 && !strncmp(s + e - 4, "data", 4) && (e == 4 || s[e - 5] == ' ' || s[e - 5] == '\t')) return 5;
    }
    return (uint8_t)mode;
}

/* ---------------------------------------------------------------- KCM model */
static const char *const MOD_PRIM[] = { "box", "cyl", "sphere", "cone", "plane", "torus", NULL };
static const char *const MOD_FLAG[] = { "twosided", "unlit", "translucent", NULL };
static uint8_t hl_model(const char *s, int len, uint8_t st, uint8_t *o) {
    (void)st; int i = 0; bool first = true;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c == ' ' || c == '\t' || c == '\r') { o[i++] = HC_TEXT; continue; }
        if ((c == '#' && (i + 1 >= len || s[i + 1] == ' ')) || (c == '/' && i + 1 < len && s[i + 1] == '/')) { fill(o, i, len, HC_COMMENT); break; }
        int j = i; while (j < len && s[j] != ' ' && s[j] != '\t' && s[j] != '\r') j++;
        int n = j - i; const char *w = s + i;
        if (first) {
            uint8_t cls = HC_TEXT;
            if ((n == 7 && !strncmp(w, "kcmodel", 7)) || (n == 4 && !strncmp(w, "part", 4))) cls = HC_DECL;
            else if (n == 1 && (w[0] == 'v' || w[0] == 'f' || w[0] == 'n')) cls = HC_KEYWORD;
            else if (in_set(MOD_PRIM, w, n)) cls = HC_BUILTIN;
            fill(o, i, j, cls); first = false; i = j; continue;
        }
        first = false;
        const char *eq = memchr(w, '=', (size_t)n);
        if (eq) {
            int k = (int)(eq - w); fill(o, i, i + k, HC_FIELD); o[i + k] = HC_OPERATOR;
            bool color = (k == 5 && !strncmp(w, "color", 5)), tex = (k == 3 && !strncmp(w, "tex", 3));
            for (int q = i + k + 1; q < j; q++) o[q] = (color || tex) ? HC_STRING : (s[q] == ',' ? HC_PUNCT : (isdigit((unsigned char)s[q]) || s[q] == '-' || s[q] == '.' || s[q] == 'e') ? HC_NUMBER : HC_CONST);
        } else if (in_set(MOD_FLAG, w, n)) fill(o, i, j, HC_CONST);
        else if (numeric_start(s, i, len)) fill(o, i, j, HC_NUMBER);
        else fill(o, i, j, HC_TEXT);
        i = j;
    }
    return 0;
}

/* ---------------------------------------------------------------- SSM (SSML) */
static const char *const SSML_TAGS[] = { "speak", "break", "prosody", "emphasis", "phoneme", "sub", "say-as", "voice", "mark", "p", "s", "lang", "audio", "w", "lexicon", NULL };
static uint8_t hl_ssml(const char *s, int len, uint8_t st, uint8_t *o) {
    int mode = st & 7, quote = (st >> 3) & 1, i = 0;
    while (i < len) {
        if (mode == HS_BLOCK) {
            int j = i; while (j < len && !(s[j] == '-' && j + 2 < len + 0 && j + 2 <= len - 1 + 0 && s[j + 1] == '-' && s[j + 2] == '>')) j++;
            if (j < len) { j += 3; mode = HS_NORMAL; } fill(o, i, j, HC_COMMENT); i = j; continue;
        }
        if (mode == 7) {                                                    /* inside an attribute value */
            char q = quote ? '\'' : '"';
            while (i < len && s[i] != q) {
                if (s[i] == '&') { int j = i + 1; while (j < len && j < i + 10 && s[j] != ';' && s[j] != q) j++; if (j < len && s[j] == ';') { fill(o, i, j + 1, HC_ENTITY); i = j + 1; continue; } }
                o[i++] = HC_STRING;
            }
            if (i < len) { o[i++] = HC_STRING; mode = 6; }
            continue;
        }
        if (mode == 6) {                                                    /* inside <tag ...> */
            unsigned char c = (unsigned char)s[i];
            if (c == ' ' || c == '\t' || c == '\r') { o[i++] = HC_TEXT; continue; }
            if (c == '/' && i + 1 < len && s[i + 1] == '>') { fill(o, i, i + 2, HC_OPERATOR); i += 2; mode = HS_NORMAL; continue; }
            if (c == '>' || c == '?') { o[i++] = HC_OPERATOR; if (c == '>') mode = HS_NORMAL; continue; }
            if (c == '=') { o[i++] = HC_OPERATOR; continue; }
            if (c == '"' || c == '\'') { quote = c == '\''; o[i++] = HC_STRING; mode = 7; continue; }
            int j = i; while (j < len && (isalnum((unsigned char)s[j]) || s[j] == '-' || s[j] == '_' || s[j] == ':')) j++;
            if (j == i) j = i + 1;
            fill(o, i, j, HC_ATTR); i = j; continue;
        }
        unsigned char c = (unsigned char)s[i];
        if (c == '<') {
            if (i + 3 < len && !strncmp(s + i, "<!--", 4)) { int j = i + 4; while (j + 2 < len && strncmp(s + j, "-->", 3)) j++; if (j + 2 < len) { j += 3; } else { j = len; mode = HS_BLOCK; } fill(o, i, j, HC_COMMENT); i = j; continue; }
            if (i + 1 < len && s[i + 1] == '?') { int j = i + 2; while (j + 1 < len && strncmp(s + j, "?>", 2)) j++; j = j + 1 < len ? j + 2 : len; fill(o, i, j, HC_DECL); i = j; continue; }
            int j = i + 1; o[i] = HC_OPERATOR; if (j < len && s[j] == '/') { o[j] = HC_OPERATOR; j++; }
            int a = j; while (j < len && (isalnum((unsigned char)s[j]) || s[j] == '-' || s[j] == '_' || s[j] == ':')) j++;
            fill(o, a, j, in_set(SSML_TAGS, s + a, j - a) ? HC_TAG : (j > a ? HC_TYPE : HC_TEXT)); i = j; mode = 6; continue;
        }
        if (c == '&') { int j = i + 1; while (j < len && j < i + 10 && s[j] != ';' && s[j] != ' ' && s[j] != '<') j++; if (j < len && s[j] == ';') { fill(o, i, j + 1, HC_ENTITY); i = j + 1; continue; } }
        o[i++] = HC_TEXT;
    }
    return (uint8_t)(mode | (mode == 7 ? quote << 3 : 0));
}

uint8_t hl_line_lang(Lang l, const char *s, int len, uint8_t st, uint8_t *out) {
    switch (l) {
    case LANG_KC: return hl_line(s, len, st, out);
    case LANG_KCM_MUSIC: return hl_music(s, len, st, out);
    case LANG_KCM_MODEL: return hl_model(s, len, st, out);
    case LANG_SSM: return hl_ssml(s, len, st, out);
    default: memset(out, HC_TEXT, (size_t)len + 1); return 0;
    }
}

/* ---------------------------------------------------------------- completion vocabularies */
static const char *const W_MUSIC[] = { "title", "composer", "tempo", "time", "key", "track", "pattern", "end", "sample", "include", "length", "mark", "at", "wait", "inst", "vol", "pan", "reverb", "delay",
    "delay_time", "delay_fb", "chorus", "lowpass", "highpass", "swing", "swing16", "transpose", "strum", "gate", "vel", "glide", "adsr", "vibrato", "detune", "octave", "dyn", "gain", "root", "bpm",
    "oneshot", "reverse", "cut", "data", "strict_bars", "sine", "square", "saw", "triangle", "pulse", "organ", "piano", "epiano", "bell", "pluck", "bass", "strings", "pad", "brass", "flute", "lead",
    "marimba", "xylophone", "harp", "choir", "noise", "808", "cowbell", "clav", "slap", "wah", "stab", "drums", "kick", "snare", "rim", "clap", "hat", "ohat", "tom_lo", "tom_mid", "tom_hi", "crash",
    "ride", "shaker", "tambourine", "conga", "clave", "kick808", "snap", "rest", NULL };
static const char *const W_MODEL[] = { "kcmodel", "part", "v", "f", "n", "box", "cyl", "cone", "sphere", "plane", "torus", "color=", "tex=", "twosided", "unlit", "translucent", "at=", "rot=", "seg=", NULL };
static const char *const W_SSM[] = { "speak", "break", "prosody", "emphasis", "phoneme", "sub", "say-as", "voice", "mark", "p", "s", "rate", "pitch", "volume", "time", "strength", "level", "alphabet", "ph", "alias", "interpret-as", NULL };
const char *const *lang_words(Lang l) { return l == LANG_KCM_MUSIC ? W_MUSIC : l == LANG_KCM_MODEL ? W_MODEL : l == LANG_SSM ? W_SSM : NULL; }
