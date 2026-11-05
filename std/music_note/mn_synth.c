/* KingCobra std.music_note — synthesis: instruments, drums, score renderer, tone generators.
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "mn_internal.h"

#define SETK(o, k, v) kc_object_set((o), (k), strlen(k), (v))

/* ============================================================ oscillators */

typedef enum { W_SINE, W_SQUARE, W_SAW, W_TRI, W_PULSE } Wave;

static inline double polyblep(double t, double dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.0; }
    if (t > 1.0 - dt) { t = (t - 1.0) / dt; return t * t + t + t + 1.0; }
    return 0.0;
}
static inline double wrap1(double x) { return x - floor(x); }
static double osc(Wave w, double ph, double dt, double duty) {
    switch (w) {
        case W_SINE: return sin(MN_TAU * ph);
        case W_SAW: return 2.0 * ph - 1.0 - polyblep(ph, dt);
        case W_SQUARE: return (ph < 0.5 ? 1.0 : -1.0) + polyblep(ph, dt) - polyblep(wrap1(ph + 0.5), dt);
        case W_PULSE: return (ph < duty ? 1.0 : -1.0) + polyblep(ph, dt) - polyblep(wrap1(ph + 1.0 - duty), dt);
        case W_TRI: return 2.0 * fabs(2.0 * wrap1(ph + 0.25) - 1.0) - 1.0;
    }
    return 0;
}

/* complex-rotation sine oscillator with a built-in exponential amplitude decay */
typedef struct { double c, s, dc, ds, a, r; } Par;
static void par_init(Par *p, double f, double rate, double amp, double tau, double phase) {
    double w = MN_TAU * f / rate;
    p->c = cos(phase); p->s = sin(phase); p->dc = cos(w); p->ds = sin(w);
    p->a = amp; p->r = tau > 0 ? exp(-1.0 / (tau * rate)) : 1.0;
}
static inline double par_step(Par *p) {
    double y = p->s * p->a;
    double c = p->c * p->dc - p->s * p->ds, s = p->s * p->dc + p->c * p->ds;
    p->c = c; p->s = s; p->a *= p->r;
    return y;
}

/* ============================================================ instrument table */

typedef enum { A_OSC, A_ORGAN, A_PIANO, A_EPIANO, A_BELL, A_PLUCK, A_BASS, A_PAD, A_BRASS, A_FLUTE, A_LEAD,
               A_MALLET, A_CHOIR, A_NOISE, A_808, A_COWBELL, A_CLAV, A_SLAP, A_WAH, A_SAMPLE, A_DRUMS } Algo;
typedef struct {
    const char *name; Algo algo; Wave wave;
    double atk, dec, sus, rel, gain; int program;
    double vr, vd;       /* default vibrato rate (Hz) / depth (semitones) */
    double p1;           /* algorithm parameter */
} Inst;

static const Inst INST[] = {
    /* name       algo      wave      atk    dec   sus   rel   gain  prog vr   vd    p1 */
    { "sine",     A_OSC,    W_SINE,   0.010, 0.10, 0.85, 0.15, 0.60,  79, 0,   0,    0.5 },
    { "square",   A_OSC,    W_SQUARE, 0.008, 0.10, 0.70, 0.12, 0.30,  80, 0,   0,    0.5 },
    { "saw",      A_OSC,    W_SAW,    0.008, 0.10, 0.70, 0.12, 0.32,  81, 0,   0,    0.5 },
    { "triangle", A_OSC,    W_TRI,    0.010, 0.10, 0.85, 0.15, 0.55,  79, 0,   0,    0.5 },
    { "pulse",    A_OSC,    W_PULSE,  0.008, 0.10, 0.70, 0.12, 0.30,  80, 0,   0,    0.25 },
    { "organ",    A_ORGAN,  W_SINE,   0.012, 0.05, 0.95, 0.06, 0.22,  16, 0,   0,    0 },
    { "piano",    A_PIANO,  W_SINE,   0.002, 0.00, 1.00, 0.28, 0.42,   0, 0,   0,    0 },
    { "epiano",   A_EPIANO, W_SINE,   0.003, 0.00, 1.00, 0.30, 0.45,   4, 0,   0,    0 },
    { "bell",     A_BELL,   W_SINE,   0.002, 0.00, 1.00, 0.90, 0.38,  14, 0,   0,    0 },
    { "pluck",    A_PLUCK,  W_SINE,   0.001, 0.00, 1.00, 0.20, 0.55,  25, 0,   0,    0.0 },
    { "bass",     A_BASS,   W_SAW,    0.006, 0.20, 0.75, 0.10, 0.50,  33, 0,   0,    0 },
    { "strings",  A_PAD,    W_SAW,    0.220, 0.30, 0.85, 0.35, 0.26,  48, 5.2, 0.06, 0.55 },
    { "pad",      A_PAD,    W_SAW,    0.600, 0.40, 0.85, 0.80, 0.24,  89, 0,   0,    0.20 },
    { "brass",    A_BRASS,  W_SAW,    0.050, 0.15, 0.80, 0.12, 0.34,  61, 5.0, 0.04, 0 },
    { "flute",    A_FLUTE,  W_SINE,   0.060, 0.10, 0.85, 0.12, 0.50,  73, 5.0, 0.10, 0 },
    { "lead",     A_LEAD,   W_SAW,    0.010, 0.12, 0.75, 0.15, 0.32,  80, 5.5, 0.10, 0 },
    { "marimba",  A_MALLET, W_SINE,   0.001, 0.00, 1.00, 0.12, 0.60,  12, 0,   0,    0 },
    { "xylophone",A_MALLET, W_SINE,   0.001, 0.00, 1.00, 0.08, 0.55,  13, 0,   0,    1 },
    { "harp",     A_PLUCK,  W_SINE,   0.001, 0.00, 1.00, 0.30, 0.55,  46, 0,   0,    1.0 },
    { "choir",    A_CHOIR,  W_SAW,    0.200, 0.20, 0.90, 0.30, 0.40,  52, 5.3, 0.15, 0 },
    { "noise",    A_NOISE,  W_SINE,   0.010, 0.10, 0.80, 0.15, 0.50, 122, 0,   0,    0 },
    { "808",      A_808,    W_SINE,   0.002, 0.00, 1.00, 0.05, 0.80,  38, 0,   0,    0 },
    { "cowbell",  A_COWBELL,W_SQUARE, 0.001, 0.00, 1.00, 0.06, 0.40, 113, 0,   0,    0 },
    { "clav",     A_CLAV,   W_PULSE,  0.001, 0.00, 1.00, 0.05, 0.45,   7, 0,   0,    0.18 },
    { "slap",     A_SLAP,   W_SAW,    0.001, 0.00, 1.00, 0.06, 0.50,  36, 0,   0,    0 },
    { "wah",      A_WAH,    W_SINE,   0.001, 0.00, 1.00, 0.08, 0.50,  28, 0,   0,    0 },
    { "stab",     A_BRASS,  W_SAW,    0.012, 0.12, 0.50, 0.08, 0.36,  61, 0,   0,    0 },
    { "sample",   A_SAMPLE, W_SINE,   0.000, 0.00, 1.00, 0.04, 1.00,   0, 0,   0,    0 },
    { "drums",    A_DRUMS,  W_SINE,   0.000, 0.00, 1.00, 0.00, 1.00,  -1, 0,   0,    0 },
};
#define NINST ((int)(sizeof INST / sizeof INST[0]))

static const struct { const char *alias; const char *name; } INST_ALIAS[] = {
    { "guitar", "pluck" }, { "acoustic", "pluck" }, { "nylon", "pluck" }, { "violin", "strings" }, { "cello", "strings" },
    { "string", "strings" }, { "rhodes", "epiano" }, { "ep", "epiano" }, { "synth", "lead" }, { "sawtooth", "saw" },
    { "tri", "triangle" }, { "trumpet", "brass" }, { "horn", "brass" }, { "voice", "choir" }, { "vox", "choir" },
    { "kit", "drums" }, { "drum", "drums" }, { "percussion", "drums" }, { "chimes", "bell" }, { "vibes", "marimba" },
    { "sin", "sine" }, { "sq", "square" }, { "white_noise", "noise" }, { "sub", "808" }, { "808bass", "808" },
    { "funk_bass", "slap" }, { "slapbass", "slap" }, { "clavinet", "clav" }, { "chicken", "wah" }, { "guitar_wah", "wah" }, { "horns", "stab" },
};

int mn_inst_find(const char *name) {
    for (int i = 0; i < NINST; i++) if (!strcasecmp(INST[i].name, name)) return i;
    for (size_t i = 0; i < sizeof INST_ALIAS / sizeof INST_ALIAS[0]; i++)
        if (!strcasecmp(INST_ALIAS[i].alias, name)) return mn_inst_find(INST_ALIAS[i].name);
    return -1;
}
const char *mn_inst_name(int id) { return INST[id].name; }
int mn_inst_count(void) { return NINST; }
int mn_inst_program(int id) { return INST[id].program < 0 ? 0 : INST[id].program; }
bool mn_inst_is_drum(int id) { return INST[id].algo == A_DRUMS; }

/* ============================================================ drums */

typedef struct { const char *name; int midi; } DrumDef;
static const DrumDef DRUMS[] = {
    { "kick", 36 }, { "snare", 38 }, { "rim", 37 }, { "clap", 39 }, { "hat", 42 }, { "ohat", 46 },
    { "tom_lo", 45 }, { "tom_mid", 47 }, { "tom_hi", 50 }, { "crash", 49 }, { "ride", 51 }, { "cowbell", 56 },
    { "shaker", 70 }, { "tambourine", 54 }, { "conga", 63 }, { "clave", 75 }, { "kick808", 35 }, { "snap", 85 },
};
#define NDRUMS ((int)(sizeof DRUMS / sizeof DRUMS[0]))
static const struct { const char *alias; int drum; } DRUM_ALIAS[] = {
    { "bd", 1 }, { "bass", 1 }, { "sd", 2 }, { "sidestick", 3 }, { "rs", 3 }, { "cp", 4 }, { "hh", 5 }, { "hihat", 5 }, { "chat", 5 },
    { "oh", 6 }, { "openhat", 6 }, { "ohh", 6 }, { "tom", 8 }, { "tom1", 9 }, { "tom2", 8 }, { "tom3", 7 }, { "lt", 7 }, { "mt", 8 }, { "ht", 9 },
    { "cr", 10 }, { "rd", 11 }, { "cb", 12 }, { "shk", 13 }, { "tamb", 14 }, { "cl", 16 },
};
int mn_drum_find(const char *name) {
    for (int i = 0; i < NDRUMS; i++) if (!strcasecmp(DRUMS[i].name, name)) return i + 1;
    for (size_t i = 0; i < sizeof DRUM_ALIAS / sizeof DRUM_ALIAS[0]; i++) if (!strcasecmp(DRUM_ALIAS[i].alias, name)) return DRUM_ALIAS[i].drum;
    return 0;
}
int mn_drum_midi(int drum) { return DRUMS[drum - 1].midi; }
const char *mn_drum_name(int drum) { return drum >= 1 && drum <= NDRUMS ? DRUMS[drum - 1].name : ""; }
int mn_drum_count(void) { return NDRUMS; }

static float *drum_voice(int drum, double vel, int rate, uint64_t seed, size_t *nout) {
    static const double LEN[] = { 0, 0.55, 0.32, 0.10, 0.40, 0.14, 0.65, 0.55, 0.55, 0.50, 2.2, 1.6, 0.40, 0.16, 0.34, 0.35, 0.12, 1.3, 0.26 };
    double len = LEN[drum];
    size_t N = (size_t)(len * rate) + 1;
    float *o = kc_malloc(N * sizeof(float));
    MnRng rng = { seed * 2654435761ull + 12345 };
    MnBiquad bq1, bq2; memset(&bq1, 0, sizeof bq1); memset(&bq2, 0, sizeof bq2);
    static const double METAL[6] = { 263.0, 400.0, 421.0, 474.0, 587.0, 845.0 };
    double ph = 0, ph2 = 0;
    switch (drum) {
        case 5: case 6: case 10: case 14: mn_biquad_set(&bq1, BQ_HIGH, drum == 10 ? 3000 : drum == 14 ? 5000 : 7000, 0.7, 0, rate); break;
        case 2: mn_biquad_set(&bq1, BQ_HIGH, 1800, 0.7, 0, rate); break;
        case 3: mn_biquad_set(&bq1, BQ_BAND, 3000, 1.5, 0, rate); break;
        case 4: mn_biquad_set(&bq1, BQ_BAND, 1500, 1.4, 0, rate); break;
        case 13: mn_biquad_set(&bq1, BQ_BAND, 6500, 1.0, 0, rate); break;
        case 11: mn_biquad_set(&bq1, BQ_HIGH, 8000, 0.7, 0, rate); break;
        case 12: mn_biquad_set(&bq1, BQ_BAND, 800, 1.8, 0, rate); break;
        case 18: mn_biquad_set(&bq1, BQ_BAND, 2400, 1.2, 0, rate); break;
        default: break;
    }
    mn_biquad_set(&bq2, BQ_HIGH, 6000, 0.7, 0, rate);
    for (size_t i = 0; i < N; i++) {
        double t = (double)i / rate, y = 0, nz = mn_rng_f(&rng);
        switch (drum) {
            case 1: {   /* kick */
                double f = 45.0 + 110.0 * exp(-t / 0.03);
                ph += MN_TAU * f / rate;
                y = sin(ph) * exp(-t / 0.16) + nz * exp(-t / 0.002) * 0.35;
                y = tanh(1.6 * y);
            } break;
            case 2: {   /* snare */
                ph += MN_TAU * (170.0 + 30.0 * exp(-t / 0.02)) / rate;
                y = sin(ph) * exp(-t / 0.05) * 0.55 + mn_biquad_proc(&bq1, nz) * exp(-t / 0.085) * 0.9;
            } break;
            case 3: {   /* rim */
                ph += MN_TAU * 1700.0 / rate;
                y = sin(ph) * exp(-t / 0.004) * 0.8 + mn_biquad_proc(&bq1, nz) * exp(-t / 0.003) * 1.2;
            } break;
            case 4: {   /* clap */
                double env = exp(-(t > 0.03 ? t - 0.03 : 0) / 0.12);
                double burst = 0; for (int b = 0; b < 3; b++) if (t >= b * 0.010) burst += exp(-(t - b * 0.010) / 0.004) * (b < 2 ? 1.0 : 0.0);
                y = mn_biquad_proc(&bq1, nz) * (t < 0.03 ? burst * 0.6 : env * 0.9);
            } break;
            case 5: case 6: {   /* hats */
                double m = 0; for (int k = 0; k < 6; k++) m += sin(MN_TAU * METAL[k] * 4.0 * t) >= 0 ? 1.0 : -1.0;
                y = mn_biquad_proc(&bq1, nz * 0.7 + m * 0.12) * exp(-t / (drum == 5 ? 0.025 : 0.17));
            } break;
            case 7: case 8: case 9: {   /* toms */
                double f0 = drum == 7 ? 90.0 : drum == 8 ? 130.0 : 190.0;
                ph += MN_TAU * (f0 + f0 * 0.6 * exp(-t / 0.05)) / rate;
                y = sin(ph) * exp(-t / 0.2) + nz * exp(-t / 0.003) * 0.15;
            } break;
            case 10: {  /* crash */
                double m = 0; for (int k = 0; k < 6; k++) m += sin(MN_TAU * METAL[k] * 3.1 * t) >= 0 ? 1.0 : -1.0;
                y = mn_biquad_proc(&bq1, nz * 0.8 + m * 0.10) * exp(-t / 0.55) * (t < 0.002 ? t / 0.002 : 1.0);
            } break;
            case 11: {  /* ride */
                double m = sin(MN_TAU * 523.0 * t) + 0.7 * sin(MN_TAU * 784.0 * t * 1.003) + 0.5 * sin(MN_TAU * 1397.0 * t);
                y = (m * 0.28 * exp(-t / 0.45) + mn_biquad_proc(&bq1, nz) * 0.25 * exp(-t / 0.12));
            } break;
            case 12: {  /* cowbell */
                double s1 = sin(MN_TAU * 562.0 * t) >= 0 ? 1 : -1, s2 = sin(MN_TAU * 845.0 * t) >= 0 ? 1 : -1;
                y = mn_biquad_proc(&bq1, (s1 + s2) * 0.5) * (0.6 * exp(-t / 0.07) + 0.4 * exp(-t / 0.012));
            } break;
            case 13: {  /* shaker */
                double e = t < 0.02 ? t / 0.02 : exp(-(t - 0.02) / 0.04);
                y = mn_biquad_proc(&bq1, nz) * e;
            } break;
            case 14: {  /* tambourine */
                double m = sin(MN_TAU * 3300.0 * t) * 0.3 + sin(MN_TAU * 4700.0 * t) * 0.2 + sin(MN_TAU * 6100.0 * t) * 0.15;
                y = mn_biquad_proc(&bq1, nz * 0.6 + m) * exp(-t / 0.09);
            } break;
            case 15: {  /* conga */
                ph += MN_TAU * (170.0 + 40.0 * exp(-t / 0.02)) / rate;
                y = sin(ph) * exp(-t / 0.09) + nz * exp(-t / 0.004) * 0.2;
            } break;
            case 17: {  /* long 808 kick */
                double f = 36.0 + 70.0 * exp(-t / 0.035);
                ph += MN_TAU * f / rate;
                y = tanh(1.8 * sin(ph)) * exp(-t / 0.5) + nz * exp(-t / 0.002) * 0.25;
            } break;
            case 18: {  /* finger snap / trap clap */
                ph += MN_TAU * 1700.0 / rate;
                y = mn_biquad_proc(&bq1, nz) * exp(-t / 0.05) * 1.1 + sin(ph) * exp(-t / 0.012) * 0.4;
            } break;
            case 16: {  /* clave */
                ph += MN_TAU * 2500.0 / rate; ph2 += MN_TAU * 4000.0 / rate;
                y = (sin(ph) + 0.4 * sin(ph2)) * exp(-t / 0.018);
            } break;
        }
        (void)ph2;
        o[i] = (float)y;
    }
    /* level each voice to a fixed peak (relative loudness per drum) so none clips */
    static const double G[] = { 0, 0.90, 0.85, 0.60, 0.70, 0.45, 0.50, 0.80, 0.80, 0.80, 0.55, 0.45, 0.50, 0.35, 0.45, 0.70, 0.55, 0.95, 0.70 };
    float pk = 0; for (size_t i = 0; i < N; i++) { float v = fabsf(o[i]); if (v > pk) pk = v; }
    if (pk > 1e-9f) { float g = (float)(G[drum] * vel / pk); for (size_t i = 0; i < N; i++) o[i] *= g; }
    *nout = N;
    return o;
}

/* ============================================================ voices */

typedef struct {
    double f; int rate; size_t N; double dur, vel, vr, vd, detune, p1; MnRng rng; double gr, gtau;
} Ctx;

static inline double vib_mul(const Ctx *c, size_t i) {
    double t = (double)i / c->rate, m = 1.0;
    if (c->gtau > 0) m = pow(c->gr, exp(-t / c->gtau));          /* portamento from the previous note */
    if (c->vd <= 0) return m;
    double ramp = t < 0.18 ? 0.0 : (t > 0.55 ? 1.0 : (t - 0.18) / 0.37);
    return m * exp2(c->vd / 12.0 * ramp * sin(MN_TAU * c->vr * t));
}

static void gen_osc(const Inst *in, float *o, Ctx *c) {
    double ph = 0, ph2 = 0, dt, duty = in->p1;
    double d2 = c->detune != 0 ? exp2(c->detune / 1200.0) : 1.0;
    for (size_t i = 0; i < c->N; i++) {
        double m = vib_mul(c, i); dt = c->f * m / c->rate;
        ph = wrap1(ph + dt);
        double y = osc(in->wave, ph, dt, duty);
        if (c->detune != 0) { ph2 = wrap1(ph2 + dt * d2); y = 0.5 * (y + osc(in->wave, ph2, dt * d2, duty)); }
        o[i] = (float)y;
    }
}
static void gen_organ(float *o, Ctx *c) {
    static const int M[8] = { 1, 2, 3, 4, 6, 8, 10, 16 };           /* multiples of f/2 */
    static const double A[8] = { 0.6, 1.0, 0.4, 0.6, 0.25, 0.3, 0.15, 0.2 };
    double sp = 0, nyq = c->rate * 0.45;
    for (size_t i = 0; i < c->N; i++) {
        sp = wrap1(sp + 0.5 * c->f / c->rate);
        double y = 0;
        for (int k = 0; k < 8; k++) if (M[k] * 0.5 * c->f < nyq) y += A[k] * sin(MN_TAU * sp * M[k]);
        o[i] = (float)(y * 0.5);
    }
}
static void gen_piano(float *o, Ctx *c) {
    double f = c->f, midi = 69.0 + 12.0 * log2(f / 440.0);
    double T0 = 3.0 * pow(261.6 / f, 0.7); if (T0 < 0.4) T0 = 0.4; if (T0 > 9.0) T0 = 9.0;
    double B = 0.00015 + 0.000004 * (midi > 40 ? midi - 40 : 0);
    double bright = 0.3 + 0.7 * c->vel;
    Par p[16]; int np = 0;
    for (int k = 1; k <= 14; k++) {
        double fk = k * f * sqrt(1.0 + B * k * k);
        if (fk > c->rate * 0.45) break;
        double amp = 1.0 / pow(k, 1.05) * exp(-(k - 1) * (1.0 - bright) * 0.55);
        double tau = T0 / (1.0 + 0.8 * (k - 1));
        par_init(&p[np++], fk, c->rate, amp, tau, 0);
    }
    double hp = 0;
    for (size_t i = 0; i < c->N; i++) {
        double y = 0, t = (double)i / c->rate;
        for (int k = 0; k < np; k++) y += par_step(&p[k]);
        double nz = mn_rng_f(&c->rng); hp += 0.35 * (nz - hp);       /* soft hammer thump */
        y += hp * exp(-t / 0.005) * 0.25 * c->vel;
        o[i] = (float)(y * 0.55);
    }
}
static void gen_epiano(float *o, Ctx *c) {
    double ph = 0, pm = 0, pb = 0;
    for (size_t i = 0; i < c->N; i++) {
        double t = (double)i / c->rate;
        double idx = (0.4 + 2.2 * c->vel) * exp(-t / 0.35) + 0.25;
        ph += MN_TAU * c->f / c->rate; pm += MN_TAU * c->f / c->rate; pb += MN_TAU * c->f * 14.0 / c->rate;
        double y = sin(ph + idx * sin(pm)) * exp(-t / (1.6 + 1.5 * 261.6 / c->f * 0.3));
        y += 0.12 * c->vel * sin(pb) * exp(-t / 0.04);
        o[i] = (float)(y * 0.7);
    }
}
static void gen_bell(float *o, Ctx *c) {
    double ph = 0, pm = 0, p2 = 0;
    for (size_t i = 0; i < c->N; i++) {
        double t = (double)i / c->rate;
        ph += MN_TAU * c->f / c->rate; pm += MN_TAU * c->f * 3.5 / c->rate; p2 += MN_TAU * c->f * 2.76 / c->rate;
        double y = sin(ph + (2.5 * exp(-t / 1.4) + 0.3) * sin(pm)) * exp(-t / 3.0);
        y += 0.35 * sin(p2) * exp(-t / 1.1);
        o[i] = (float)(y * 0.6);
    }
}
static void gen_pluck(const Inst *in, float *o, Ctx *c) {
    double f = c->f; if (f > c->rate / 6.0) f = c->rate / 6.0;
    double P = c->rate / f, D = P - 0.5;
    int Nn = (int)floor(D - 0.5); if (Nn < 2) Nn = 2;
    double d = D - Nn, a = (1.0 - d) / (1.0 + d);
    double T60 = (in->p1 > 0.5 ? 4.0 : 2.4) * pow(261.6 / f, 0.3); if (T60 < 0.5) T60 = 0.5; if (T60 > 8.0) T60 = 8.0;
    double g = pow(10.0, -3.0 / (f * T60));
    double *buf = kc_malloc((size_t)Nn * sizeof(double));
    double lp = 0, soft = in->p1 > 0.5 ? 0.25 : 0.6 - 0.35 * c->vel;
    for (int i = 0; i < Nn; i++) { double nz = mn_rng_f(&c->rng); lp += (1.0 - soft) * (nz - lp); buf[i] = lp * 1.6 * (0.5 + 0.5 * c->vel); }
    int idx = 0; double prev = buf[Nn - 1], apx = 0, apy = 0;
    for (size_t i = 0; i < c->N; i++) {
        double x = buf[idx];
        double avg = g * 0.5 * (x + prev);
        double y = a * (avg - apy) + apx; apx = avg; apy = y;
        buf[idx] = y; prev = x;
        if (++idx >= Nn) idx = 0;
        o[i] = (float)y;
    }
    free(buf);
}
static void gen_bass(float *o, Ctx *c) {
    double ph = 0, ps = 0; MnBiquad q; memset(&q, 0, sizeof q);
    for (size_t i = 0; i < c->N; i++) {
        if ((i & 63) == 0) {
            double t = (double)i / c->rate, cut = c->f * (2.0 + 7.0 * c->vel * exp(-t / 0.22));
            if (cut > c->rate * 0.4) cut = c->rate * 0.4;
            mn_biquad_set(&q, BQ_LOW, cut, 1.1, 0, c->rate);
        }
        double dt = c->f * vib_mul(c, i) / c->rate; ph = wrap1(ph + dt); ps = wrap1(ps + 0.5 * dt);
        double y = mn_biquad_proc(&q, osc(W_SAW, ph, dt, 0.5)) * 0.8 + 0.6 * sin(MN_TAU * ps);
        o[i] = (float)y;
    }
}
static void gen_pad(const Inst *in, float *o, Ctx *c) {
    double det = c->detune > 0 ? c->detune : 9.0;
    double ph[3] = { 0, 0.33, 0.66 }, r[3] = { exp2(-det / 1200.0), 1.0, exp2(det / 1200.0) };
    MnBiquad q = {0}; double cut = c->f * (3.0 + in->p1 * 9.0); if (cut < 400) cut = 400; if (cut > 9000) cut = 9000;
    mn_biquad_set(&q, BQ_LOW, cut, 0.7, 0, c->rate);
    for (size_t i = 0; i < c->N; i++) {
        double m = vib_mul(c, i), y = 0;
        for (int k = 0; k < 3; k++) { double dt = c->f * m * r[k] / c->rate; ph[k] = wrap1(ph[k] + dt); y += osc(W_SAW, ph[k], dt, 0.5); }
        o[i] = (float)(mn_biquad_proc(&q, y / 3.0) * 1.4);
    }
}
static void gen_brass(float *o, Ctx *c) {
    double ph = 0, ph2 = 0; MnBiquad q; memset(&q, 0, sizeof q);
    for (size_t i = 0; i < c->N; i++) {
        double t = (double)i / c->rate;
        if ((i & 63) == 0) {
            double sw = 1.0 - exp(-t / 0.10), cut = c->f * (1.3 + (3.0 + 5.0 * c->vel) * sw);
            if (cut > c->rate * 0.4) cut = c->rate * 0.4;
            mn_biquad_set(&q, BQ_LOW, cut, 0.9, 0, c->rate);
        }
        double m = vib_mul(c, i), dt = c->f * m / c->rate, dt2 = dt * 1.003;
        ph = wrap1(ph + dt); ph2 = wrap1(ph2 + dt2);
        o[i] = (float)(mn_biquad_proc(&q, 0.5 * (osc(W_SAW, ph, dt, 0.5) + osc(W_SAW, ph2, dt2, 0.5))) * 1.2);
    }
}
static void gen_flute(float *o, Ctx *c) {
    double ph = 0; MnBiquad q = {0}; double fc = c->f * 2.0; if (fc > c->rate * 0.4) fc = c->rate * 0.4;
    mn_biquad_set(&q, BQ_BAND, fc, 1.5, 0, c->rate);
    for (size_t i = 0; i < c->N; i++) {
        double m = vib_mul(c, i); ph = wrap1(ph + c->f * m / c->rate);
        double y = sin(MN_TAU * ph) + 0.18 * sin(MN_TAU * 2 * ph) + 0.05 * sin(MN_TAU * 3 * ph);
        y += mn_biquad_proc(&q, mn_rng_f(&c->rng)) * 0.25;
        o[i] = (float)y;
    }
}
static void gen_lead(float *o, Ctx *c) {
    double ph = 0, ph2 = 0;
    for (size_t i = 0; i < c->N; i++) {
        double m = vib_mul(c, i), dt = c->f * m / c->rate, dt2 = dt * 1.004;
        ph = wrap1(ph + dt); ph2 = wrap1(ph2 + dt2);
        o[i] = (float)(0.55 * osc(W_SAW, ph, dt, 0.5) + 0.45 * osc(W_PULSE, ph2, dt2, 0.35));
    }
}
static void gen_mallet(const Inst *in, float *o, Ctx *c) {
    double f = c->f;
    double tau = 1.2 * pow(261.6 / f, 0.6); if (tau < 0.08) tau = 0.08; if (tau > 1.6) tau = 1.6;
    if (in->p1 > 0.5) tau *= 0.5;
    const double R[3] = { 1.0, in->p1 > 0.5 ? 3.76 : 4.0, in->p1 > 0.5 ? 6.0 : 9.9 };
    const double A[3] = { 1.0, 0.35, 0.15 }, TM[3] = { 1.0, 0.4, 0.2 };
    Par p[3]; int np = 0;
    for (int k = 0; k < 3; k++) if (f * R[k] < c->rate * 0.45) par_init(&p[np++], f * R[k], c->rate, A[k], tau * TM[k], 0);
    for (size_t i = 0; i < c->N; i++) { double y = 0; for (int k = 0; k < np; k++) y += par_step(&p[k]); o[i] = (float)(y * 0.7); }
}
static void gen_choir(float *o, Ctx *c) {
    double ph = 0; MnBiquad q[3]; memset(q, 0, sizeof q);
    static const double F[3] = { 800, 1150, 2900 }, G[3] = { 1.0, 0.55, 0.25 };
    for (int k = 0; k < 3; k++) mn_biquad_set(&q[k], BQ_BAND, F[k], 5.0, 0, c->rate);
    for (size_t i = 0; i < c->N; i++) {
        double m = vib_mul(c, i), dt = c->f * m / c->rate; ph = wrap1(ph + dt);
        double s = osc(W_SAW, ph, dt, 0.5) + mn_rng_f(&c->rng) * 0.05, y = 0;
        for (int k = 0; k < 3; k++) y += G[k] * mn_biquad_proc(&q[k], s);
        o[i] = (float)(y * 2.2);
    }
}
static void gen_noise(float *o, Ctx *c) {
    MnBiquad q = {0}; double fc = c->f > 0 ? c->f : 1000; if (fc > c->rate * 0.4) fc = c->rate * 0.4;
    mn_biquad_set(&q, BQ_BAND, fc, 0.8, 0, c->rate);
    for (size_t i = 0; i < c->N; i++) o[i] = (float)(mn_biquad_proc(&q, mn_rng_f(&c->rng)) * 1.5);
}


static void gen_808(float *o, Ctx *c) {
    double ph = 0;
    for (size_t i = 0; i < c->N; i++) {
        double t = (double)i / c->rate, f = c->f * vib_mul(c, i) * (1.0 + 0.55 * exp(-t / 0.012));
        ph += MN_TAU * f / c->rate;
        double y = sin(ph) + 0.10 * sin(2.0 * ph + 0.5);
        y = tanh(1.7 * y) * 0.9 * exp(-t / 1.1);
        if (t < 0.004) y += mn_rng_f(&c->rng) * 0.10 * (1.0 - t / 0.004);
        o[i] = (float)y;
    }
}
static void gen_cowbell(float *o, Ctx *c) {
    double p1 = 0, p2 = 0; MnBiquad q = {0}; double fc = c->f * 2.2; if (fc > c->rate * 0.4) fc = c->rate * 0.4;
    mn_biquad_set(&q, BQ_BAND, fc, 1.6, 0, c->rate);
    for (size_t i = 0; i < c->N; i++) {
        double t = (double)i / c->rate, m = vib_mul(c, i), d1 = c->f * m / c->rate, d2 = d1 * 1.504;
        p1 = wrap1(p1 + d1); p2 = wrap1(p2 + d2);
        double y = mn_biquad_proc(&q, osc(W_SQUARE, p1, d1, 0.5) + osc(W_SQUARE, p2, d2, 0.5));
        o[i] = (float)(y * (0.6 * exp(-t / 0.09) + 0.4 * exp(-t / 0.4)) * 1.4);
    }
}
static void gen_clav(const Inst *in, float *o, Ctx *c) {
    double ph = 0; MnBiquad q; memset(&q, 0, sizeof q);
    for (size_t i = 0; i < c->N; i++) {
        double t = (double)i / c->rate;
        if ((i & 31) == 0) { double cut = c->f * (3.0 + (6.0 + 8.0 * c->vel) * exp(-t / 0.06)); if (cut > c->rate * 0.4) cut = c->rate * 0.4; mn_biquad_set(&q, BQ_LOW, cut, 1.2, 0, c->rate); }
        double dt = c->f * vib_mul(c, i) / c->rate; ph = wrap1(ph + dt);
        double duty = in->p1 + 0.08 * sin(MN_TAU * 5.0 * t);
        o[i] = (float)(mn_biquad_proc(&q, osc(W_PULSE, ph, dt, duty)) * exp(-t / 0.28) * 1.3);
    }
}
static void gen_slap(float *o, Ctx *c) {
    double ph = 0, ph2 = 0, ps = 0; MnBiquad q; memset(&q, 0, sizeof q); MnBiquad hp = {0}; mn_biquad_set(&hp, BQ_HIGH, 2500, 0.7, 0, c->rate);
    for (size_t i = 0; i < c->N; i++) {
        double t = (double)i / c->rate;
        if ((i & 31) == 0) { double cut = c->f * (2.0 + 14.0 * c->vel * exp(-t / 0.05)); if (cut > c->rate * 0.4) cut = c->rate * 0.4; mn_biquad_set(&q, BQ_LOW, cut, 1.6, 0, c->rate); }
        double dt = c->f * vib_mul(c, i) / c->rate; ph = wrap1(ph + dt); ph2 = wrap1(ph2 + dt * 1.005); ps = wrap1(ps + 0.5 * dt);
        double y = mn_biquad_proc(&q, 0.6 * osc(W_SAW, ph, dt, 0.5) + 0.4 * osc(W_SQUARE, ph2, dt, 0.5)) + 0.45 * sin(MN_TAU * ps);
        y *= exp(-t / 0.45);
        y += mn_biquad_proc(&hp, mn_rng_f(&c->rng)) * exp(-t / 0.004) * 0.35 * c->vel;       /* thumb pop */
        o[i] = (float)y;
    }
}
static void gen_wah(const Inst *in, float *o, Ctx *c) {
    Ctx k = *c; gen_pluck(in, o, &k);
    MnBiquad q; memset(&q, 0, sizeof q);
    for (size_t i = 0; i < c->N; i++) {
        double t = (double)i / c->rate;
        if ((i & 15) == 0) mn_biquad_set(&q, BQ_BAND, 450.0 + 2200.0 * exp(-t / 0.11), 3.5, 0, c->rate);
        double y = o[i];
        o[i] = (float)(0.3 * y + 1.6 * mn_biquad_proc(&q, y));
    }
}

static inline double adsr_level(double t, double atk, double dec, double sus) {
    if (t < atk) return atk > 0 ? t / atk : 1.0;
    t -= atk;
    if (t < dec) return 1.0 - (1.0 - sus) * (t / dec);
    return sus;
}

float *mn_voice(const MnPatch *p, int drum, double freq, double dur_s, double vel01, int rate, uint64_t seed, double glide_from, double glide_time, size_t *n) {
    if (vel01 < 0) vel01 = 0;
    if (vel01 > 1) vel01 = 1;
    if (drum > 0) return drum_voice(drum, vel01, rate, seed, n);
    const Inst *in = &INST[p->inst];
    if (in->algo == A_DRUMS) { *n = 1; float *z = kc_malloc(sizeof(float)); z[0] = 0; return z; }
    double atk = p->atk >= 0 ? p->atk : in->atk, dec = p->dec >= 0 ? p->dec : in->dec;
    double sus = p->sus >= 0 ? p->sus : in->sus, rel = p->rel >= 0 ? p->rel : in->rel;
    if (sus > 1) sus = 1;
    if (dur_s < 0.005) dur_s = 0.005;
    double total = dur_s + rel + 0.01;
    if (total > 40.0) total = 40.0;
    size_t N = (size_t)(total * rate) + 1;
    float *o = kc_malloc(N * sizeof(float));
    Ctx c; c.f = freq; c.rate = rate; c.N = N; c.dur = dur_s; c.vel = vel01;
    c.vr = p->vib_rate >= 0 ? p->vib_rate : in->vr; c.vd = p->vib_depth >= 0 ? p->vib_depth : in->vd;
    c.detune = p->detune; c.p1 = in->p1; c.rng.s = seed * 0x9E3779B97F4A7C15ull + 0x1234567ull;
    c.gr = 1.0; c.gtau = 0;
    if (glide_from > 0 && glide_time > 0 && freq > 0 && fabs(glide_from - freq) > 1e-6) { c.gr = glide_from / freq; c.gtau = glide_time / 3.0; }
    switch (in->algo) {
        case A_OSC: gen_osc(in, o, &c); break;
        case A_ORGAN: gen_organ(o, &c); break;
        case A_PIANO: gen_piano(o, &c); break;
        case A_EPIANO: gen_epiano(o, &c); break;
        case A_BELL: gen_bell(o, &c); break;
        case A_PLUCK: gen_pluck(in, o, &c); break;
        case A_BASS: gen_bass(o, &c); break;
        case A_PAD: gen_pad(in, o, &c); break;
        case A_BRASS: gen_brass(o, &c); break;
        case A_FLUTE: gen_flute(o, &c); break;
        case A_LEAD: gen_lead(o, &c); break;
        case A_MALLET: gen_mallet(in, o, &c); break;
        case A_CHOIR: gen_choir(o, &c); break;
        case A_NOISE: gen_noise(o, &c); break;
        case A_808: gen_808(o, &c); break;
        case A_COWBELL: gen_cowbell(o, &c); break;
        case A_CLAV: gen_clav(in, o, &c); break;
        case A_SLAP: gen_slap(o, &c); break;
        case A_WAH: gen_wah(in, o, &c); break;
        default: memset(o, 0, N * sizeof(float)); break;
    }
    double lvl_off = adsr_level(dur_s, atk, dec, sus);
    double amp = in->gain * (0.25 + 0.75 * vel01);
    for (size_t i = 0; i < N; i++) {
        double t = (double)i / rate, e;
        if (t <= dur_s) e = adsr_level(t, atk, dec, sus);
        else { double r = rel > 0 ? 1.0 - (t - dur_s) / rel : 0.0; e = r > 0 ? lvl_off * r * r : 0.0; }
        o[i] = (float)(o[i] * e * amp);
    }
    size_t fade = (size_t)(0.002 * rate); if (fade > N) fade = N;
    for (size_t i = 0; i < fade; i++) o[N - 1 - i] *= (float)i / (float)fade;
    *n = N;
    return o;
}

/* ============================================================ sample playback inside the renderer */

typedef struct { int smp; double ratio, stretch; MnAudio *au; } PCache;
typedef struct { PCache *e; int n, cap; } PCL;

static MnAudio *prep_sample(const MnSample *m, int idx, double ratio, double stretch, int rate, PCL *c, KcPos pos, const char *who) {
    for (int i = 0; i < c->n; i++) if (c->e[i].smp == idx && c->e[i].ratio == ratio && c->e[i].stretch == stretch) return c->e[i].au;
    const MnAudio *src = m->a;
    size_t f0 = (size_t)(m->start * src->rate), f1 = m->end > 0 ? (size_t)(m->end * src->rate) : src->frames;
    if (f1 > src->frames) f1 = src->frames;
    if (f0 >= f1) { f0 = 0; f1 = src->frames; }
    MnAudio *t = mn_audio_new(f1 - f0, src->ch, src->rate, pos, who);
    memcpy(t->d, src->d + f0 * (size_t)src->ch, (f1 - f0) * (size_t)src->ch * sizeof(float));
    if (m->reverse) {
        for (size_t i = 0; i < t->frames / 2; i++) for (int ch = 0; ch < t->ch; ch++) {
            float *x = &t->d[i * t->ch + ch], *y = &t->d[(t->frames - 1 - i) * t->ch + ch]; float v = *x; *x = *y; *y = v;
        }
    }
    if (fabs(stretch - 1.0) > 0.002) { MnAudio *x = mn_time_stretch(t, stretch, pos, who); mn_audio_free(t); t = x; }
    double step = ratio * (double)t->rate / (double)rate;
    if (fabs(step - 1.0) > 1e-9) { MnAudio *x = mn_resample_step(t, step, rate, pos, who); mn_audio_free(t); t = x; }
    t->rate = rate;
    size_t fi = (size_t)((m->fade_in > 0.0015 ? m->fade_in : 0.0015) * rate), fo = (size_t)((m->fade_out > 0.0015 ? m->fade_out : 0.0015) * rate);
    if (fi > t->frames) fi = t->frames;
    if (fo > t->frames) fo = t->frames;
    for (size_t i = 0; i < fi; i++) for (int ch = 0; ch < t->ch; ch++) t->d[i * t->ch + ch] *= (float)i / (float)fi;
    for (size_t i = 0; i < fo; i++) for (int ch = 0; ch < t->ch; ch++) t->d[(t->frames - 1 - i) * t->ch + ch] *= (float)i / (float)fo;
    if (c->n == c->cap) { c->cap = c->cap ? c->cap * 2 : 16; c->e = kc_realloc(c->e, (size_t)c->cap * sizeof *c->e); }
    c->e[c->n].smp = idx; c->e[c->n].ratio = ratio; c->e[c->n].stretch = stretch; c->e[c->n].au = t; c->n++;
    return t;
}
/* resolve the sample, speed ratio and tempo-sync stretch of a sample-backed event; returns the sample index or -1 */
static int sample_of(const MnScore *s, const MnEv *e, int transpose, double speed, double *ratio, double *stretch) {
    int idx = e->smp ? e->smp : s->patch[e->patch].sample;
    if (idx <= 0) return -1;
    const MnSample *m = &s->smp[idx - 1];
    *ratio = e->smp ? pow(2.0, e->midi / 12.0) : pow(2.0, (e->midi + transpose - m->root) / 12.0);
    double bpm = mn_bpm_at(s, e->beat) * speed;
    *stretch = m->bpm > 0 ? m->bpm / bpm : 1.0;
    if (*stretch < 0.25) *stretch = 0.25;
    if (*stretch > 4.0) *stretch = 4.0;
    return idx - 1;
}

/* ============================================================ score renderer */

static bool name_in(KcValue v, const char *name) {
    if (v.type == VAL_STRING) return !strcasecmp(v.as.s->data, name);
    if (v.type == VAL_ARRAY) for (int i = 0; i < v.as.arr->count; i++)
        if (v.as.arr->items[i].type == VAL_STRING && !strcasecmp(v.as.arr->items[i].as.s->data, name)) return true;
    return false;
}

MnAudio *mn_score_render(const MnScore *s, KcValue opts, KcPos pos, const char *who) {
    int rate = (int)mn_opt_int(opts, "rate", MN_RATE, pos, who);
    double tail = mn_opt_num(opts, "tail", 1.5, pos, who), master = mn_opt_num(opts, "master", 0.8, pos, who);
    double speed = mn_opt_num(opts, "speed", 1.0, pos, who), human = mn_opt_num(opts, "humanize", 0.0, pos, who);
    int transpose = (int)mn_opt_int(opts, "transpose", 0, pos, who);
    uint64_t seed = (uint64_t)mn_opt_int(opts, "seed", 0, pos, who) + s->seed;
    bool normalize = mn_opt_bool(opts, "normalize", false, pos, who), mono = mn_opt_bool(opts, "mono", false, pos, who);
    double a4 = mn_opt_num(opts, "a4", 440.0, pos, who);
    KcValue *solo = mn_opt_get(opts, "solo"), *mute = mn_opt_get(opts, "mute");
    if (speed <= 0.05 || speed > 20) kc_error_at(pos, "music_note.%s: speed must be 0.05..20", who);
    if (tail < 0 || tail > 60) kc_error_at(pos, "music_note.%s: tail must be 0..60 seconds", who);

    double total_s = mn_beat_to_sec(s, s->total_beats) / speed + tail;
    for (int ti = 0; ti < s->ntr && s->nsmp; ti++) {                     /* samples may ring on past the last note */
        const MnTrack *tr = &s->tr[ti];
        if (solo && solo->type != VAL_NULL && !name_in(*solo, tr->name)) continue;
        if (mute && mute->type != VAL_NULL && name_in(*mute, tr->name)) continue;
        for (int ei = 0; ei < tr->n; ei++) {
            const MnEv *e = &tr->ev[ei]; double ratio, stretch;
            int si = sample_of(s, e, transpose, speed, &ratio, &stretch); if (si < 0) continue;
            const MnSample *m = &s->smp[si];
            double len = ((m->end > 0 && m->end * m->a->rate < m->a->frames ? m->end : (double)m->a->frames / m->a->rate) - m->start); if (len < 0) len = (double)m->a->frames / m->a->rate;
            double est = len * stretch / ratio, s0 = mn_beat_to_sec(s, e->beat) / speed;
            bool cutit = e->smp ? m->cut : !m->oneshot;
            if (cutit) { double lim = (mn_beat_to_sec(s, e->beat + e->dur * e->gate) / speed - s0) + 0.05; if (est > lim) est = lim; }
            if (s0 + est + 0.05 > total_s) total_s = s0 + est + 0.05;
        }
    }
    if (s->length_sec > 0) total_s = s->length_sec;
    size_t frames = (size_t)ceil(total_s * rate - 1e-6);
    if (frames < 1) frames = 1;
    MnAudio *out = mn_audio_new(frames, 2, rate, pos, who);
    PCL pcache = { 0 };
    int active = 0;
    for (int ti = 0; ti < s->ntr; ti++) {
        const MnTrack *tr = &s->tr[ti];
        if (solo && solo->type != VAL_NULL && !name_in(*solo, tr->name)) continue;
        if (mute && mute->type != VAL_NULL && name_in(*mute, tr->name)) continue;
        active++;
    }
    double headroom = 1.0 / (1.0 + 0.22 * (active > 1 ? active - 1 : 0));
    for (int ti = 0; ti < s->ntr; ti++) {
        const MnTrack *tr = &s->tr[ti];
        if (solo && solo->type != VAL_NULL && !name_in(*solo, tr->name)) continue;
        if (mute && mute->type != VAL_NULL && name_in(*mute, tr->name)) continue;
        if (tr->n == 0) continue;
        MnAudio *tb = mn_audio_new(frames, 2, rate, pos, who);
        double pan = tr->pan < -1 ? -1 : tr->pan > 1 ? 1 : tr->pan, ang = (pan + 1.0) * M_PI / 4.0;
        float gl = (float)cos(ang), gr = (float)sin(ang);
        MnRng hr = { seed * 7919u + (uint64_t)ti * 104729u + 1 };
        for (int ei = 0; ei < tr->n; ei++) {
            const MnEv *e = &tr->ev[ei];
            double s0 = mn_beat_to_sec(s, e->beat) / speed;
            double s1 = mn_beat_to_sec(s, e->beat + e->dur * e->gate) / speed;
            double dur = s1 - s0;
            double vel = e->vel / 127.0, st = s0 + e->off;
            if (human > 0) { st += mn_rng_f(&hr) * human / 1000.0; vel *= 1.0 + mn_rng_f(&hr) * 0.08; if (st < 0) st = 0; }
            size_t start = (size_t)(st * rate);
            if (start >= frames) continue;
            { double ratio, stretch; int si = sample_of(s, e, transpose, speed, &ratio, &stretch);
              if (si >= 0) {                                       /* ---- sample playback ---- */
                  const MnSample *m = &s->smp[si];
                  MnAudio *pa = prep_sample(m, si, ratio, stretch, rate, &pcache, pos, who);
                  bool cutit = e->smp ? m->cut : !m->oneshot; double rel = 0.04, dcut = dur > 0 ? dur : 0;
                  size_t n = pa->frames;
                  if (cutit) { size_t lim = (size_t)((dcut + rel) * rate); if (lim < n) n = lim; }
                  if (start + n > frames) n = frames - start;
                  double amp = mn_db2lin(m->gain_db) * (0.25 + 0.75 * vel) / (0.25 + 0.75 * 80.0 / 127.0);
                  float *dst = tb->d + start * 2; size_t cut_at = (size_t)(dcut * rate), rel_n = (size_t)(rel * rate) + 1;
                  for (size_t i = 0; i < n; i++) {
                      double g = amp;
                      if (cutit && i > cut_at) { double u = (double)(i - cut_at) / (double)rel_n; g *= u < 1 ? 1.0 - u : 0.0; }
                      if (pa->ch == 1) { float x = (float)(pa->d[i] * g); dst[2 * i] += x * gl; dst[2 * i + 1] += x * gr; }
                      else { dst[2 * i] += (float)(pa->d[i * pa->ch] * g) * gl * 1.4142f; dst[2 * i + 1] += (float)(pa->d[i * pa->ch + 1] * g) * gr * 1.4142f; }
                  }
                  continue;
              } }
            double freq = e->drum ? 0.0 : mn_midi_freq(e->midi + transpose, a4), gfrom = 0;
            if (tr->glide > 0 && !e->drum) {     /* slide from the previous note when it was held to the end (tenuto ^) */
                int j = ei - 1; while (j >= 0 && tr->ev[j].beat >= e->beat - 1e-9) j--;
                if (j >= 0 && !tr->ev[j].drum && tr->ev[j].gate >= 0.999f && fabs(tr->ev[j].beat + tr->ev[j].dur - e->beat) < 1e-6 && tr->ev[j].midi != e->midi)
                    gfrom = mn_midi_freq(tr->ev[j].midi + transpose, a4);
            }
            size_t nn;
            float *v = mn_voice(&s->patch[e->patch], e->drum, freq, dur, vel, rate,
                                seed ^ ((uint64_t)ti * 1315423911u) ^ ((uint64_t)ei * 2654435761u), gfrom, tr->glide, &nn);
            if (start + nn > frames) nn = frames - start;
            float *dst = tb->d + start * 2;
            for (size_t i = 0; i < nn; i++) { dst[2 * i] += v[i] * gl; dst[2 * i + 1] += v[i] * gr; }
            free(v);
        }
        if (tr->highpass > 0) mn_filter(tb, BQ_HIGH, tr->highpass, 0.707, 0);
        if (tr->lowpass > 0) mn_filter(tb, BQ_LOW, tr->lowpass, 0.707, 0);
        if (tr->chorus > 0) mn_chorus(tb, 0.8, 7.0, tr->chorus, 3);
        if (tr->delay > 0) mn_delay(tb, tr->delay_time * 60.0 / (s->tempo[0].bpm * speed), tr->delay_fb, tr->delay, true, 0, pos, who);
        if (tr->reverb > 0) mn_reverb(tb, 0.7, 0.4, tr->reverb, 1.0, 0.012, 0, pos, who);
        float g = (float)(tr->vol * master * headroom * 1.5);
        for (size_t i = 0; i < frames * 2; i++) out->d[i] += tb->d[i] * g;
        mn_audio_free(tb);
    }
    if (normalize) {
        float pk = 0; for (size_t i = 0; i < frames * 2; i++) if (fabsf(out->d[i]) > pk) pk = fabsf(out->d[i]);
        if (pk > 1e-9f) { float g = (float)(0.89 / pk); for (size_t i = 0; i < frames * 2; i++) out->d[i] *= g; }
    }
    for (int i = 0; i < pcache.n; i++) mn_audio_free(pcache.e[i].au);
    free(pcache.e);
    mn_limit(out, 0.97, 0.004, 0.12);
    if (s->length_sec > 0) {                                           /* exact requested length: short fade-out at the cut */
        size_t fo = (size_t)(0.02 * rate); if (fo > frames) fo = frames;
        for (size_t i = 0; i < fo; i++) { float g = (float)i / (float)fo; out->d[(frames - 1 - i) * 2] *= g; out->d[(frames - 1 - i) * 2 + 1] *= g; }
    }
    if (mono) {
        MnAudio *m = mn_audio_new(frames, 1, rate, pos, who);
        for (size_t i = 0; i < frames; i++) m->d[i] = 0.5f * (out->d[2 * i] + out->d[2 * i + 1]);
        mn_audio_free(out); out = m;
    }
    return out;
}

/* ============================================================ API */

/* score source: notation string | score handle */
static MnScore *src_score(KcValue v, KcValue opts, bool *owned, KcPos pos, const char *who) {
    if (v.type == VAL_STRING) { *owned = true; return mn_score_parse_opts(v, opts, pos, who); }
    *owned = false; return mn_score_arg(v, pos, who);
}

/* render(source[, options]) -> audio */
static KcValue s_render(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_set_ip(ip);
    mn_need(n, 1, 2, pos, "render(notation | score[, options]) -> audio   (options: rate, tail, master, speed, transpose, humanize, seed, normalize, mono, solo, mute, a4)");
    bool owned; MnScore *s = src_score(a[0], n > 1 ? a[1] : kc_null(), &owned, pos, "render");
    MnAudio *r = mn_score_render(s, n > 1 ? a[1] : kc_null(), pos, "render");
    if (owned) mn_score_free(s);
    return mn_audio_wrap(r);
}

static int parse_inst_opt(KcValue o, KcPos pos, const char *who, MnPatch *p, const char *def) {
    const char *nm = mn_opt_str(o, "instrument", def, pos, who);
    int id = mn_inst_find(nm);
    if (id < 0) kc_error_at(pos, "music_note.%s: unknown instrument '%s' (see music_note.instruments())", who, nm);
    p->inst = id;
    p->atk = mn_opt_num(o, "attack", -1, pos, who); p->dec = mn_opt_num(o, "decay", -1, pos, who);
    p->sus = mn_opt_num(o, "sustain", -1, pos, who); p->rel = mn_opt_num(o, "release", -1, pos, who);
    p->vib_rate = mn_opt_num(o, "vibrato_rate", -1, pos, who); p->vib_depth = mn_opt_num(o, "vibrato_depth", -1, pos, who);
    p->detune = mn_opt_num(o, "detune", 0, pos, who);
    return id;
}

/* note(note | midi, seconds[, options]) -> audio (mono, includes the release tail) */
static KcValue s_note(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 2, 3, pos, "note(note | midi, seconds[, {instrument, velocity, rate, attack, decay, sustain, release, vibrato_rate, vibrato_depth, detune}]) -> audio");
    double midi;
    if (mn_is_num(a[0])) midi = mn_num(a[0], pos, "note", "note");
    else {
        MnNote nn; const char *e = mn_note_scan(mn_str(a[0], pos, "note", "note"), &nn);
        if (!e || *e) kc_error_at(pos, "music_note.note: '%s' is not a note name", a[0].as.s->data);
        midi = mn_note_midi(&nn, 4);
    }
    double secs = mn_num(a[1], pos, "note", "seconds");
    if (secs <= 0 || secs > 60) kc_error_at(pos, "music_note.note: seconds must be 0..60");
    KcValue o = n > 2 ? a[2] : kc_null();
    MnPatch p; parse_inst_opt(o, pos, "note", &p, "piano");
    if (mn_inst_is_drum(p.inst)) kc_error_at(pos, "music_note.note: use music_note.drum() for percussion");
    if (INST[p.inst].algo == A_SAMPLE) kc_error_at(pos, "music_note.note: the 'sample' instrument is only available inside scores (track name sample=NAME)");
    int rate = (int)mn_opt_int(o, "rate", MN_RATE, pos, "note");
    double vel = mn_opt_num(o, "velocity", 0.8, pos, "note");
    size_t nn;
    float *v = mn_voice(&p, 0, mn_midi_freq(midi, mn_opt_num(o, "a4", 440.0, pos, "note")), secs, vel, rate, 1, 0, 0, &nn);
    MnAudio *r = mn_audio_new(nn, 1, rate, pos, "note");
    memcpy(r->d, v, nn * sizeof(float)); free(v);
    return mn_audio_wrap(r);
}
/* drum(name[, {velocity, rate}]) -> audio */
static KcValue s_drum(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 2, pos, "drum(name[, {velocity, rate}]) -> audio   (see music_note.drums())");
    const char *nm = mn_str(a[0], pos, "drum", "name");
    int d = mn_drum_find(nm);
    if (!d) kc_error_at(pos, "music_note.drum: unknown drum '%s' (see music_note.drums())", nm);
    KcValue o = n > 1 ? a[1] : kc_null();
    int rate = (int)mn_opt_int(o, "rate", MN_RATE, pos, "drum");
    size_t nn; MnPatch p; memset(&p, 0, sizeof p);
    float *v = mn_voice(&p, d, 0, 0, mn_opt_num(o, "velocity", 0.9, pos, "drum"), rate, 7, 0, 0, &nn);
    MnAudio *r = mn_audio_new(nn, 1, rate, pos, "drum");
    memcpy(r->d, v, nn * sizeof(float)); free(v);
    return mn_audio_wrap(r);
}
static KcValue s_instruments(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    KcValue out = kc_array_new();
    for (int i = 0; i < NINST; i++) if (INST[i].algo != A_SAMPLE) kc_array_push(out, kc_string(INST[i].name));
    return out;
}
static KcValue s_drums(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    KcValue out = kc_array_new();
    for (int i = 0; i < NDRUMS; i++) kc_array_push(out, kc_string(DRUMS[i].name));
    return out;
}

static int wave_from(const char *s) {
    if (!strcasecmp(s, "sine") || !strcasecmp(s, "sin")) return W_SINE;
    if (!strcasecmp(s, "square")) return W_SQUARE;
    if (!strcasecmp(s, "saw") || !strcasecmp(s, "sawtooth")) return W_SAW;
    if (!strcasecmp(s, "triangle") || !strcasecmp(s, "tri")) return W_TRI;
    if (!strcasecmp(s, "pulse")) return W_PULSE;
    return -1;
}

/* tone(wave, freq, seconds[, {amp, rate, channels, duty, phase, harmonics[], fade_in, fade_out}]) -> audio */
static KcValue s_tone(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 3, 4, pos, "tone(wave, freq, seconds[, {amp, rate, channels, duty, phase, harmonics[], fade_in, fade_out}]) -> audio   (wave: sine square saw triangle pulse)");
    int w = wave_from(mn_str(a[0], pos, "tone", "wave"));
    if (w < 0) kc_error_at(pos, "music_note.tone: unknown wave '%s' (sine, square, saw, triangle, pulse)", a[0].as.s->data);
    double f;
    if (mn_is_num(a[1])) f = mn_num(a[1], pos, "tone", "freq");
    else {
        MnNote tn; const char *te = mn_note_scan(mn_str(a[1], pos, "tone", "freq"), &tn);
        if (!te || *te) kc_error_at(pos, "music_note.tone: '%s' is not a frequency or note name", a[1].as.s->data);
        f = mn_midi_freq(mn_note_midi(&tn, 4), 440.0);
    }
    double secs = mn_num(a[2], pos, "tone", "seconds");
    KcValue o = n > 3 ? a[3] : kc_null();
    int rate = (int)mn_opt_int(o, "rate", MN_RATE, pos, "tone"), ch = (int)mn_opt_int(o, "channels", 1, pos, "tone");
    double amp = mn_opt_num(o, "amp", 0.5, pos, "tone"), duty = mn_opt_num(o, "duty", 0.5, pos, "tone");
    double ph0 = mn_opt_num(o, "phase", 0.0, pos, "tone"), fi = mn_opt_num(o, "fade_in", 0.002, pos, "tone"), fo = mn_opt_num(o, "fade_out", 0.002, pos, "tone");
    if (f <= 0 || f >= rate / 2.0) kc_error_at(pos, "music_note.tone: frequency must be between 0 and half the sample rate");
    if (secs <= 0 || secs > 3600) kc_error_at(pos, "music_note.tone: seconds must be > 0");
    if (duty <= 0.01 || duty >= 0.99) duty = 0.5;
    KcValue *hv = mn_opt_get(o, "harmonics");
    MnAudio *r = mn_audio_new((size_t)(secs * rate), ch, rate, pos, "tone");
    double ph = wrap1(ph0), dt = f / rate;
    for (size_t i = 0; i < r->frames; i++) {
        double y;
        if (hv && hv->type == VAL_ARRAY) {
            y = 0; double norm = 0;
            for (int k = 0; k < hv->as.arr->count; k++) {
                double ak = mn_num(hv->as.arr->items[k], pos, "tone", "harmonics[]");
                if ((k + 1) * f < rate / 2.0) y += ak * sin(MN_TAU * ph * (k + 1));
                norm += fabs(ak);
            }
            if (norm > 0) y /= norm;
        } else y = osc((Wave)w, ph, dt, duty);
        double t = (double)i / rate, e = 1.0;
        if (fi > 0 && t < fi) e = t / fi;
        double tr = secs - t; if (fo > 0 && tr < fo) e *= tr / fo;
        for (int c = 0; c < ch; c++) r->d[i * ch + c] = (float)(y * amp * e);
        ph = wrap1(ph + dt);
    }
    return mn_audio_wrap(r);
}

/* noise(kind, seconds[, {amp, rate, channels, seed}]) -> audio   kinds: white pink brown blue violet */
static KcValue s_noise(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 2, 3, pos, "noise(kind, seconds[, {amp, rate, channels, seed}]) -> audio   (kind: white pink brown blue violet)");
    const char *kind = mn_str(a[0], pos, "noise", "kind");
    double secs = mn_num(a[1], pos, "noise", "seconds");
    KcValue o = n > 2 ? a[2] : kc_null();
    int rate = (int)mn_opt_int(o, "rate", MN_RATE, pos, "noise"), ch = (int)mn_opt_int(o, "channels", 1, pos, "noise");
    double amp = mn_opt_num(o, "amp", 0.3, pos, "noise");
    if (secs <= 0 || secs > 3600) kc_error_at(pos, "music_note.noise: seconds must be > 0");
    int k = !strcasecmp(kind, "white") ? 0 : !strcasecmp(kind, "pink") ? 1 : !strcasecmp(kind, "brown") || !strcasecmp(kind, "red") ? 2 : !strcasecmp(kind, "blue") ? 3 : !strcasecmp(kind, "violet") ? 4 : -1;
    if (k < 0) kc_error_at(pos, "music_note.noise: unknown noise '%s' (white, pink, brown, blue, violet)", kind);
    MnAudio *r = mn_audio_new((size_t)(secs * rate), ch, rate, pos, "noise");
    for (int c = 0; c < ch; c++) {
        MnRng rng = { (uint64_t)mn_opt_int(o, "seed", 1, pos, "noise") * 2654435761ull + (uint64_t)c * 97 + 5 };
        double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, brown = 0, prev = 0, prev2 = 0;
        for (size_t i = 0; i < r->frames; i++) {
            double w = mn_rng_f(&rng), y = w;
            if (k == 1) {   /* Paul Kellet's refined pink filter */
                b0 = 0.99886 * b0 + w * 0.0555179; b1 = 0.99332 * b1 + w * 0.0750759; b2 = 0.96900 * b2 + w * 0.1538520;
                b3 = 0.86650 * b3 + w * 0.3104856; b4 = 0.55000 * b4 + w * 0.5329522; b5 = -0.7616 * b5 - w * 0.0168980;
                y = (b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362) * 0.11; b6 = w * 0.115926;
            } else if (k == 2) { brown = (brown + 0.02 * w) / 1.02; y = brown * 3.5; }
            else if (k == 3) { y = (w - prev) * 0.5; prev = w; }
            else if (k == 4) { y = (w - 2 * prev + prev2) * 0.25; prev2 = prev; prev = w; }
            r->d[i * ch + c] = (float)(y * amp);
        }
    }
    return mn_audio_wrap(r);
}

/* sweep(f0, f1, seconds[, {wave, curve:"exp"|"linear", amp, rate, channels}]) -> audio */
static KcValue s_sweep(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 3, 4, pos, "sweep(f0, f1, seconds[, {wave, curve, amp, rate, channels}]) -> audio");
    double f0 = mn_num(a[0], pos, "sweep", "f0"), f1 = mn_num(a[1], pos, "sweep", "f1"), secs = mn_num(a[2], pos, "sweep", "seconds");
    KcValue o = n > 3 ? a[3] : kc_null();
    int rate = (int)mn_opt_int(o, "rate", MN_RATE, pos, "sweep"), ch = (int)mn_opt_int(o, "channels", 1, pos, "sweep");
    double amp = mn_opt_num(o, "amp", 0.5, pos, "sweep");
    int w = wave_from(mn_opt_str(o, "wave", "sine", pos, "sweep"));
    bool expc = strcasecmp(mn_opt_str(o, "curve", "exp", pos, "sweep"), "linear") != 0;
    if (w < 0) kc_error_at(pos, "music_note.sweep: unknown wave");
    if (f0 <= 0 || f1 <= 0 || secs <= 0) kc_error_at(pos, "music_note.sweep: frequencies and seconds must be > 0");
    MnAudio *r = mn_audio_new((size_t)(secs * rate), ch, rate, pos, "sweep");
    double ph = 0;
    for (size_t i = 0; i < r->frames; i++) {
        double u = (double)i / (double)r->frames;
        double f = expc ? f0 * pow(f1 / f0, u) : f0 + (f1 - f0) * u;
        if (f > rate * 0.49) f = rate * 0.49;
        double dt = f / rate; ph = wrap1(ph + dt);
        double e = i < 100 ? i / 100.0 : 1.0;
        for (int c = 0; c < ch; c++) r->d[i * ch + c] = (float)(osc((Wave)w, ph, dt, 0.5) * amp * e);
    }
    return mn_audio_wrap(r);
}
static KcValue s_silence(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 3, pos, "silence(seconds[, rate = 44100[, channels = 1]]) -> audio");
    double secs = mn_num(a[0], pos, "silence", "seconds");
    if (secs < 0 || secs > 3600) kc_error_at(pos, "music_note.silence: seconds must be 0..3600");
    return mn_audio_wrap(mn_audio_new((size_t)(secs * (n > 1 ? mn_int(a[1], pos, "silence", "rate") : MN_RATE)),
        n > 2 ? (int)mn_int(a[2], pos, "silence", "channels") : 1, n > 1 ? (int)mn_int(a[1], pos, "silence", "rate") : MN_RATE, pos, "silence"));
}
/* click_track(bpm, beats[, {accent_every = 4, rate}]) -> audio — a metronome */
static KcValue s_click_track(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 2, 3, pos, "click_track(bpm, beats[, {accent_every = 4, rate}]) -> audio");
    double bpm = mn_num(a[0], pos, "click_track", "bpm"); int beats = (int)mn_int(a[1], pos, "click_track", "beats");
    KcValue o = n > 2 ? a[2] : kc_null();
    int rate = (int)mn_opt_int(o, "rate", MN_RATE, pos, "click_track"), acc = (int)mn_opt_int(o, "accent_every", 4, pos, "click_track");
    if (bpm < 10 || bpm > 400 || beats < 1 || beats > 100000) kc_error_at(pos, "music_note.click_track: bpm must be 10..400 and beats 1..100000");
    double spb = 60.0 / bpm;
    MnAudio *r = mn_audio_new((size_t)(spb * beats * rate) + (size_t)(0.05 * rate), 1, rate, pos, "click_track");
    for (int b = 0; b < beats; b++) {
        size_t s0 = (size_t)(b * spb * rate); double f = (acc > 0 && b % acc == 0) ? 1600.0 : 1000.0;
        for (size_t i = 0; i < (size_t)(0.03 * rate) && s0 + i < r->frames; i++) {
            double t = (double)i / rate; r->d[s0 + i] += (float)(sin(MN_TAU * f * t) * exp(-t / 0.008) * 0.7);
        }
    }
    return mn_audio_wrap(r);
}

KcModuleEntry mn_synth_entries[] = {
    { "render", s_render }, { "note", s_note }, { "drum", s_drum }, { "instruments", s_instruments }, { "drums", s_drums },
    { "tone", s_tone }, { "noise", s_noise }, { "sweep", s_sweep }, { "silence", s_silence }, { "click_track", s_click_track },
};
const int mn_synth_count = (int)(sizeof mn_synth_entries / sizeof mn_synth_entries[0]);
