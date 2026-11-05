/* KingCobra std.music_note — internal shared declarations.
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#ifndef MN_INTERNAL_H
#define MN_INTERNAL_H
#include "interp.h"
#include <math.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#if defined(_WIN32)
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define MN_TAU 6.283185307179586476925287
#define MN_MAX_SAMPLES ((size_t)1 << 28)   /* total float samples per buffer (1 GiB) */
#define MN_RATE 44100

/* ------------------------------------------------------------ audio buffer */
typedef struct { float *d; size_t frames; int ch; int rate; } MnAudio;

MnAudio *mn_audio_new(size_t frames, int ch, int rate, KcPos pos, const char *who);
MnAudio *mn_audio_clone(const MnAudio *a, KcPos pos, const char *who);
void     mn_audio_free(MnAudio *a);
KcValue  mn_audio_wrap(MnAudio *a);
bool     mn_is_audio(KcValue v);
MnAudio *mn_audio_arg(KcValue v, KcPos pos, const char *who);
/* grow/shrink a buffer in place (new frames are zero) */
void     mn_audio_resize(MnAudio *a, size_t frames, KcPos pos, const char *who);

/* ------------------------------------------------------------ argument helpers */
void        mn_need(int n, int lo, int hi, KcPos pos, const char *usage);
bool        mn_is_num(KcValue v);
double      mn_num(KcValue v, KcPos pos, const char *who, const char *what);
int64_t     mn_int(KcValue v, KcPos pos, const char *who, const char *what);
const char *mn_str(KcValue v, KcPos pos, const char *who, const char *what);
bool        mn_is_opts(KcValue v);
KcValue    *mn_opt_get(KcValue o, const char *key);
KcValue     mn_opts_object(KcValue o);
double      mn_opt_num(KcValue o, const char *key, double def, KcPos pos, const char *who);
int64_t     mn_opt_int(KcValue o, const char *key, int64_t def, KcPos pos, const char *who);
bool        mn_opt_bool(KcValue o, const char *key, bool def, KcPos pos, const char *who);
const char *mn_opt_str(KcValue o, const char *key, const char *def, KcPos pos, const char *who);
double      mn_db2lin(double db);
double      mn_lin2db(double lin);

/* ------------------------------------------------------------ biquad */
typedef enum { BQ_LOW, BQ_HIGH, BQ_BAND, BQ_NOTCH, BQ_ALLPASS, BQ_PEAK, BQ_LSHELF, BQ_HSHELF, BQ_BANDQ } MnBqType;
typedef struct { double b0, b1, b2, a1, a2, z1, z2; } MnBiquad;
void mn_biquad_set(MnBiquad *q, MnBqType t, double freq, double Q, double gain_db, double rate);
static inline double mn_biquad_proc(MnBiquad *q, double x) {
    double y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}
int  mn_bq_type_from_name(const char *s);   /* -1 if unknown */

/* portable strtok_r (MinGW does not always provide one) */
static inline char *mn_tok(char *s, const char *delims, char **save) {
    if (!s) s = *save;
    if (!s) return NULL;
    s += strspn(s, delims);
    if (!*s) { *save = NULL; return NULL; }
    char *e = s + strcspn(s, delims);
    if (*e) { *e = 0; *save = e + 1; } else *save = NULL;
    return s;
}

/* ------------------------------------------------------------ noise rng */
typedef struct { uint64_t s; } MnRng;
static inline double mn_rng_f(MnRng *r) {   /* uniform [-1,1) */
    uint64_t x = r->s ? r->s : 0x9E3779B97F4A7C15ull;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17; r->s = x;
    return (double)(int64_t)(x >> 11) / (double)(1ll << 52) - 1.0 + 0.0;
}

/* ------------------------------------------------------------ theory (music_note.c) */
typedef struct { int letter; int pc; int acc; bool has_acc; bool natural; int octave; bool has_oct; } MnNote;
const char *mn_note_scan(const char *s, MnNote *out);              /* NULL when not a note */
int         mn_note_midi(const MnNote *n, int default_octave);
const char *mn_midi_name(int midi, bool flats, char *buf);
bool        mn_chord_symbol(const char *s, int octave, int *midis, int *count, int cap, const char **end);
double      mn_midi_freq(double midi, double a4);
/* key signature: accidental (-1,0,+1) for letter 0..6 (C D E F G A B) given circle-of-fifths count */
int         mn_key_acc(int fifths, int letter);
bool        mn_parse_key(const char *s, int *fifths, bool *minor);

/* ------------------------------------------------------------ instruments (mn_synth.c) */
typedef struct { int inst; double atk, dec, sus, rel; double vib_rate, vib_depth; double detune; int sample; } MnPatch; /* <0 = instrument default; sample = index+1 of a sample-backed instrument */
int         mn_inst_find(const char *name);
const char *mn_inst_name(int id);
int         mn_inst_count(void);
int         mn_inst_program(int id);       /* General MIDI program 0..127 */
bool        mn_inst_is_drum(int id);
int         mn_drum_find(const char *name);     /* 1-based, 0 = unknown */
int         mn_drum_midi(int drum);
const char *mn_drum_name(int drum);
int         mn_drum_count(void);

/* ------------------------------------------------------------ score (mn_score.c) */
typedef struct { double beat, dur; float vel, gate, off; int16_t midi; int16_t patch; uint8_t drum; int16_t smp; } MnEv;   /* smp = index+1: one-shot sample trigger (midi = semitone shift) */
typedef struct {
    char name[40]; MnEv *ev; int n, cap;
    double vol, pan, reverb, delay, delay_time, delay_fb, chorus, lowpass, highpass, glide;
    bool drums; int first_patch;
} MnTrack;
typedef struct { double beat, bpm; } MnTempo;
/* an audio file / inline audio declared in a KCM score */
typedef struct {
    char name[32]; MnAudio *a; double gain_db, start, end, fade_in, fade_out, bpm; int root;
    bool reverse, oneshot, cut; char src[120];
} MnSample;
typedef struct { char name[40]; double beat, sec; bool timed; int track; } MnMarker;
typedef struct {
    MnTrack *tr; int ntr, captr;
    MnPatch *patch; int np, capp;
    MnTempo *tempo; int nt, capt;
    int ts_num, ts_den, key_fifths; bool key_minor, has_key;
    char title[128], composer[96];
    double total_beats; uint32_t seed;
    MnSample *smp; int nsmp, capsmp;
    MnMarker *mk; int nmk, capmk;
    double length_sec;          /* > 0: exact output length requested with `length` */
} MnScore;
typedef struct {
    bool strict; const char *base; const char *file;   /* base: directory for relative sample/include paths; file: name used in messages */
    const char **names; MnAudio **audios; int nnames;     /* samples supplied by the host program */
    KcInterp *ip;                                         /* for comptime checks on file reads */
} MnParseOpts;
MnScore *mn_score_parse_opts(KcValue text, KcValue opts, KcPos pos, const char *who);   /* text is a string value; opts: strict_bars base file */
MnScore *mn_score_parse_ex(const char *text, size_t len, const MnParseOpts *po, KcPos pos, const char *who);
void     mn_set_ip(KcInterp *ip);
double   mn_bpm_at(const MnScore *s, double beat);
double   mn_sec_to_beat(const MnScore *s, double sec);
bool     mn_parse_time(const char *s, double *sec);          /* 12.5  12.5s  500ms  1:02.5  0:01:02.500  1m30s */
char    *mn_timecode(double sec, int fps, char *buf);         /* fps 0 -> HH:MM:SS.mmm ; else HH:MM:SS:FF */
MnScore *mn_score_parse(const char *text, size_t len, bool strict_bars, KcPos pos, const char *who);
void     mn_score_free(MnScore *s);
double   mn_beat_to_sec(const MnScore *s, double beat);
KcValue  mn_score_wrap(MnScore *s);
bool     mn_is_score(KcValue v);
MnScore *mn_score_arg(KcValue v, KcPos pos, const char *who);
int      mn_midi_write(const MnScore *s, const char *path, KcPos pos, const char *who);   /* bytes written */
/* build a score from a plain array of note names (legacy melody form) */
/* synth */
MnAudio *mn_score_render(const MnScore *s, KcValue opts, KcPos pos, const char *who);
/* render one voice: returns malloc'd mono float buffer, *n set */
float   *mn_voice(const MnPatch *p, int drum, double freq, double dur_s, double vel01, int rate, uint64_t seed, double glide_from, double glide_time, size_t *n);

/* ------------------------------------------------------------ DSP kernels shared with the renderer */
void mn_reverb(MnAudio *a, double room, double damp, double wet, double width, double predelay_s, double tail_s, KcPos pos, const char *who);
void mn_delay(MnAudio *a, double time_s, double fb, double mix, bool pingpong, double tail_s, KcPos pos, const char *who);
void mn_chorus(MnAudio *a, double rate, double depth_ms, double mix, int voices);
void mn_filter(MnAudio *a, MnBqType t, double freq, double Q, double gain_db);
void mn_limit(MnAudio *a, double ceil_lin, double look_s, double rel_s);
void mn_fft(double *re, double *im, size_t n, bool inverse);   /* n power of two */
void mn_sidechain(MnAudio *a, const MnAudio *key, double depth, double thr_lin, double attack_s, double release_s);
MnAudio *mn_master(const MnAudio *a, const char *preset, KcPos pos, const char *who);   /* phonk funk clean loud lofi tape */

/* ------------------------------------------------------------ module entry tables */
extern KcModuleEntry mn_theory_entries[];   extern const int mn_theory_count;
extern KcModuleEntry mn_score_entries[];    extern const int mn_score_count;
extern KcModuleEntry mn_synth_entries[];    extern const int mn_synth_count;
extern KcModuleEntry mn_dsp_entries[];      extern const int mn_dsp_count;
extern KcModuleEntry mn_analysis_entries[]; extern const int mn_analysis_count;
extern KcModuleEntry mn_io_entries[];       extern const int mn_io_count;
extern KcModuleEntry mn_groove_entries[];   extern const int mn_groove_count;

KcValue mn_play_audio(KcInterp *ip, KcValue audio, KcPos pos);   /* mn_io.c */
/* WAV helpers (mn_io.c) */
int mn_wav_write(const MnAudio *a, const char *path, int bits, bool dither, KcPos pos, const char *who);
MnAudio *mn_wav_read(const char *path, KcPos pos, const char *who);
MnAudio *mn_wav_decode(const uint8_t *b, size_t len, const char *label, KcPos pos, const char *who);
uint8_t *mn_wav_encode(const MnAudio *a, int bits, bool dither, size_t *len, KcPos pos, const char *who);
uint8_t *mn_read_file(const char *path, size_t *len, KcPos pos, const char *who);
char    *mn_b64_encode(const uint8_t *d, size_t n, int wrap);                 /* wrap = columns per line (0 = one line) */
uint8_t *mn_b64_decode(const char *s, size_t n, size_t *out);                 /* NULL if malformed */
/* resampling and stretching kernels (mn_dsp.c) */
MnAudio *mn_resample_step(const MnAudio *a, double step, int out_rate, KcPos pos, const char *who);
MnAudio *mn_time_stretch(const MnAudio *a, double factor, KcPos pos, const char *who);

#endif
