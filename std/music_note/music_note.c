/* KingCobra std.music_note — music theory, multiline notation, synthesis, full DSP, WAV/MIDI.
 *
 * Pure native, zero external dependencies:
 *   - theory:    notes <-> MIDI <-> frequency, scales, chords, chord identification, transposition
 *   - notation:  a multiline, multi-track score language (tempo maps, key signatures, chords,
 *                ties, tuplets, repeats, dynamics, drums) parsed into a score handle
 *   - synthesis: ~25 instruments + 16 drum voices, ADSR, oscillators, FM, Karplus-Strong, noise
 *   - DSP:       biquad filters/EQ, delay, Freeverb reverb, chorus/flanger/phaser, tremolo/vibrato,
 *                distortion, bitcrusher, compressor, limiter, gate, convolution, time-stretch,
 *                pitch-shift, resample, mix/slice/fade/normalize, FFT, pitch + onset detection
 *   - files:     WAV read/write (8/16/24/32-bit int, 32-bit float), Standard MIDI File export
 *
 * This file: shared argument/option helpers, the audio handle, theory functions and the module table.
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "mn_internal.h"

/* ============================================================ argument helpers */

void mn_need(int n, int lo, int hi, KcPos pos, const char *usage) {
    if (n < lo || n > hi) kc_error_at(pos, "usage: music_note.%s", usage);
}
bool mn_is_num(KcValue v) { return v.type == VAL_INT || v.type == VAL_FLOAT; }
double mn_num(KcValue v, KcPos pos, const char *who, const char *what) {
    if (v.type == VAL_INT) return (double)v.as.i;
    if (v.type == VAL_FLOAT) return v.as.f;
    kc_error_at(pos, "music_note.%s: %s must be a number, got %s", who, what, kc_type_name(v));
    return 0;
}
int64_t mn_int(KcValue v, KcPos pos, const char *who, const char *what) {
    if (v.type == VAL_INT) return v.as.i;
    if (v.type == VAL_FLOAT) return (int64_t)llround(v.as.f);
    kc_error_at(pos, "music_note.%s: %s must be a number, got %s", who, what, kc_type_name(v));
    return 0;
}
const char *mn_str(KcValue v, KcPos pos, const char *who, const char *what) {
    if (v.type != VAL_STRING) kc_error_at(pos, "music_note.%s: %s must be a string, got %s", who, what, kc_type_name(v));
    return v.as.s->data;
}
/* ---- options: a "key=value key2=value2" string (the KingCobra convention; there is no object-literal syntax),
 *      or an object / struct value (e.g. from json.parse). The first bare word of a string is stored as "type"
 *      so effect specs like "reverb mix=0.3" work; other bare words are flags (true). Values containing ',' are lists. */
static KcValue opt_scalar(const char *v) {
    if (!strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on")) return kc_bool(true);
    if (!strcasecmp(v, "false") || !strcasecmp(v, "no") || !strcasecmp(v, "off")) return kc_bool(false);
    char *e; double d = strtod(v, &e);
    if (*v && !*e && e != v) {
        if (!strpbrk(v, ".eEnN") && fabs(d) < 9e15) return kc_int((int64_t)d);
        return kc_float(d);
    }
    return kc_string(v);
}
static KcValue opt_value(const char *v) {
    if (!strchr(v, ',')) return opt_scalar(v);
    KcValue arr = kc_array_new(); char b[256]; size_t n = strlen(v); if (n >= sizeof b) n = sizeof b - 1;
    memcpy(b, v, n); b[n] = 0; char *sv = NULL;
    for (char *t = mn_tok(b, ",", &sv); t; t = mn_tok(NULL, ",", &sv)) kc_array_push(arr, opt_scalar(t));
    return arr;
}
/* next token of an options string; returns false at the end */
static bool opt_iter(const char **pp, bool *first, char *key, size_t kc, char *val, size_t vc) {
    const char *p = *pp;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (!*p) return false;
    const char *q = p; while (*q && *q != ' ' && *q != '\t' && *q != '\n' && *q != '\r') q++;
    const char *eq = memchr(p, '=', (size_t)(q - p));
    if (eq) {
        size_t kl = (size_t)(eq - p), vl = (size_t)(q - eq - 1);
        if (kl >= kc) kl = kc - 1;
        if (vl >= vc) vl = vc - 1;
        memcpy(key, p, kl); key[kl] = 0; memcpy(val, eq + 1, vl); val[vl] = 0;
    } else {
        size_t kl = (size_t)(q - p); if (kl >= kc) kl = kc - 1;
        if (*first) { snprintf(key, kc, "type"); memcpy(val, p, kl); val[kl] = 0; }
        else { memcpy(key, p, kl); key[kl] = 0; snprintf(val, vc, "true"); }
    }
    *first = false; *pp = q;
    return true;
}
bool mn_is_opts(KcValue v) {
    if (v.type == VAL_STRUCT) return true;
    return v.type == VAL_STRING && strchr(v.as.s->data, '=') != NULL;
}
KcValue *mn_opt_get(KcValue o, const char *key) {
    static KcValue ring[16]; static int ri = 0;
    if (o.type == VAL_STRUCT) {
        KcStructInst *s = o.as.strct;
        for (int i = 0; i < s->count; i++) if (!strcasecmp(s->names[i], key)) return &s->values[i];
        return NULL;
    }
    if (o.type == VAL_STRING) {
        const char *p = o.as.s->data; bool first = true; char k[64], v[256];
        while (opt_iter(&p, &first, k, sizeof k, v, sizeof v)) {
            if (!strcasecmp(k, key)) { KcValue *slot = &ring[ri++ & 15]; *slot = opt_value(v); return slot; }
            if (!strcasecmp(k, "type") && !strcasecmp(v, key)) { KcValue *slot = &ring[ri++ & 15]; *slot = kc_bool(true); return slot; }   /* bare flag as first word */
        }
    }
    return NULL;
}
/* copy any options value into a fresh open object */
KcValue mn_opts_object(KcValue o) {
    KcValue out = kc_object_new();
    if (o.type == VAL_STRUCT) {
        KcStructInst *s = o.as.strct;
        for (int i = 0; i < s->count; i++) kc_object_set(out, s->names[i], strlen(s->names[i]), s->values[i]);
    } else if (o.type == VAL_STRING) {
        const char *p = o.as.s->data; bool first = true; char k[64], v[256];
        while (opt_iter(&p, &first, k, sizeof k, v, sizeof v)) kc_object_set(out, k, strlen(k), opt_value(v));
    }
    return out;
}
double mn_opt_num(KcValue o, const char *key, double def, KcPos pos, const char *who) {
    KcValue *v = mn_opt_get(o, key);
    if (!v || v->type == VAL_NULL) return def;
    return mn_num(*v, pos, who, key);
}
int64_t mn_opt_int(KcValue o, const char *key, int64_t def, KcPos pos, const char *who) {
    KcValue *v = mn_opt_get(o, key);
    if (!v || v->type == VAL_NULL) return def;
    return mn_int(*v, pos, who, key);
}
bool mn_opt_bool(KcValue o, const char *key, bool def, KcPos pos, const char *who) {
    KcValue *v = mn_opt_get(o, key);
    if (!v || v->type == VAL_NULL) return def;
    if (v->type == VAL_BOOL) return v->as.b;
    if (v->type == VAL_INT) return v->as.i != 0;
    kc_error_at(pos, "music_note.%s: option '%s' must be true or false", who, key);
    return def;
}
const char *mn_opt_str(KcValue o, const char *key, const char *def, KcPos pos, const char *who) {
    KcValue *v = mn_opt_get(o, key);
    if (!v || v->type == VAL_NULL) return def;
    if (v->type == VAL_INT || v->type == VAL_FLOAT) {          /* "instrument=808": a numeric-looking word is still a word */
        static char ring[4][32]; static int ri = 0; char *b = ring[ri++ & 3];
        if (v->type == VAL_INT) snprintf(b, 32, "%lld", (long long)v->as.i); else snprintf(b, 32, "%g", v->as.f);
        return b;
    }
    return mn_str(*v, pos, who, key);
}
double mn_db2lin(double db) { return pow(10.0, db / 20.0); }
double mn_lin2db(double lin) { return lin <= 1e-12 ? -240.0 : 20.0 * log10(lin); }

/* ============================================================ audio handle */

MnAudio *mn_audio_new(size_t frames, int ch, int rate, KcPos pos, const char *who) {
    if (ch < 1 || ch > 8) kc_error_at(pos, "music_note.%s: channels must be 1..8", who);
    if (rate < 1000 || rate > 384000) kc_error_at(pos, "music_note.%s: sample rate must be 1000..384000", who);
    if (frames > MN_MAX_SAMPLES / (size_t)ch) kc_error_at(pos, "music_note.%s: audio too long (limit %zu samples)", who, (size_t)MN_MAX_SAMPLES);
    MnAudio *a = kc_malloc(sizeof *a);
    a->frames = frames; a->ch = ch; a->rate = rate;
    a->d = kc_malloc(frames * (size_t)ch * sizeof(float));
    memset(a->d, 0, frames * (size_t)ch * sizeof(float));
    return a;
}
MnAudio *mn_audio_clone(const MnAudio *s, KcPos pos, const char *who) {
    MnAudio *a = mn_audio_new(s->frames, s->ch, s->rate, pos, who);
    memcpy(a->d, s->d, s->frames * (size_t)s->ch * sizeof(float));
    return a;
}
void mn_audio_free(MnAudio *a) { if (a) { free(a->d); free(a); } }
void mn_audio_resize(MnAudio *a, size_t frames, KcPos pos, const char *who) {
    if (frames > MN_MAX_SAMPLES / (size_t)a->ch) kc_error_at(pos, "music_note.%s: audio too long", who);
    a->d = kc_realloc(a->d, frames * (size_t)a->ch * sizeof(float));
    if (frames > a->frames) memset(a->d + a->frames * (size_t)a->ch, 0, (frames - a->frames) * (size_t)a->ch * sizeof(float));
    a->frames = frames;
}
KcValue mn_audio_wrap(MnAudio *a) {
    KcHandle *h = kc_malloc(sizeof *h);
    h->kind = H_AUDIO; h->ptr = a; h->size = 0; h->extra = 0;
    KcValue v; v.type = VAL_HANDLE; v.as.handle = h;
    return v;
}
bool mn_is_audio(KcValue v) { return v.type == VAL_HANDLE && v.as.handle->kind == H_AUDIO; }
MnAudio *mn_audio_arg(KcValue v, KcPos pos, const char *who) {
    if (!mn_is_audio(v)) kc_error_at(pos, "music_note.%s: expected an audio buffer (from render/tone/load_wav/...), got %s", who, kc_type_name(v));
    if (!v.as.handle->ptr) kc_error_at(pos, "music_note.%s: the audio buffer was already freed", who);
    return (MnAudio *)v.as.handle->ptr;
}

/* ============================================================ note names */

static const int LETTER_PC[7] = { 0, 2, 4, 5, 7, 9, 11 };   /* C D E F G A B */

const char *mn_note_scan(const char *s, MnNote *o) {
    char c = (char)toupper((unsigned char)s[0]);
    if (c < 'A' || c > 'G') return NULL;
    static const int LIDX[7] = { 5, 6, 0, 1, 2, 3, 4 };        /* A B C D E F G -> C-based index */
    o->letter = LIDX[c - 'A'];
    o->pc = LETTER_PC[o->letter];
    o->acc = 0; o->has_acc = false; o->natural = false; o->has_oct = false; o->octave = 4;
    const char *p = s + 1;
    for (;; p++) {
        if (*p == '#' || *p == 's') { o->acc += 1; o->has_acc = true; }
        else if (*p == 'x') { o->acc += 2; o->has_acc = true; }
        else if (*p == 'b') { o->acc -= 1; o->has_acc = true; }
        else if (*p == 'n') { o->natural = true; o->has_acc = true; }
        else break;
    }
    if (isdigit((unsigned char)*p) || (*p == '-' && isdigit((unsigned char)p[1]))) {
        char *e; long v = strtol(p, &e, 10);
        o->octave = (int)v; o->has_oct = true; p = e;
    }
    return p;
}
int mn_note_midi(const MnNote *n, int default_octave) {
    return ((n->has_oct ? n->octave : default_octave) + 1) * 12 + n->pc + n->acc;
}
const char *mn_midi_name(int midi, bool flats, char *buf) {
    static const char *SH[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    static const char *FL[12] = { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" };
    int pc = ((midi % 12) + 12) % 12;
    int oct = (midi - pc) / 12 - 1;
    sprintf(buf, "%s%d", flats ? FL[pc] : SH[pc], oct);
    return buf;
}
double mn_midi_freq(double midi, double a4) { return a4 * pow(2.0, (midi - 69.0) / 12.0); }

/* A note-name or number argument as a (possibly fractional) midi value. */
static double arg_midi(KcValue v, KcPos pos, const char *who) {
    if (v.type == VAL_INT) return (double)v.as.i;
    if (v.type == VAL_FLOAT) return v.as.f;
    const char *s = mn_str(v, pos, who, "note");
    MnNote n; const char *e = mn_note_scan(s, &n);
    if (!e || *e) kc_error_at(pos, "music_note.%s: '%s' is not a note name (examples: \"A4\", \"C#5\", \"Bb3\")", who, s);
    return mn_note_midi(&n, 4);
}

/* ============================================================ key signatures */

int mn_key_acc(int fifths, int letter) {
    static const int SH[7] = { 3, 0, 4, 1, 5, 2, 6 }, FL[7] = { 6, 2, 5, 1, 4, 0, 3 };
    if (fifths > 0) { for (int i = 0; i < fifths && i < 7; i++) if (SH[i] == letter) return 1; }
    else if (fifths < 0) { for (int i = 0; i < -fifths && i < 7; i++) if (FL[i] == letter) return -1; }
    return 0;
}
bool mn_parse_key(const char *s, int *fifths, bool *minor) {
    while (*s == ' ') s++;
    char c = (char)toupper((unsigned char)s[0]);
    int base;
    switch (c) { case 'C': base = 0; break; case 'G': base = 1; break; case 'D': base = 2; break; case 'A': base = 3; break;
                 case 'E': base = 4; break; case 'B': base = 5; break; case 'F': base = -1; break; default: return false; }
    const char *p = s + 1; int acc = 0;
    if (*p == '#') { acc = 1; p++; } else if (*p == 'b') { acc = -1; p++; }
    while (*p == ' ') p++;
    bool mn;
    if (!*p || !strcmp(p, "maj") || !strcmp(p, "major") || !strcmp(p, "M")) mn = false;
    else if (!strcmp(p, "m") || !strcmp(p, "min") || !strcmp(p, "minor")) mn = true;
    else return false;
    int f = base + 7 * acc - (mn ? 3 : 0);
    if (f < -7 || f > 7) return false;
    *fifths = f; *minor = mn;
    return true;
}

/* ============================================================ scales + chords */

typedef struct { const char *name; int n; int iv[12]; } ScaleDef;
static const ScaleDef SCALES[] = {
    { "major", 7, { 0, 2, 4, 5, 7, 9, 11 } }, { "ionian", 7, { 0, 2, 4, 5, 7, 9, 11 } },
    { "minor", 7, { 0, 2, 3, 5, 7, 8, 10 } }, { "natural_minor", 7, { 0, 2, 3, 5, 7, 8, 10 } }, { "aeolian", 7, { 0, 2, 3, 5, 7, 8, 10 } },
    { "harmonic_minor", 7, { 0, 2, 3, 5, 7, 8, 11 } }, { "melodic_minor", 7, { 0, 2, 3, 5, 7, 9, 11 } },
    { "dorian", 7, { 0, 2, 3, 5, 7, 9, 10 } }, { "phrygian", 7, { 0, 1, 3, 5, 7, 8, 10 } },
    { "lydian", 7, { 0, 2, 4, 6, 7, 9, 11 } }, { "mixolydian", 7, { 0, 2, 4, 5, 7, 9, 10 } },
    { "locrian", 7, { 0, 1, 3, 5, 6, 8, 10 } },
    { "pentatonic_major", 5, { 0, 2, 4, 7, 9 } }, { "pentatonic_minor", 5, { 0, 3, 5, 7, 10 } },
    { "blues", 6, { 0, 3, 5, 6, 7, 10 } }, { "chromatic", 12, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 } },
    { "whole_tone", 6, { 0, 2, 4, 6, 8, 10 } }, { "diminished", 8, { 0, 2, 3, 5, 6, 8, 9, 11 } },
    { "diminished_half", 8, { 0, 1, 3, 4, 6, 7, 9, 10 } }, { "hungarian_minor", 7, { 0, 2, 3, 6, 7, 8, 11 } },
    { "phrygian_dominant", 7, { 0, 1, 4, 5, 7, 8, 10 } }, { "bebop_dominant", 8, { 0, 2, 4, 5, 7, 9, 10, 11 } },
    { "japanese_in", 5, { 0, 1, 5, 7, 8 } }, { "hirajoshi", 5, { 0, 2, 3, 7, 8 } },
};
#define NSCALES ((int)(sizeof SCALES / sizeof SCALES[0]))

typedef struct { const char *name; int n; int iv[7]; bool canon; } ChordDef;
static const ChordDef CHORDS[] = {
    { "", 3, { 0, 4, 7 }, true }, { "maj", 3, { 0, 4, 7 }, false }, { "M", 3, { 0, 4, 7 }, false },
    { "m", 3, { 0, 3, 7 }, true }, { "min", 3, { 0, 3, 7 }, false }, { "-", 3, { 0, 3, 7 }, false },
    { "dim", 3, { 0, 3, 6 }, true }, { "o", 3, { 0, 3, 6 }, false },
    { "aug", 3, { 0, 4, 8 }, true }, { "+", 3, { 0, 4, 8 }, false },
    { "sus2", 3, { 0, 2, 7 }, true }, { "sus4", 3, { 0, 5, 7 }, true }, { "sus", 3, { 0, 5, 7 }, false },
    { "5", 2, { 0, 7 }, true },
    { "6", 4, { 0, 4, 7, 9 }, true }, { "m6", 4, { 0, 3, 7, 9 }, true }, { "69", 5, { 0, 4, 7, 9, 14 }, true },
    { "7", 4, { 0, 4, 7, 10 }, true },
    { "maj7", 4, { 0, 4, 7, 11 }, true }, { "M7", 4, { 0, 4, 7, 11 }, false }, { "Maj7", 4, { 0, 4, 7, 11 }, false },
    { "m7", 4, { 0, 3, 7, 10 }, true }, { "min7", 4, { 0, 3, 7, 10 }, false }, { "-7", 4, { 0, 3, 7, 10 }, false },
    { "mM7", 4, { 0, 3, 7, 11 }, true }, { "mmaj7", 4, { 0, 3, 7, 11 }, false },
    { "dim7", 4, { 0, 3, 6, 9 }, true }, { "o7", 4, { 0, 3, 6, 9 }, false },
    { "m7b5", 4, { 0, 3, 6, 10 }, true }, { "aug7", 4, { 0, 4, 8, 10 }, true }, { "7#5", 4, { 0, 4, 8, 10 }, false },
    { "7b5", 4, { 0, 4, 6, 10 }, true }, { "7sus4", 4, { 0, 5, 7, 10 }, true }, { "7sus", 4, { 0, 5, 7, 10 }, false },
    { "9", 5, { 0, 4, 7, 10, 14 }, true }, { "maj9", 5, { 0, 4, 7, 11, 14 }, true }, { "M9", 5, { 0, 4, 7, 11, 14 }, false },
    { "m9", 5, { 0, 3, 7, 10, 14 }, true }, { "11", 6, { 0, 4, 7, 10, 14, 17 }, true }, { "m11", 6, { 0, 3, 7, 10, 14, 17 }, true },
    { "13", 6, { 0, 4, 7, 10, 14, 21 }, true }, { "maj13", 6, { 0, 4, 7, 11, 14, 21 }, true }, { "m13", 6, { 0, 3, 7, 10, 14, 21 }, true },
    { "add9", 4, { 0, 4, 7, 14 }, true }, { "madd9", 4, { 0, 3, 7, 14 }, true },
    { "7b9", 5, { 0, 4, 7, 10, 13 }, true }, { "7#9", 5, { 0, 4, 7, 10, 15 }, true }, { "7#11", 5, { 0, 4, 7, 10, 18 }, true },
    { "9sus4", 5, { 0, 5, 7, 10, 14 }, true },
};
#define NCHORDS ((int)(sizeof CHORDS / sizeof CHORDS[0]))

bool mn_chord_symbol(const char *s, int octave, int *midis, int *count, int cap, const char **end) {
    char c = (char)toupper((unsigned char)s[0]);
    int letter;
    switch (c) { case 'C': letter = 0; break; case 'D': letter = 1; break; case 'E': letter = 2; break; case 'F': letter = 3; break;
                 case 'G': letter = 4; break; case 'A': letter = 5; break; case 'B': letter = 6; break; default: return false; }
    const char *p = s + 1; int acc = 0;
    if (*p == '#') { acc = 1; p++; } else if (*p == 'b') { acc = -1; p++; }
    const char *q = p; while (*q && *q != '/') q++;
    size_t ql = (size_t)(q - p);
    const ChordDef *cd = NULL;
    for (int i = 0; i < NCHORDS; i++) if (strlen(CHORDS[i].name) == ql && !strncmp(CHORDS[i].name, p, ql)) { cd = &CHORDS[i]; break; }
    if (!cd) return false;
    int root = (octave + 1) * 12 + LETTER_PC[letter] + acc;
    int n = 0, bass = INT_MIN;
    const char *e = q;
    if (*q == '/') {
        MnNote bn; const char *be = mn_note_scan(q + 1, &bn);
        if (!be || bn.has_oct) return false;
        int diff = ((((root - bn.pc - bn.acc) % 12) + 12) % 12);
        bass = root - (diff == 0 ? 12 : diff);
        e = be;
    }
    if (bass != INT_MIN && n < cap) midis[n++] = bass;
    for (int i = 0; i < cd->n && n < cap; i++) midis[n++] = root + cd->iv[i];
    *count = n; if (end) *end = e;
    return true;
}

/* ============================================================ theory API */

#define SETK(o, k, v) kc_object_set((o), (k), strlen(k), (v))

static KcValue name_value(int midi, bool flats) { char b[16]; return kc_string(mn_midi_name(midi, flats, b)); }

/* freq(note|midi [, a4 = 440]) -> float */
static KcValue t_freq(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 2, pos, "freq(note | midi[, a4 = 440]) -> float");
    double a4 = n > 1 ? mn_num(a[1], pos, "freq", "a4") : 440.0;
    if (a4 < 100 || a4 > 1000) kc_error_at(pos, "music_note.freq: a4 tuning must be 100..1000 Hz");
    return kc_float(mn_midi_freq(arg_midi(a[0], pos, "freq"), a4));
}
static KcValue t_midi(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 1, pos, "midi(note) -> int");
    return kc_int((int64_t)llround(arg_midi(a[0], pos, "midi")));
}
static KcValue t_name(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 2, pos, "name(midi[, use_flats = false]) -> string");
    bool fl = n > 1 && kc_truthy(a[1]);
    return name_value((int)mn_int(a[0], pos, "name", "midi"), fl);
}
static KcValue t_from_freq(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 2, pos, "from_freq(hz[, a4 = 440]) -> {note, midi, cents, freq}");
    double hz = mn_num(a[0], pos, "from_freq", "hz"), a4 = n > 1 ? mn_num(a[1], pos, "from_freq", "a4") : 440.0;
    if (hz <= 0) kc_error_at(pos, "music_note.from_freq: frequency must be > 0");
    double m = 69.0 + 12.0 * log2(hz / a4);
    long nm = lround(m);
    KcValue o = kc_object_new();
    SETK(o, "note", name_value((int)nm, false)); SETK(o, "midi", kc_int(nm));
    SETK(o, "cents", kc_float((m - (double)nm) * 100.0)); SETK(o, "freq", kc_float(mn_midi_freq((double)nm, a4)));
    return o;
}
static KcValue transpose_one(KcValue v, int semis, KcPos pos) {
    if (v.type == VAL_INT) return kc_int(v.as.i + semis);
    if (v.type == VAL_FLOAT) return kc_float(v.as.f + semis);
    const char *s = mn_str(v, pos, "transpose", "note");
    if (!strcmp(s, "rest") || !strcmp(s, "-") || !strcmp(s, "r")) return v;
    MnNote nn; const char *e = mn_note_scan(s, &nn);
    if (!e || *e) kc_error_at(pos, "music_note.transpose: '%s' is not a note name", s);
    return name_value(mn_note_midi(&nn, 4) + semis, nn.acc < 0);
}
static KcValue t_transpose(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 2, 2, pos, "transpose(note | midi | notes[], semitones)");
    int semis = (int)mn_int(a[1], pos, "transpose", "semitones");
    if (a[0].type == VAL_ARRAY) {
        KcValue out = kc_array_new();
        for (int i = 0; i < a[0].as.arr->count; i++) kc_array_push(out, transpose_one(a[0].as.arr->items[i], semis, pos));
        return out;
    }
    return transpose_one(a[0], semis, pos);
}
static KcValue t_interval(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 2, 2, pos, "interval(a, b) -> int   (semitones from a up to b)");
    return kc_int((int64_t)llround(arg_midi(a[1], pos, "interval") - arg_midi(a[0], pos, "interval")));
}
static KcValue t_scale(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 3, pos, "scale(root[, kind = \"major\"[, octaves = 1]]) -> string[]");
    int root = (int)llround(arg_midi(a[0], pos, "scale"));
    const char *kind = n > 1 ? mn_str(a[1], pos, "scale", "kind") : "major";
    int oct = n > 2 ? (int)mn_int(a[2], pos, "scale", "octaves") : 1;
    if (oct < 1 || oct > 8) kc_error_at(pos, "music_note.scale: octaves must be 1..8");
    const ScaleDef *sd = NULL;
    for (int i = 0; i < NSCALES; i++) if (!strcmp(SCALES[i].name, kind)) { sd = &SCALES[i]; break; }
    if (!sd) kc_error_at(pos, "music_note.scale: unknown scale '%s' (see music_note.scales())", kind);
    bool fl = a[0].type == VAL_STRING && a[0].as.s->len > 1 && (a[0].as.s->data[1] == 'b' || a[0].as.s->data[0] == 'F');
    KcValue out = kc_array_new();
    for (int o = 0; o < oct; o++) for (int i = 0; i < sd->n; i++) kc_array_push(out, name_value(root + o * 12 + sd->iv[i], fl));
    kc_array_push(out, name_value(root + oct * 12, fl));
    return out;
}
static KcValue t_scales(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    KcValue out = kc_array_new();
    for (int i = 0; i < NSCALES; i++) kc_array_push(out, kc_string(SCALES[i].name));
    return out;
}
static KcValue t_chord(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 2, pos, "chord(symbol[, octave = 4]) -> string[]   (\"Cmaj7\", \"F#m\", \"Bb7\", \"Am/E\")");
    const char *s = mn_str(a[0], pos, "chord", "symbol");
    int oct = n > 1 ? (int)mn_int(a[1], pos, "chord", "octave") : 4;
    int m[16], cnt = 0; const char *e = NULL;
    if (!mn_chord_symbol(s, oct, m, &cnt, 16, &e) || (e && *e)) kc_error_at(pos, "music_note.chord: cannot read chord symbol '%s' (see music_note.chords())", s);
    bool fl = s[1] == 'b';
    KcValue out = kc_array_new();
    for (int i = 0; i < cnt; i++) kc_array_push(out, name_value(m[i], fl));
    return out;
}
static KcValue t_chords(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    KcValue out = kc_array_new();
    for (int i = 0; i < NCHORDS; i++) if (CHORDS[i].canon) kc_array_push(out, kc_string(CHORDS[i].name[0] ? CHORDS[i].name : "(major)"));
    return out;
}
/* identify(notes[]) -> string   e.g. ["C4","E4","G4","B4"] -> "Cmaj7"; "" when nothing matches */
static KcValue t_identify(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 1, pos, "identify(notes[]) -> string");
    if (a[0].type != VAL_ARRAY || a[0].as.arr->count < 2) kc_error_at(pos, "music_note.identify: expected an array of at least 2 notes");
    int cnt = a[0].as.arr->count; if (cnt > 24) cnt = 24;
    int low = INT_MAX; unsigned mask = 0;
    for (int i = 0; i < cnt; i++) {
        int m = (int)llround(arg_midi(a[0].as.arr->items[i], pos, "identify"));
        if (m < low) low = m;
        mask |= 1u << (((m % 12) + 12) % 12);
    }
    int lowpc = ((low % 12) + 12) % 12;
    static const char *PCN[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    for (int pass = 0; pass < 2; pass++) {
        for (int r = 0; r < 12; r++) {
            if (!(mask & (1u << r))) continue;
            if (pass == 0 && r != lowpc) continue;
            for (int c = 0; c < NCHORDS; c++) {
                if (!CHORDS[c].canon) continue;
                unsigned cm = 0;
                for (int i = 0; i < CHORDS[c].n; i++) cm |= 1u << ((r + CHORDS[c].iv[i]) % 12);
                if (cm != mask) continue;
                char out[48];
                snprintf(out, sizeof out, "%s%s", PCN[r], CHORDS[c].name);
                if (r != lowpc) { size_t o = strlen(out); snprintf(out + o, sizeof out - o, "/%s", PCN[lowpc]); }
                return kc_string(out);
            }
        }
    }
    return kc_string("");
}
/* key_signature("Gm") -> {fifths, minor, accidentals} */
static KcValue t_key_signature(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 1, pos, "key_signature(key) -> {fifths, minor, accidentals}");
    const char *s = mn_str(a[0], pos, "key_signature", "key");
    int f; bool mi;
    if (!mn_parse_key(s, &f, &mi)) kc_error_at(pos, "music_note.key_signature: unknown key '%s' (examples: \"G\", \"Bbm\", \"F#\")", s);
    static const char *SH[7] = { "F#", "C#", "G#", "D#", "A#", "E#", "B#" }, *FL[7] = { "Bb", "Eb", "Ab", "Db", "Gb", "Cb", "Fb" };
    KcValue acc = kc_array_new();
    for (int i = 0; i < (f > 0 ? f : -f); i++) kc_array_push(acc, kc_string(f > 0 ? SH[i] : FL[i]));
    KcValue o = kc_object_new();
    SETK(o, "fifths", kc_int(f)); SETK(o, "minor", kc_bool(mi)); SETK(o, "accidentals", acc);
    return o;
}
/* free(handle) — release an audio buffer or score */
static KcValue t_free(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 1, pos, "free(audio | score)");
    if (a[0].type == VAL_HANDLE && a[0].as.handle->kind == H_AUDIO) {
        if (a[0].as.handle->ptr) { mn_audio_free((MnAudio *)a[0].as.handle->ptr); a[0].as.handle->ptr = NULL; }
        return kc_null();
    }
    if (a[0].type == VAL_HANDLE && a[0].as.handle->kind == H_SCORE) {
        if (a[0].as.handle->ptr) { mn_score_free((MnScore *)a[0].as.handle->ptr); a[0].as.handle->ptr = NULL; }
        return kc_null();
    }
    kc_error_at(pos, "music_note.free: expected an audio buffer or score handle");
    return kc_null();
}

KcModuleEntry mn_theory_entries[] = {
    { "freq", t_freq }, { "midi", t_midi }, { "name", t_name }, { "from_freq", t_from_freq },
    { "transpose", t_transpose }, { "interval", t_interval }, { "scale", t_scale }, { "scales", t_scales },
    { "chord", t_chord }, { "chords", t_chords }, { "identify", t_identify }, { "key_signature", t_key_signature },
    { "free", t_free },
};
const int mn_theory_count = (int)(sizeof mn_theory_entries / sizeof mn_theory_entries[0]);

/* ============================================================ module table */

KcModule *kc_mod_music_note(void) {
    static KcModule m;
    static bool init = false;
    if (!init) {
        const KcModuleEntry *tabs[7] = { mn_theory_entries, mn_score_entries, mn_synth_entries, mn_dsp_entries, mn_analysis_entries, mn_io_entries, mn_groove_entries };
        const int cnts[7] = { mn_theory_count, mn_score_count, mn_synth_count, mn_dsp_count, mn_analysis_count, mn_io_count, mn_groove_count };
        int total = 0; for (int i = 0; i < 7; i++) total += cnts[i];
        KcModuleEntry *all = kc_malloc((size_t)total * sizeof *all);
        int k = 0; for (int i = 0; i < 7; i++) for (int j = 0; j < cnts[i]; j++) all[k++] = tabs[i][j];
        m.name = "music_note"; m.entries = all; m.count = total;
        init = true;
    }
    return &m;
}

/* text shown when an audio buffer or score handle is printed */
void mn_handle_display(const KcHandle *h, char *buf, size_t n) {
    if (h->kind == H_AUDIO) {
        const MnAudio *a = h->ptr;
        if (!a) snprintf(buf, n, "<audio (freed)>");
        else snprintf(buf, n, "<audio %.3fs %dch %dHz>", (double)a->frames / a->rate, a->ch, a->rate);
    } else {
        const MnScore *s = h->ptr;
        if (!s) snprintf(buf, n, "<score (freed)>");
        else snprintf(buf, n, "<score %d track%s%s%s>", s->ntr, s->ntr == 1 ? "" : "s", s->title[0] ? " " : "", s->title);
    }
}
