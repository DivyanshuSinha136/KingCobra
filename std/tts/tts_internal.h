/* KingCobra std.tts — internal shared declarations.
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#ifndef TTS_INTERNAL_H
#define TTS_INTERNAL_H
#include "../music_note/mn_internal.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* portable strndup (MinGW / older C runtimes do not provide it) */
static inline char *tts_strndup(const char *s, size_t n) {
    size_t l = 0; while (l < n && s[l]) l++;
    char *r = (char *)malloc(l + 1); if (!r) return NULL;
    memcpy(r, s, l); r[l] = 0; return r;
}

/* ------------------------------------------------------------ phones (ARPAbet + a few extras) */
enum {
    P_NONE = 0,
    P_AA, P_AE, P_AH, P_AO, P_AW, P_AY, P_EH, P_ER, P_EY, P_IH, P_IY, P_OW, P_OY, P_UH, P_UW,   /* vowels 1..15 */
    P_B, P_CH, P_D, P_DH, P_F, P_G, P_HH, P_JH, P_K, P_L, P_M, P_N, P_NG, P_P, P_R, P_S, P_SH, P_T, P_TH, P_V, P_W, P_Y, P_Z, P_ZH,
    P_DX,      /* alveolar flap (US "water") */
    P_Q,       /* glottal stop */
    P_PAUSE,   /* silence */
    P_COUNT
};
typedef enum { C_SIL, C_VOWEL, C_DIPH, C_STOP, C_AFFR, C_FRIC, C_NASAL, C_LIQ, C_GLIDE, C_ASP, C_FLAP } TtsCls;
#define ST_UNSET 3        /* vowel stress not decided yet (rule-generated word) */

typedef struct { uint8_t ph; uint8_t st; } TtsPh;   /* st: 0 none/unstressed, 1 primary, 2 secondary, 3 unset */

typedef struct {
    const char *arpa, *ipa;
    uint8_t cls, voiced, place, viseme;
    int16_t dur, dmin;                 /* nominal / minimum ms (stops, affricates: closure) */
    int16_t vot;                       /* stops: aspiration ms; affricates: frication ms */
    int16_t f1, f2, f3, g1, g2, g3;    /* formant target (stops/consonants: locus); g = glide end for diphthongs */
    int16_t b1, b2, b3;                /* bandwidths */
    int16_t av;                        /* voicing level, dB re vowel (0 = unvoiced) */
    int16_t fc, fbw, fa;               /* frication/burst: centre Hz, bandwidth Hz, level dB */
    int16_t fz;                        /* nasal anti-resonance Hz (0 = none) */
} TtsPhone;
extern const TtsPhone TTS_PH[P_COUNT];

static inline bool tts_is_vowel(int p) { return p >= P_AA && p <= P_UW; }
static inline bool tts_is_voiced_cons(int p) { return p >= P_B && p < P_PAUSE && TTS_PH[p].voiced && !tts_is_vowel(p); }
int  tts_ph_find(const char *name);                             /* 0 if unknown (name without stress digit) */
int  tts_phseq_parse(const char *s, TtsPh *out, int cap, char *bad, size_t badn);   /* tokens "AH0 B AW1 T"; -1 on error; vowels without a digit = primary stress */
int  tts_phseq_parse_ex(const char *s, TtsPh *out, int cap, char *bad, size_t badn, int def_stress);
int  tts_ipa_parse(const char *s, TtsPh *out, int cap);          /* IPA string -> phones; -1 on unknown symbol */
char *tts_phseq_format(const TtsPh *p, int n, bool ipa, bool stress_digits);   /* malloc'd */
bool tts_voice_is_front(int ph);

/* ------------------------------------------------------------ voices */
typedef struct {
    char name[24]; char desc[72];
    double f0, range, tract, oq, tilt, breath, jitter, shimmer, rate, bw;
    bool whisper, robot;
} TtsVoice;
int  tts_voice_find(const char *name);                          /* -1 if unknown */
int  tts_voice_count(void);
TtsVoice *tts_voice_at(int i);
int  tts_voice_define(const char *name, const char *base, KcValue opts, KcPos pos);   /* returns index */

/* ------------------------------------------------------------ vocal styles ([happy] [sad] [whisper] ...) */
typedef struct {
    float pitch;      /* semitones */
    float range;      /* intonation range x */
    float rate;       /* speed x */
    float vol;        /* gain x */
    float breath;     /* + breathiness */
    float whisper;    /* 0..1: voicing -> noise */
    float tilt;       /* + spectral tilt (darker) / - (brighter) */
    float oq;         /* + open quotient (breathy) / - (pressed) */
    float jitter, shimmer;
    float tremor, tremor_hz;   /* pitch tremor depth (semitones) and rate */
    float creak;      /* 0..1 vocal fry */
    float formant;    /* vocal tract scale x (smile > 1) */
    float pause;      /* pause length x */
    float accent;     /* pitch accent height x */
    float decl;       /* declination x */
    float artic;      /* consonant strength x */
    float fall;       /* final fall depth x */
    float lilt;       /* sing-song alternation of accents 0..1 */
    float amod;       /* amplitude modulation depth (laughing speech) */
} TtsStyle;
void tts_style_neutral(TtsStyle *s);

/* style registry + tag parser (tts_style.c) */
enum { TOP_STYLE = 0, TOP_EVENT, TOP_INTERJ, TOP_PAUSE, TOP_RESET, TOP_VOICE, TOP_PARAM };
typedef struct { int type, idx, field; float amount, value; char name[24]; } TagOp;
typedef struct { TagOp op[8]; int n; bool add, closing; } TagParse;
bool tts_tag_parse(const char *inner, TagParse *out, char *suggest, size_t sn);   /* false: not a tag */
int  tts_style_count(void);
const char *tts_style_name(int i);
const char *tts_style_cat(int i);
const char *tts_style_desc(int i);
const TtsStyle *tts_style_at(int i);
int  tts_style_find(const char *name);                     /* exact name or alias; fuzzy for typos */
const char *tts_style_canon(const char *text);             /* canonical tag name for a (mis)spelled tag, or NULL */
int  tts_style_define(const char *name, const char *base, KcValue opts, KcPos pos);
int  tts_event_count(void);
const char *tts_event_name(int i);
const char *tts_interj_name(int i);
int  tts_interj_count(void);
const char *tts_interj_phones(int i, float *pitch, float *stretch, const char **mood);
int  tts_style_field(const char *name);                    /* parameter name -> field index, -1 */
void tts_style_set_field(TtsStyle *s, int field, float v);
float tts_style_get_field(const TtsStyle *s, int field);
void tts_style_compose(TtsStyle *out, const TtsStyle *const *st, const float *amount, int n);

/* ------------------------------------------------------------ options */
typedef struct {
    int voice;
    double rate, pitch_st, f0, range, volume, pause_scale, comma_ms, period_ms, para_ms;
    double reverb, breath, emph_gain, tilt;
    int sr;
    bool flap, uk, normalize, ssml, ssml_auto, caps_emph, trim, dateorder_dmy, and_word, year_cardinal, spell_unknown;
    bool tags, strict_tags, slang;
    char style[48]; double style_amount;
    uint32_t seed;
} TtsOpts;
void tts_opts_default(TtsOpts *o);
void tts_opts_parse(TtsOpts *o, KcValue v, KcPos pos, const char *who);

/* ------------------------------------------------------------ words / tokens */
enum { WK_WORD = 0, WK_PUNCT, WK_BREAK, WK_MARK, WK_PHONES, WK_EVENT, WK_STYLE };
enum { SRC_LEXICON = 0, SRC_DICT, SRC_MORPH, SRC_COMPOUND, SRC_RULES, SRC_SPELL, SRC_PHONES, SRC_HOMOGRAPH, SRC_ACRONYM, SRC_PUNCT };
typedef struct {
    char *text;               /* lower-case orthography (WK_WORD), punctuation char (WK_PUNCT), mark name (WK_MARK) */
    TtsPh *ph; int nph;
    uint8_t kind, src, content, emph, caps;
    char punc;
    float brk_ms;
    float rate, pitch_st, vol;
    int voice;                /* voice override (-1 = utterance voice) */
    int para;                 /* 1 = paragraph break follows this punctuation */
    int style;                /* index into TtsWords.sty (0 = neutral) */
    int event;                /* WK_EVENT: event id */
    float amount;             /* WK_EVENT: intensity / length multiplier */
} TtsWord;
typedef struct { TtsWord *w; int n, cap; TtsStyle *sty; int nsty, capsty; } TtsWords;
int tts_words_style(TtsWords *ws, const TtsStyle *s);   /* intern a resolved style, returns its index */
void tts_words_free(TtsWords *ws);
TtsWord *tts_words_push(TtsWords *ws, int kind, const char *text);

/* text front end (tts_text.c) */
typedef struct { char *d; size_t n, cap; } TBuf;
void tb_init(TBuf *b);
void tb_free(TBuf *b);
void tb_putc(TBuf *b, char c);
void tb_puts(TBuf *b, const char *s);
void tb_printf(TBuf *b, const char *fmt, ...);
char *tb_take(TBuf *b);
char *tts_normalize(const char *text, const TtsOpts *o);        /* malloc'd, plain ASCII text with punctuation */
void  tts_number_words(TBuf *b, long double v, int mode, const TtsOpts *o);   /* mode: 0 cardinal 1 ordinal 2 year 3 digits */
void  tts_cardinal_ll(TBuf *b, long long v, bool andword);
void  tts_ordinal_ll(TBuf *b, long long v, bool andword);
void  tts_year_words(TBuf *b, int y);
void  tts_tokenize(const char *text, const TtsOpts *o, TtsWords *out, KcPos pos);   /* normalise + SSML + markup -> tokens with phonemes */

/* g2p (tts_g2p.c) */
void tts_g2p_init(void);
void tts_g2p_word(TtsWord *w, const char *prev, const char *next, const TtsOpts *o);   /* fills ph/nph/src from w->text */
bool tts_lex_lookup(const char *word, TtsPh *out, int *n, int cap, int *src);          /* user lexicon + builtin dictionary only */
void tts_lex_add(const char *word, const TtsPh *ph, int n);
bool tts_lex_remove(const char *word);
void tts_lex_clear(void);
int  tts_lex_size(void);
int  tts_dict_size(void);
int  tts_lex_load_text(const char *text, KcPos pos, const char *who, int *errors);      /* CMUdict / "word=pron" lines */
char *tts_lex_dump(void);                                                              /* user lexicon as text (malloc'd) */
void tts_add_sub(const char *from, const char *to);
bool tts_sub_remove(const char *from);
void tts_sub_clear(void);
const char *tts_sub_lookup(const char *word);
void tts_spell_word(const char *w, TtsPh *out, int *n, int cap);
bool tts_is_function_word(const char *w);
int  tts_syllables(const TtsPh *p, int n);
void tts_assign_stress(TtsPh *p, int n, const char *word);
extern const char *const TTS_DICT[];                                                     /* tts_dict.c */
extern const int TTS_DICT_N;

/* ------------------------------------------------------------ planning + synthesis */
typedef struct {
    uint8_t ph, sub, st;        /* sub: 0 whole, 1 closure, 2 burst/release */
    int word;                   /* index into the word list (-1 for pauses) */
    int voice;
    float dur, t0;              /* ms */
    float fS[3], fE[3];         /* F1..F3 start/end targets (Hz, unscaled) */
    float bw[3];
    float avS, avE, afS, afE, ahS, ahE;   /* dB */
    float fc, fbw;
    float nasal, fz;
    float tin, tout;            /* formant ramp ms */
    float vol, pit;             /* word volume gain, pitch offset (semitones) */
    uint8_t vmod;               /* frication modulated by voicing */
    uint8_t flags;              /* SEGF_* */
    uint8_t accent;             /* pitch-accent nucleus */
    uint8_t soft;               /* soft amplitude ramps (fricatives) */
    int ip;                     /* intonation phrase index */
    int pk;                     /* phone index inside its word (for alignment) */
    int sty;                    /* style index into the plan's style table */
} TtsSeg;
#define SEGF_NOFORM 1           /* pauses: formant tracks are not touched */
typedef struct {
    TtsSeg *s; int n, cap;
    float *f0t; float *f0v; int nf0, capf0;      /* F0 anchors: time ms, semitone offset */
    float total_ms;
    int *mark_seg; char **mark_name; int nmark;
    TtsStyle *sty; int nsty;
} TtsPlan;
void tts_plan_build(const TtsWords *ws, const TtsOpts *o, TtsPlan *pl);
void tts_plan_free(TtsPlan *pl);
MnAudio *tts_synth(const TtsPlan *pl, const TtsOpts *o, KcPos pos, const char *who);

/* api helpers used across files */
KcValue tts_audio_value(MnAudio *a);
#endif
